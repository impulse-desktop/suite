#!/usr/bin/env python3

"""Renders video.frag, a Jinja template, into its shader variants.

  video_shaders.py compile TEMPLATE FETCH TRANSFER OUTPUT HEADER GLSLANG
  video_shaders.py codes TEMPLATE HEADER
  video_shaders.py spirv TEMPLATE HEADER VARIANT_HEADER...
  video_shaders.py metal TEMPLATE HEADER GLSLANG SPIRV_CROSS
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
    return env.get_template(Path(template).name)


def tables(template):
    return environment(template).make_module({"fetch": "", "transfer": "", "output": ""})


def variants(template):
    module = tables(template)
    return [(f, t, o) for f in module.fetches for t in module.transfers for o in module.outputs]


def name(variant):
    return "video_" + "_".join(variant)


def spirv(template, variant, glslang, directory, header=None):
    fetch, transfer, output = variant
    source = Path(directory) / f"{name(variant)}.frag"
    source.write_text(environment(template).render(fetch=fetch, transfer=transfer, output=output), encoding="utf-8")
    command = [glslang, "--quiet", "--target-env", "vulkan1.1", "-V", "-S", "frag"]
    if header:
        command += ["--variable-name", f"{name(variant)}_spv", "-o", str(header)]
    else:
        command += ["-o", str(source.with_suffix(".spv"))]
    subprocess.run([*command, str(source)], check=True)
    return source.with_suffix(".spv")


HEADER = ["#pragma once", "", "#include <stddef.h>", "#include <stdint.h>", ""]


def table(variants, symbol, size):
    lines = [
        "struct VideoShaderCode {",
        "    const char* fetch;",
        "    const char* transfer;",
        "    const char* output;",
        "    const void* code;",
        "    size_t size;",
        "};",
        "",
        "static constexpr VideoShaderCode videoShaders[] = {",
    ]
    for variant in variants:
        fetch, transfer, output = variant
        lines.append(f'    {{"{fetch}", "{transfer}", "{output}", {symbol(variant)}, {size(variant)}}},')
    return lines + ["};", ""]


def codes(template, header):
    module = tables(template)
    lines = HEADER + ["struct VideoTransferCode {", "    uint8_t code;", "    const char* kind;", "};", ""]
    lines.append("static constexpr VideoTransferCode videoTransfers[] = {")
    for transfer, values in module.transfers.items():
        lines += [f'    {{{value}, "{transfer}"}},' for value in values]
    lines += ["};", ""]
    for symbol, values in (
        ("videoMatrices", module.matrices),
        ("videoPrimaries", list(module.primaries)),
        ("videoRanges", module.ranges),
        ("videoLocations", module.locations),
    ):
        lines += [f"static constexpr uint8_t {symbol}[] = {{{', '.join(str(value) for value in values)}}};", ""]
    Path(header).write_text("\n".join(lines), encoding="utf-8")


def spirv_table(template, header, parts):
    every = variants(template)
    found = {Path(part).name: Path(part) for part in parts}
    lines = list(HEADER)
    for variant in every:
        text = found[f"{name(variant)}.spv.h"].read_text(encoding="utf-8")
        lines += [line for line in text.splitlines() if line.strip() != "#pragma once"] + [""]
    lines += table(every, lambda v: f"{name(v)}_spv", lambda v: f"sizeof({name(v)}_spv)")
    Path(header).write_text("\n".join(lines), encoding="utf-8")


def metal(template, header, glslang, spirv_cross):
    every = variants(template)
    lines = list(HEADER)
    with tempfile.TemporaryDirectory(prefix="video-shader-") as directory:
        for variant in every:
            module = spirv(template, variant, glslang, directory)
            msl = module.with_suffix(".metal")
            subprocess.run(
                [spirv_cross, str(module), "--msl", "--msl-version", "20100", "--msl-decoration-binding", "--output", str(msl)],
                check=True,
            )
            text = msl.read_text(encoding="utf-8")
            if ")VIDEO_MSL" in text:
                raise ValueError("a Metal shader contains its raw string delimiter")
            lines += [f'static constexpr char {name(variant)}_msl[] = R"VIDEO_MSL(', text.rstrip(), ')VIDEO_MSL";', ""]
    lines += table(every, lambda v: f"{name(v)}_msl", lambda v: f"sizeof({name(v)}_msl) - 1")
    Path(header).write_text("\n".join(lines), encoding="utf-8")


def main():
    args = sys.argv[1:]
    if len(args) == 7 and args[0] == "compile":
        with tempfile.TemporaryDirectory(prefix="video-shader-") as directory:
            spirv(args[1], (args[2], args[3], args[4]), args[6], directory, header=args[5])
    elif len(args) == 3 and args[0] == "codes":
        codes(args[1], args[2])
    elif len(args) >= 3 and args[0] == "spirv":
        spirv_table(args[1], args[2], args[3:])
    elif len(args) == 5 and args[0] == "metal":
        metal(args[1], args[2], args[3], args[4])
    elif len(args) == 2 and args[0] == "variants":
        print("\n".join(" ".join(variant) for variant in variants(args[1])))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
