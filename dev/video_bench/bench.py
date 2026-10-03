#!/usr/bin/env python3

"""Times the video shaders of gpu/ against hand-written ones on a corpus of pipelines.

Every case renders one variant of the video templates and the hand-written shader
dev/video_bench/hand/NAME.frag over the same random frame, checks that the
two agree, and times both at 3840x2160 into R8 (arithmetic-bound) and
RGBA16F (bandwidth-bound) targets. The table gives template/hand ratios and
their geometric means; the instruction and memory counts come from the
driver through VK_KHR_pipeline_executable_properties.

  ix run set/pg/libs lib/vulkan/drivers --vulkan=amd/radv -- python3 dev/video_bench/bench.py [--runs N] [--work DIR] [CASE...]
"""

import argparse
import math
import os
import random
import re
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SUITE = HERE.parent.parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(SUITE / "gpu"))

import video_shaders

W, H = 1920, 1080
WHITE = 203.0
DRAWS = 200

CORPUS = [
    ("common", "yuv420p_709_sdr", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr"),
    ("common", "yuv420p_601_sdr", "yuv420p", (1, 1), 6, 1, 6, 6, 1, "sdr"),
    ("common", "yuvj420p_601_sdr", "yuvj420p", (1, 1), 5, 2, 1, 1, 2, "sdr"),
    ("common", "nv12_709_sdr", "nv12", (1, 1), 1, 1, 1, 1, 1, "sdr"),
    ("common", "yuv420p10_pq_hdr", "yuv420p10le", (1, 1), 9, 1, 16, 9, 1, "hdr"),
    ("common", "p010_pq_hdr", "p010le", (1, 1), 9, 1, 16, 9, 1, "hdr"),
    ("common", "yuv420p10_hlg_hdr", "yuv420p10le", (1, 1), 9, 1, 18, 9, 1, "hdr"),
    ("common", "yuv422p10_709_sdr", "yuv422p10le", (1, 0), 1, 1, 1, 1, 1, "sdr"),
    ("common", "bgra_srgb_sdr", "bgra", (0, 0), 0, 2, 13, 1, 0, "sdr"),
    ("common", "pal8_srgb_sdr", "pal8", (0, 0), 0, 2, 13, 1, 0, "sdr"),
    ("hard", "bayer8_sdr", "bayer_rggb8", (0, 0), 0, 2, 1, 1, 0, "sdr"),
    ("hard", "bayer16_sdr", "bayer_rggb16le", (0, 0), 0, 2, 1, 1, 0, "sdr"),
    ("hard", "yuv420p10_cl_hdr", "yuv420p10le", (1, 1), 10, 1, 14, 9, 1, "hdr"),
    ("hard", "yuv420p10_ictcp_hdr", "yuv420p10le", (1, 1), 14, 1, 16, 9, 1, "hdr"),
    ("hard", "rgb24_srgb_sdr", "rgb24", (0, 0), 0, 2, 13, 1, 0, "sdr"),
    ("hard", "xyz12_dci_sdr", "xyz12le", (0, 0), 0, 2, 17, 10, 0, "sdr"),
    ("hard", "gray_srgb_sdr", "gray", (0, 0), 0, 2, 13, 1, 0, "sdr"),
    ("hard", "yuv444p_709_sdr", "yuv444p", (0, 0), 1, 1, 1, 1, 0, "sdr"),
    ("hard", "yuv420p_709_hdr", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "hdr"),
    ("hard", "rgb48be_srgb_sdr", "rgb48be", (0, 0), 0, 2, 13, 1, 0, "sdr"),
    ("popular", "yuyv422_601_sdr", "yuyv422", (1, 0), 6, 1, 1, 1, 0, "sdr"),
    ("popular", "uyvy422_709_sdr", "uyvy422", (1, 0), 1, 1, 1, 1, 0, "sdr"),
    ("popular", "nv21_601_full_sdr", "nv21", (1, 1), 5, 2, 1, 1, 1, "sdr"),
    ("popular", "yuvj422p_srgb_sdr", "yuvj422p", (1, 0), 5, 2, 13, 1, 2, "sdr"),
    ("popular", "yuva420p_709_sdr", "yuva420p", (1, 1), 1, 1, 1, 1, 1, "sdr"),
    ("popular", "yuv420p10_pq_sdr", "yuv420p10le", (1, 1), 9, 1, 16, 9, 1, "sdr"),
    ("popular", "yuv420p10_hlg_sdr", "yuv420p10le", (1, 1), 9, 1, 18, 9, 1, "sdr"),
    ("popular", "gbrp_srgb_sdr", "gbrp", (0, 0), 0, 2, 13, 1, 0, "sdr"),
    ("popular", "rgba64be_srgb_sdr", "rgba64be", (0, 0), 0, 2, 13, 1, 0, "sdr"),
    ("popular", "gray16be_srgb_sdr", "gray16be", (0, 0), 0, 2, 13, 1, 0, "sdr"),
]
FIELDS = ("group", "name", "format", "subsampling", "matrix", "range", "transfer", "primaries", "location", "output")

SHADERS = SUITE / "gpu"
MODULE = video_shaders.tables(SHADERS)
LAYOUTS = video_shaders.layouts(MODULE)


def layout_of(format):
    for name, layout in LAYOUTS.items():
        for formats, components in layout["members"]:
            if format in formats:
                return name, layout, components
    raise KeyError(format)


def half(value):
    return struct.unpack("<H", struct.pack("<e", value))[0]


def frame(case, layout, components):
    rng = random.Random(1)
    sx, sy = case["subsampling"]
    planes = 1 + max(component[0] for component in components)
    grids = [(W, H)] * 4
    if layout["model"] == "yuv":
        grids[1] = grids[2] = (-(-W >> sx), -(-H >> sy))
    widths, heights = [0] * planes, [0] * planes
    for c, (plane, step, offset, shift, depth) in enumerate(components):
        width, height = grids[c]
        widths[plane] = max(widths[plane], width * step)
        heights[plane] = max(heights[plane], height)
    offsets, lines, total = [], [], 0
    for plane in range(planes):
        offsets.append(total)
        lines.append((widths[plane] + 63) // 64 * 64)
        total += (lines[plane] * heights[plane] + 63) // 64 * 64
    palette = total
    data = bytearray(total + (1024 if layout["model"] == "palette" else 0) + 64)
    big = "be" in layout["flags"]
    for c, (plane, step, offset, shift, depth) in enumerate(components):
        width, height = grids[c]
        if layout["model"] == "bayer" and c:
            continue
        if layout["model"] == "bayer":
            shift, depth = 0, 8 * step
        size = 1 if shift + depth <= 8 else 2 if shift + depth <= 16 else 4
        start = offset + 1 if big and size == 1 else offset
        for y in range(height):
            row = offsets[plane] + y * lines[plane]
            for x in range(width):
                if "float" in layout["flags"]:
                    value = half(rng.random()) if depth == 16 else struct.unpack("<I", struct.pack("<f", rng.random()))[0]
                else:
                    value = rng.getrandbits(depth) << shift
                at = row + x * step + start
                old = int.from_bytes(data[at:at + size], "big" if big else "little")
                data[at:at + size] = (old | value).to_bytes(size, "big" if big else "little")
    if layout["model"] == "palette":
        data[palette:palette + 1024] = bytes(rng.getrandbits(8) for _ in range(1024))
        offsets.append(palette)
        lines.append(0)
    return bytes(data), (offsets + [0] * 4)[:4], (lines + [0] * 4)[:4]


def ycc(kr, kb):
    kg = 1 - kr - kb
    return [[1, 0, 2 * (1 - kr)], [1, -2 * kb * (1 - kb) / kg, -2 * kr * (1 - kr) / kg], [1, 2 * (1 - kb), 0]]


def piece(segments, top):
    upper, lower, threshold = video_shaders.segments(segments)
    knee = threshold >= 0 and upper[1] == 1
    curved, straight = (lower, upper) if knee else (upper, lower)
    return [*curved[:4], straight[0] * straight[2], straight[0] * straight[3] - straight[4], -1 if knee else 1, 0, curved[4], 0, threshold, top]


def columns(matrix):
    return [value for column in range(3) for value in (matrix[0][column], matrix[1][column], matrix[2][column], 0.0)]


def template(case, layout, components, offsets, lines):
    sx, sy = case["subsampling"]
    model = layout["model"]
    yuv = model == "yuv"
    matrix = MODULE.matrices.get(case["matrix"], {})
    transfer = MODULE.transfers[case["transfer"]]
    primaries = MODULE.primaries[case["primaries"]]
    to_xyz = video_shaders.to_xyz(primaries)
    to_output = video_shaders.product(video_shaders.inverse(video_shaders.to_xyz(MODULE.primaries[MODULE.outputs[case["output"]]])), to_xyz)
    same = all(abs(to_output[i][j] - (i == j)) < 1e-6 for i in range(3) for j in range(3))
    sdr = case["output"] == "sdr"
    shape, conversion, output = transfer["shape"], "same" if same else "convert", case["output"]
    unbounded = 3.4e38
    eotf = video_shaders.segments(transfer.get("eotf", [[0] * 5]))
    curve = piece(eotf, unbounded)
    power = eotf[0] == eotf[1] and eotf[0][2] == 1 and eotf[0][3] == 0 and eotf[0][4] == 0 and eotf[2] < 0
    if shape == "curve" and same and case["transfer"] == (13 if sdr else 8):
        shape, output = "identity", "any"
        curve[11] = 1.0 if sdr else unbounded
    elif shape == "curve" and same and not sdr:
        output = "any"
    elif shape == "curve" and same and power and eotf[0][1] in (1, 2.4):
        scale, gamma = eotf[0][0], eotf[0][1]
        curve = piece([[1.055 * scale ** (1 / 2.4), gamma / 2.4, 1, 0, 0.055], [12.92 * scale, gamma, 1, 0, 0], (0.0031308 / scale) ** (1 / gamma)], 1.0)
        output = "any"
    elif shape == "curve" or sdr or shape == "log":
        conversion = "convert"
    levels, scales = [0.0] * 4, [1.0] * 4
    for c, component in enumerate(components if model != "palette" and "float" not in layout["flags"] else []):
        depth = 8 * component[1] if model == "bayer" else component[4]
        alpha = "alpha" in layout["flags"] and c == len(components) - 1
        chroma = yuv and c in (1, 2) and case["matrix"] != 0
        full = depth < 8 or case["range"] == 2 or (case["range"] == 0 and not yuv)
        unit, top, slot = 2 ** (depth - 8), 2**depth - 1, 3 if alpha else c
        if alpha:
            levels[slot], scales[slot] = 0.0, top
        elif chroma:
            levels[slot], scales[slot] = (2 ** (depth - 1), top) if full else (128 * unit, 224 * unit)
        else:
            levels[slot], scales[slot] = (0, top) if full else (16 * unit, 219 * unit)
        padding = 2 ** (component[3] - layout["components"][c][3])
        levels[slot] *= padding
        scales[slot] *= padding
    kr, kb = matrix.get("weights", [0, 0]) if isinstance(matrix.get("weights"), list) else (0, 0)
    to_signal = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
    if yuv and "toSignal" in matrix:
        to_signal = matrix["toSignal"]
    elif yuv and matrix.get("system") == "linear":
        to_signal = ycc(kr, kb)
    elif model == "gray":
        to_signal = [[1, 0, 0], [1, 0, 0], [1, 0, 0]]
    decode = [[to_signal[r][c] / scales[c] for c in range(3)] for r in range(3)]
    bias = [-sum(decode[r][c] * levels[c] for c in range(3)) for r in range(3)] + [1 / scales[3]]
    site = MODULE.locations[case["location"]]
    red = 0
    if model == "bayer":
        red = case["format"][6:].index("r")
    uniform = struct.pack("<4I", *offsets) + struct.pack("<4I", *lines) + struct.pack("<4I", W, H, -(-W >> sx), -(-H >> sy))
    uniform += struct.pack("<4f", 2.0**-sx, 2.0**-sy, site[0] * ((1 << sx) - 1) * 2.0**-sx, site[1] * ((1 << sy) - 1) * 2.0**-sy)
    uniform += struct.pack("<12f", *columns(decode)) + struct.pack("<4f", *bias)
    uniform += struct.pack("<4f", red % 2, red // 2, 0, 0) + struct.pack("<4f", kr, kb, 0, 0)
    uniform += struct.pack("<12f", *curve)
    uniform += struct.pack("<12f", *piece(transfer.get("oetf", [[0] * 5]), unbounded))
    uniform += struct.pack("<12f", *piece(transfer.get("inverse", [[0] * 5]), unbounded))
    uniform += struct.pack("<12f", *columns(to_output))
    uniform += struct.pack("<4f", transfer.get("decades", 0), 10000 / WHITE, 1000 / WHITE, 0)
    uniform += struct.pack("<4f", *to_xyz[1], 0)
    system = matrix["system"] if yuv else model
    return (system, shape, conversion, output), uniform


def prepare(case, directory):
    name, layout, components = layout_of(case["format"])
    data, offsets, lines = frame(case, layout, components)
    (system, shape, conversion, output), uniform = template(case, layout, components, offsets, lines)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "data.bin").write_bytes(data)
    (directory / "template.ubo").write_bytes(uniform)
    (directory / "template.frag").write_text(video_shaders.render(SHADERS, (name, system, shape, conversion, output)))
    sx, sy = case["subsampling"]
    (directory / "optimum.ubo").write_bytes(struct.pack("<4I", *offsets) + struct.pack("<4I", *lines) + struct.pack("<4I", W, H, -(-W >> sx), -(-H >> sy)) + struct.pack("<4f", WHITE, 0, 0, 0))
    (directory / "optimum.frag").write_text((HERE / "hand" / f"{case['name']}.frag").read_text())
    for shader in ("template", "optimum"):
        subprocess.run(["glslangValidator", "--quiet", "--target-env", "vulkan1.1", "-V", "-S", "frag", str(directory / f"{shader}.frag"), "-o", str(directory / f"{shader}.spv")], check=True)
    return "_".join((name, system, shape, conversion, output))


def measure(harness, vertex, directory):
    files = [str(directory / name) for name in ("data.bin", "optimum.spv", "optimum.ubo", "template.spv", "template.ubo")]
    text = subprocess.run([str(harness), str(vertex), *files, str(DRAWS)], check=True, capture_output=True, text=True).stdout
    times = {(target, shader): float(value) for target, shader, value in re.findall(r"time (\w+) (\w+) ([\d.]+)", text)}
    stats = {shader: {key: int(value) for key, value in re.findall(r"(\w[\w ]*?)=(\d+)", rest)} for shader, rest in re.findall(r"stats (\w+) (.*)", text)}
    return float(re.search(r"difference (\S+)", text).group(1)), times, stats


def geomean(values):
    return math.exp(sum(math.log(value) for value in values) / len(values))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--work", default=str(SUITE / ".build" / "video_bench"))
    parser.add_argument("cases", nargs="*")
    args = parser.parse_args()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    harness = work / "harness"
    flags = " ".join(os.environ.get(name, "") for name in ("CPPFLAGS", "CFLAGS"))
    libraries = " ".join(os.environ.get(name, "") for name in ("CTRFLAGS", "LDFLAGS"))
    subprocess.run(f"cc {flags} -o {harness} {HERE / 'harness.c'} $(pkg-config --cflags --libs vulkan) {libraries}", shell=True, check=True)
    vertex = work / "fullscreen.vert.spv"
    subprocess.run(["glslangValidator", "--quiet", "--target-env", "vulkan1.1", "-V", str(HERE / "fullscreen.vert"), "-o", str(vertex)], check=True)
    cases = [dict(zip(FIELDS, entry)) for entry in CORPUS if not args.cases or entry[1] in args.cases]
    rows = []
    for case in cases:
        directory = work / case["name"]
        variant = prepare(case, directory)
        runs = [measure(harness, vertex, directory) for _ in range(args.runs)]
        best = {key: min(run[1][key] for run in runs) for key in runs[0][1]}
        stats = runs[0][2]
        row = dict(case, variant=variant, difference=runs[0][0], stats=stats, best=best)
        row["r8"] = best[("r8", "template")] / best[("r8", "optimum")]
        row["rgba16f"] = best[("rgba16f", "template")] / best[("rgba16f", "optimum")]
        row["instructions"] = stats["template"]["Instructions"] / stats["optimum"]["Instructions"]
        rows.append(row)
        print(
            f"{case['name']:22} r8 {row['r8']:.3f}  rgba16f {row['rgba16f']:.3f}  instructions {stats['optimum']['Instructions']}/{stats['template']['Instructions']}"
            f"  loads {stats['optimum']['VMEM']}/{stats['template']['VMEM']}  difference {row['difference']:.3g}  {variant}",
            flush=True,
        )
    groups = [("all", rows)] + [(group, [row for row in rows if row["group"] == group]) for group in dict.fromkeys(row["group"] for row in rows)]
    for label, members in groups:
        if members:
            print(f"{label:22} r8 {geomean([row['r8'] for row in members]):.3f}  rgba16f {geomean([row['rgba16f'] for row in members]):.3f}  instructions {geomean([row['instructions'] for row in members]):.3f}  ({len(members)} cases)")


if __name__ == "__main__":
    main()
