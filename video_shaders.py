#!/usr/bin/env python3

"""Renders video.frag, a Jinja template, into its shader variants.

  video_shaders.py parts TEMPLATE
  video_shaders.py codes TEMPLATE HEADER
  video_shaders.py compile TEMPLATE LAYOUT HEADER GLSLANG
  video_shaders.py spirv TEMPLATE PART SOURCE LAYOUT_HEADER...
  video_shaders.py msl TEMPLATE LAYOUT HEADER GLSLANG SPIRV_CROSS
  video_shaders.py metal TEMPLATE PART SOURCE LAYOUT_HEADER...
"""

import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent / "ext"))

import jinja2


def chromaticity(x, y):
    return [x / y, 1.0, (1.0 - x - y) / y]


def product(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def inverse(m):
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [
        [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
        [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
        [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det],
    ]


def diagonal(v):
    return [[v[0], 0.0, 0.0], [0.0, v[1], 0.0], [0.0, 0.0, v[2]]]


def apply(m, v):
    return [sum(m[i][k] * v[k] for k in range(3)) for i in range(3)]


BRADFORD = [[0.8951, 0.2664, -0.1614], [-0.7502, 1.7135, 0.0367], [0.0389, -0.0685, 1.0296]]
D65 = (0.3127, 0.3290)


def to_xyz(primaries):
    if primaries == "xyz":
        return diagonal([1.0, 1.0, 1.0])
    rx, ry, gx, gy, bx, by, wx, wy = primaries
    columns = [chromaticity(rx, ry), chromaticity(gx, gy), chromaticity(bx, by)]
    matrix = [[columns[j][i] for j in range(3)] for i in range(3)]
    scale = apply(inverse(matrix), chromaticity(wx, wy))
    matrix = product(matrix, diagonal(scale))
    if (wx, wy) != D65:
        source = apply(BRADFORD, chromaticity(wx, wy))
        target = apply(BRADFORD, chromaticity(*D65))
        adapt = product(inverse(BRADFORD), product(diagonal([t / s for t, s in zip(target, source)]), BRADFORD))
        matrix = product(adapt, matrix)
    return matrix


def mat3(matrix):
    columns = [matrix[i][j] for j in range(3) for i in range(3)]
    return "mat3(" + ", ".join(f"{value:.10f}" for value in columns) + ")"


def environment(template):
    env = jinja2.Environment(
        loader=jinja2.FileSystemLoader(str(Path(template).resolve().parent)),
        undefined=jinja2.StrictUndefined,
        keep_trailing_newline=True,
    )
    env.globals["rgb_to_xyz"] = lambda primaries: mat3(to_xyz(primaries))
    env.globals["xyz_to_rgb"] = lambda primaries: mat3(inverse(to_xyz(primaries)))
    env.globals["inverse_matrix"] = lambda rows, scale: mat3(inverse([[value / scale for value in row] for row in rows]))
    return env.get_template(Path(template).name)


def tables(template):
    return environment(template).make_module({"layout": "gray", "color": "gray", "transfer": "power", "output": "sdr"})


def window(shift, depth):
    return 1 if shift + depth <= 8 else 2 if shift + depth <= 16 else 4


def check(name, layout):
    if layout["model"] in ("bayer", "palette") or "bits" in layout["flags"]:
        return
    for c, (plane, step, offset, shift, depth) in enumerate(layout["components"]):
        if c == 0 and "luma" in layout:
            continue
        size = window(shift, depth)
        start = offset + (1 if "be" in layout["flags"] and size == 1 else 0)
        for x in range(4):
            byte = x * step + start
            if start < 0 or (4 % step == 0 and start + size > step) or byte % 4 + size > 4:
                raise ValueError(f"component {c} of {name} straddles a word")


def variants(template):
    module = tables(template)
    every = []
    for name, layout in module.layouts.items():
        check(name, layout)
        for color in module.models[layout["model"]]:
            for transfer in module.signals.get(color, list(module.transfers)):
                for output in module.outputs:
                    every.append((name, color, transfer, output))
    return every


PARTS = 16


def parts(template):
    layouts = list(tables(template).layouts)
    return [layouts[part::PARTS] for part in range(PARTS)]


def part_variants(template, part):
    layouts = parts(template)[part]
    return [variant for variant in variants(template) if variant[0] in layouts]


def symbol(variant):
    return "video_" + "_".join(variant)


def spirv(template, variant, glslang, directory, variable=None):
    layout, color, transfer, output = variant
    source = Path(directory) / f"{symbol(variant)}.frag"
    source.write_text(environment(template).render(layout=layout, color=color, transfer=transfer, output=output), encoding="utf-8")
    command = [glslang, "--quiet", "--target-env", "vulkan1.1", "-V", "-S", "frag"]
    if variable:
        command += ["--variable-name", variable, "-o", str(source.with_suffix(".h"))]
    else:
        command += ["-o", str(source.with_suffix(".spv"))]
    subprocess.run([*command, str(source)], check=True)
    return source.with_suffix(".h" if variable else ".spv")


HEADER = ["#pragma once", "", "#include <stddef.h>", "#include <stdint.h>", ""]


def unpragma(text):
    return [line for line in text.splitlines() if line.strip() != "#pragma once"]


def compile_layout(template, layout, header, glslang):
    lines = []
    with tempfile.TemporaryDirectory(prefix="video-shader-") as directory:
        for variant in variants(template):
            if variant[0] == layout:
                part = spirv(template, variant, glslang, directory, f"{symbol(variant)}_spv")
                lines += unpragma(part.read_text(encoding="utf-8")) + [""]
    Path(header).write_text("\n".join(lines), encoding="utf-8")


def msl_layout(template, layout, header, glslang, spirv_cross):
    lines = []
    with tempfile.TemporaryDirectory(prefix="video-shader-") as directory:
        for variant in variants(template):
            if variant[0] != layout:
                continue
            module = spirv(template, variant, glslang, directory)
            source = module.with_suffix(".metal")
            subprocess.run(
                [spirv_cross, str(module), "--msl", "--msl-version", "20100", "--msl-decoration-binding", "--output", str(source)],
                check=True,
            )
            text = source.read_text(encoding="utf-8")
            if ")VIDEO_MSL" in text:
                raise ValueError("a Metal shader contains its raw string delimiter")
            lines += [f'static constexpr char {symbol(variant)}_msl[] = R"VIDEO_MSL(', text.rstrip(), ')VIDEO_MSL";', ""]
    Path(header).write_text("\n".join(lines), encoding="utf-8")


def part_source(template, part, source, headers, suffix, size):
    found = {Path(header).name: Path(header) for header in headers}
    lines = ["#include <video_codes.h>", ""]
    for layout in parts(template)[part]:
        lines += unpragma(found[f"video_{layout}.{suffix}.h"].read_text(encoding="utf-8")) + [""]
    every = part_variants(template, int(part))
    lines.append(f"const VideoShaderCode videoShaders{part}[{len(every)}] = {{")
    for variant in every:
        layout, color, transfer, output = variant
        name = symbol(variant) + "_" + suffix
        lines.append(f'    {{"{layout}", "{color}", "{transfer}", "{output}", {name}, {size(name)}}},')
    Path(source).write_text("\n".join(lines + ["};", ""]), encoding="utf-8")


def codes(template, header):
    module = tables(template)
    lines = list(HEADER)
    lines += [
        "struct VideoLayout {",
        "    const char* name;",
        "    const char* model;",
        "    bool bigEndian;",
        "    bool alpha;",
        "    bool floating;",
        "    bool bits;",
        "    int count;",
        "    int components[4][5];",
        "};",
        "",
        "static constexpr VideoLayout videoLayouts[] = {",
    ]
    for name, layout in module.layouts.items():
        flags = ", ".join("true" if flag in layout["flags"] else "false" for flag in ("be", "alpha", "float", "bits"))
        components = ", ".join("{" + ", ".join(str(value) for value in component) + "}" for component in layout["components"])
        lines.append(f'    {{"{name}", "{layout["model"]}", {flags}, {len(layout["components"])}, {{{components}}}}},')
    lines += ["};", ""]
    lines += ["struct VideoFormat {", "    const char* name;", "    int layout;", "};", "", "static constexpr VideoFormat videoFormats[] = {"]
    for index, layout in enumerate(module.layouts.values()):
        lines += [f'    {{"{name}", {index}}},' for name in layout["formats"]]
    lines += ["};", ""]
    lines += ["struct VideoCode {", "    uint8_t code;", "    const char* kind;", "};", ""]
    for symbol_name, kinds in (("videoMatrices", module.matrices), ("videoTransfers", module.transfers)):
        lines.append(f"static constexpr VideoCode {symbol_name}[] = {{")
        for kind, values in kinds.items():
            lines += [f'    {{{value}, "{kind}"}},' for value in values]
        lines += ["};", ""]
    for symbol_name, values in (
        ("videoPrimaries", list(module.primaries)),
        ("videoRanges", module.ranges),
        ("videoLocations", module.locations),
    ):
        lines += [f"static constexpr uint8_t {symbol_name}[] = {{{', '.join(str(value) for value in values)}}};", ""]
    lines += [
        "struct VideoShaderCode {",
        "    const char* layout;",
        "    const char* color;",
        "    const char* transfer;",
        "    const char* output;",
        "    const void* code;",
        "    size_t size;",
        "};",
        "",
        "struct VideoShaderPart {",
        "    const VideoShaderCode* codes;",
        "    size_t count;",
        "};",
        "",
    ]
    counts = [len(part_variants(template, part)) for part in range(PARTS)]
    lines += [f"extern const VideoShaderCode videoShaders{part}[{count}];" for part, count in enumerate(counts)]
    lines += ["", "static constexpr VideoShaderPart videoShaderParts[] = {"]
    lines += [f"    {{videoShaders{part}, {count}}}," for part, count in enumerate(counts)]
    lines += ["};", ""]
    Path(header).write_text("\n".join(lines), encoding="utf-8")


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "parts":
        print("\n".join(" ".join(layouts) for layouts in parts(args[1])))
    elif len(args) == 3 and args[0] == "codes":
        codes(args[1], args[2])
    elif len(args) == 5 and args[0] == "compile":
        compile_layout(args[1], args[2], args[3], args[4])
    elif len(args) >= 4 and args[0] == "spirv":
        part_source(args[1], int(args[2]), args[3], args[4:], "spv", lambda name: f"sizeof({name})")
    elif len(args) == 6 and args[0] == "msl":
        msl_layout(args[1], args[2], args[3], args[4], args[5])
    elif len(args) >= 4 and args[0] == "metal":
        part_source(args[1], int(args[2]), args[3], args[4:], "msl", lambda name: f"sizeof({name}) - 1")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
