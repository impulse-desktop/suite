#!/usr/bin/env python3

"""Renders the player's video shaders into their variants and the tables
the player matches frames against.

video.py builds a variant's shader as simplified scalar expressions
(ir.py) and prints it as GLSL; tables.py holds the storage layouts and
the codes of H.273.

  video_shaders.py parts
  video_shaders.py codes HEADER
  video_shaders.py compile LAYOUT HEADER GLSLANG
  video_shaders.py spirv PART SOURCE LAYOUT_HEADER...
  video_shaders.py msl LAYOUT HEADER GLSLANG SPIRV_CROSS
  video_shaders.py metal PART SOURCE LAYOUT_HEADER...
"""

import subprocess
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import video


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


def tables():
    import tables as source

    names = ("layouts", "systems", "shapes", "chains", "matrices", "transfers", "primaries", "outputs", "ranges", "locations")
    return SimpleNamespace(**{name: getattr(source, name.upper()) for name in names})


def render(variant, facts=None):
    layout, system, transfer, conversion, output = variant
    return video.shader(layouts(tables())[layout], system, transfer, conversion, output, facts)


def layouts(module):
    every = {}
    for name, entry in module.layouts.items():
        for suffix in entry.get("endians", [""]):
            layout = {key: value for key, value in entry.items() if key not in ("endians", "formats", "fields")}
            layout["flags"] = (["be"] if suffix == "be" else []) + entry.get("flags", [])
            named = lambda text: [format + suffix for format in text.split()]
            layout["members"] = [(named(entry["formats"]), entry["components"])]
            for formats, (shift, depth) in entry.get("fields", {}).items():
                if any(component[3] or component[4] < 16 for component in entry["components"]):
                    raise ValueError(f"{name} narrows a component that is not a whole word")
                layout["members"].append((named(formats), [[*component[:3], shift, depth] for component in entry["components"]]))
            every[name + suffix] = layout
    return every


def segments(piece):
    return [piece[0], piece[0], -1] if len(piece) == 1 else piece


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
        if step % 4 == 0:
            inside = start % 4 + size <= 4
        elif step in (1, 2):
            inside = start + size <= step
        else:
            inside = all((x * step + start) % 4 + size <= 4 for x in range(4))
        if start < 0 or not inside:
            raise ValueError(f"component {c} of {name} straddles a word")


def variants():
    module = tables()
    every = []
    for name, layout in layouts(module).items():
        check(name, layout)
        for system in module.systems.get(layout["model"], [layout["model"]]):
            for transfer in module.shapes.get(system, list(module.chains)):
                for conversion, output in module.chains[transfer]:
                    every.append((name, system, transfer, conversion, output))
    return every


PARTS = 16


def parts():
    names = list(layouts(tables()))
    return [names[part::PARTS] for part in range(PARTS)]


def part_variants(part):
    names = parts()[part]
    return [variant for variant in variants() if variant[0] in names]


def symbol(variant):
    return "video_" + "_".join(variant)


def spirv(variant, glslang, directory, variable=None):
    source = Path(directory) / f"{symbol(variant)}.frag"
    source.write_text(render(variant), encoding="utf-8")
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


def compile_layout(layout, header, glslang):
    lines = []
    with tempfile.TemporaryDirectory(prefix="video-shader-") as directory:
        for variant in variants():
            if variant[0] == layout:
                part = spirv(variant, glslang, directory, f"{symbol(variant)}_spv")
                lines += unpragma(part.read_text(encoding="utf-8")) + [""]
    Path(header).write_text("\n".join(lines), encoding="utf-8")


def msl_layout(layout, header, glslang, spirv_cross):
    lines = []
    with tempfile.TemporaryDirectory(prefix="video-shader-") as directory:
        for variant in variants():
            if variant[0] != layout:
                continue
            module = spirv(variant, glslang, directory)
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


def part_source(part, source, headers, suffix, size):
    found = {Path(header).name: Path(header) for header in headers}
    lines = ["#include <video_codes.h>", ""]
    for layout in parts()[part]:
        lines += unpragma(found[f"video_{layout}.{suffix}.h"].read_text(encoding="utf-8")) + [""]
    every = part_variants(int(part))
    lines.append(f"const VideoShaderCode videoShaders{part}[{len(every)}] = {{")
    for variant in every:
        layout, system, transfer, conversion, output = variant
        name = symbol(variant) + "_" + suffix
        lines.append(f'    {{"{layout}", "{system}", "{transfer}", "{conversion}", "{output}", {name}, {size(name)}}},')
    Path(source).write_text("\n".join(lines + ["};", ""]), encoding="utf-8")


def matrix(rows):
    return "{" + ", ".join("{" + ", ".join(f"{value:.12g}" for value in row) + "}" for row in rows) + "}"


def numbers(values):
    return "{" + ", ".join(f"{value:.12g}" for value in values) + "}"


def codes(header):
    module = tables()
    lines = list(HEADER)
    lines += [
        "struct VideoLayout {",
        "    const char* name;",
        "    const char* shape;",
        "    const char* model;",
        "    bool bigEndian;",
        "    bool alpha;",
        "    bool floating;",
        "    bool bits;",
        "    bool inverted;",
        "    int count;",
        "    int components[4][5];",
        "    int padding[4];",
        "};",
        "",
        "static constexpr VideoLayout videoLayouts[] = {",
    ]
    members = [(name, layout, formats, components) for name, layout in layouts(module).items() for formats, components in layout["members"]]
    for name, layout, formats, components in members:
        flags = ", ".join("true" if flag in layout["flags"] else "false" for flag in ("be", "alpha", "float", "bits"))
        flags += ", true" if layout.get("inverted") else ", false"
        fields = ", ".join("{" + ", ".join(str(value) for value in component) + "}" for component in components)
        padding = ", ".join(str(component[3] - whole[3]) for component, whole in zip(components, layout["components"]))
        lines.append(f'    {{"{formats[0]}", "{name}", "{layout["model"]}", {flags}, {len(components)}, {{{fields}}}, {{{padding}}}}},')
    lines += ["};", ""]
    lines += ["struct VideoFormat {", "    const char* name;", "    int layout;", "};", "", "static constexpr VideoFormat videoFormats[] = {"]
    for index, (name, layout, formats, components) in enumerate(members):
        lines += [f'    {{"{format}", {index}}},' for format in formats]
    lines += ["};", ""]
    lines += [
        "struct VideoMatrix {",
        "    uint8_t code;",
        "    const char* system;",
        "    const char* weights;",
        "    double kr;",
        "    double kb;",
        "    double toSignal[3][3];",
        "    int lumaBits;",
        "};",
        "",
        "static constexpr VideoMatrix videoMatrices[] = {",
    ]
    for code, entry in module.matrices.items():
        weights = entry.get("weights", "fixed" if "toSignal" in entry else "none")
        kr, kb = weights if isinstance(weights, list) else (0, 0)
        kind = "given" if isinstance(weights, list) else weights
        rows = entry.get("toSignal", [[0, 0, 0]] * 3)
        lines.append(f'    {{{code}, "{entry["system"]}", "{kind}", {kr}, {kb}, {matrix(rows)}, {entry.get("lumaBits", 0)}}},')
    lines += ["};", ""]
    lines += [
        "struct VideoTransfer {",
        "    uint8_t code;",
        "    const char* shape;",
        "    double eotf[11];",
        "    double oetf[11];",
        "    double inverse[11];",
        "    double decades;",
        "};",
        "",
        "static constexpr VideoTransfer videoTransfers[] = {",
    ]
    flat = lambda piece: numbers([*segments(piece)[0], *segments(piece)[1], segments(piece)[2]]) if piece else numbers([0] * 11)
    for code, entry in module.transfers.items():
        lines.append(f'    {{{code}, "{entry["shape"]}", {flat(entry.get("eotf"))}, {flat(entry.get("oetf"))}, {flat(entry.get("inverse"))}, {entry.get("decades", 0)}}},')
    lines += ["};", ""]
    lines += ["struct VideoPrimaries {", "    uint8_t code;", "    double toXyz[3][3];", "};", "", "static constexpr VideoPrimaries videoPrimaries[] = {"]
    for code, chromaticities in module.primaries.items():
        lines.append(f"    {{{code}, {matrix(to_xyz(chromaticities))}}},")
    lines += ["};", ""]
    lines += ["struct VideoOutput {", "    const char* name;", "    double fromXyz[3][3];", "};", "", "static constexpr VideoOutput videoOutputs[] = {"]
    for name, code in module.outputs.items():
        lines.append(f'    {{"{name}", {matrix(inverse(to_xyz(module.primaries[code])))}}},')
    lines += ["};", ""]
    lines += ["struct VideoLocation {", "    uint8_t code;", "    double site[2];", "};", "", "static constexpr VideoLocation videoLocations[] = {"]
    for code, site in module.locations.items():
        lines.append(f"    {{{code}, {numbers(site)}}},")
    lines += ["};", ""]
    lines += [f"static constexpr uint8_t videoRanges[] = {{{', '.join(str(value) for value in module.ranges)}}};", ""]
    lines += [
        "struct VideoShaderCode {",
        "    const char* layout;",
        "    const char* system;",
        "    const char* transfer;",
        "    const char* conversion;",
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
    counts = [len(part_variants(part)) for part in range(PARTS)]
    lines += [f"extern const VideoShaderCode videoShaders{part}[{count}];" for part, count in enumerate(counts)]
    lines += ["", "static constexpr VideoShaderPart videoShaderParts[] = {"]
    lines += [f"    {{videoShaders{part}, {count}}}," for part, count in enumerate(counts)]
    lines += ["};", ""]
    Path(header).write_text("\n".join(lines), encoding="utf-8")


def main():
    args = sys.argv[1:]
    if len(args) == 1 and args[0] == "parts":
        print("\n".join(" ".join(layouts) for layouts in parts()))
    elif len(args) == 2 and args[0] == "codes":
        codes(args[1])
    elif len(args) == 4 and args[0] == "compile":
        compile_layout(args[1], args[2], args[3])
    elif len(args) >= 3 and args[0] == "spirv":
        part_source(int(args[1]), args[2], args[3:], "spv", lambda name: f"sizeof({name})")
    elif len(args) == 5 and args[0] == "msl":
        msl_layout(args[1], args[2], args[3], args[4])
    elif len(args) >= 3 and args[0] == "metal":
        part_source(int(args[1]), args[2], args[3:], "msl", lambda name: f"sizeof({name}) - 1")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
