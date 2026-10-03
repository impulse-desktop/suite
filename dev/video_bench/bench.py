#!/usr/bin/env python3

"""Times the video shaders of gpu/ against hand-written ones on a corpus of pipelines.

Every case compiles its frame's facts into the player's video shader
through shader.cpp (the video_shader program the build makes) and renders it
and the hand-written shader dev/video_bench/hand/NAME.frag over the same
random frame, checks that the two agree at the case's output size, and
times both there into R8 (arithmetic-bound) and RGBA16F (bandwidth-bound)
targets. The cases are prepared by one process
each, as many at once as there are cores; then one harness times them all
on one device, a round over every case at a time, keeping each case's best.
The table gives template/hand ratios and
their geometric means; the instruction and memory counts come from the
driver through VK_KHR_pipeline_executable_properties.

  ix run set/pg/libs lib/vulkan/drivers --vulkan=amd/radv -- python3 dev/video_bench/bench.py --compiler BUILD/dev/video_shader [--rounds N] [--work DIR] [CASE...]
"""

import argparse
import array
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
    ("sota", "gbrp_roundtrip_bilinear", "gbrp", (0, 0), 0, 2, 13, 1, 0, "sdr", (1280, 720), (2560, 1440), "gbrp_srgb_sdr", "bilinear", "box2"),
    ("sota", "gbrp_roundtrip_lanczos3", "gbrp", (0, 0), 0, 2, 13, 1, 0, "sdr", (1280, 720), (2560, 1440), "gbrp_srgb_sdr", "lanczos", "box2"),
    ("sota", "yuv420p_lanczos3_1.42_sota", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1920, 1080), (2731, 1536), "yuv420p_709_sdr_lanczos3_fast", "lanczos", "chart"),
    ("sota", "yuv420p_down_0.5_sota", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (3840, 2160), (1920, 1080), "yuv420p_709_sdr_hermite", "bilinear", "chart"),
    ("kernel", "gbrp_roundtrip_sigmoid", "gbrp", (0, 0), 0, 2, 13, 1, 0, "sdr", (1280, 720), (2560, 1440), "gbrp_srgb_sigmoid", "lanczos", "box2"),
    ("kernel", "yuv420p_sigmoid_1.42", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1920, 1080), (2731, 1536), "yuv420p_709_sdr_sigmoid", "lanczos", "chart"),
    ("dither", "yuv420p_dither8", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1920, 1080), (3840, 2160), "yuv420p_709_sdr", "bilinear", "chart", 8, 1),
    ("dither", "yuv420p_lanczos3_dither8", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1920, 1080), (2731, 1536), "yuv420p_709_sdr_lanczos3_fast", "lanczos", "chart", 8, 2),
    ("scale", "yuv420p_bilinear_1.42", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1920, 1080), (2731, 1536), "yuv420p_709_sdr"),
    ("scale", "yuv420p_lanczos3_1.42", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1920, 1080), (2731, 1536), "yuv420p_709_sdr_lanczos3", "lanczos"),
    ("scale", "yuv420p_lanczos3fast_1.42", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1920, 1080), (2731, 1536), "yuv420p_709_sdr_lanczos3_fast", "lanczos"),
    ("scale", "yuv420p_chart_bilinear_1.42", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1920, 1080), (2731, 1536), "yuv420p_709_sdr_lanczos3", "bilinear", "chart"),
    ("scale", "yuv420p_chart_lanczos3_1.42", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1920, 1080), (2731, 1536), "yuv420p_709_sdr_lanczos3", "lanczos", "chart"),
    ("approx", "yuv420p_chart_f1_1.42", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1920, 1080), (2731, 1536), "yuv420p_709_sdr_lanczos3_f1", "lanczos", "chart"),
    ("approx", "yuv420p_chart_f2_1.42", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1920, 1080), (2731, 1536), "yuv420p_709_sdr_lanczos3_f2", "lanczos", "chart"),
    ("approx", "yuv420p_chart_f3_1.42", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1920, 1080), (2731, 1536), "yuv420p_709_sdr_lanczos3_f3", "lanczos", "chart"),
    ("down", "yuv420p_hermite_0.5", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (3840, 2160), (1920, 1080), "yuv420p_709_sdr_hermite", "bilinear", "chart"),
    ("down", "yuv420p_hermite_0.667", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (3840, 2160), (2560, 1440), "yuv420p_709_sdr_hermite", "bilinear", "chart"),
    ("down", "yuv420p_hermite_0.333", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (3840, 2160), (1280, 720), "yuv420p_709_sdr_hermite", "bilinear", "chart"),
    ("down", "yuv420p_hermite_0.25", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (3840, 2160), (960, 540), "yuv420p_709_sdr_hermite", "bilinear", "chart"),
    ("down", "yuv420p_hermite_0.5005", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (3840, 2160), (1922, 1081), "yuv420p_709_sdr_hermite", "bilinear", "chart"),
    ("down", "yuv420p_hermite_noise_0.5", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (3840, 2160), (1920, 1080), "yuv420p_709_sdr_hermite", "bilinear", "noise"),
    ("down", "yuv420p_vs_bilinear_0.5", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (3840, 2160), (1920, 1080), "yuv420p_709_sdr", "bilinear", "chart"),
    ("scale", "yuv420p_bilinear_1.46", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1280, 720), (1867, 1050), "yuv420p_709_sdr"),
    ("scale", "yuv420p_lanczos3_1.46", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1280, 720), (1867, 1050), "yuv420p_709_sdr_lanczos3", "lanczos"),
    ("scale", "yuv420p_lanczos3fast_1.46", "yuv420p", (1, 1), 1, 1, 1, 1, 1, "sdr", (1280, 720), (1867, 1050), "yuv420p_709_sdr_lanczos3_fast", "lanczos"),
]
FIELDS = ("group", "name", "format", "subsampling", "matrix", "range", "transfer", "primaries", "location", "output", "source", "target", "hand", "filter", "content", "dither", "phase")
SIZES = {"source": (1920, 1080), "target": (3840, 2160), "filter": "bilinear", "content": "noise", "dither": 0, "phase": 0}

MODULE = video_shaders.tables()
LAYOUTS = video_shaders.layouts(MODULE)


def layout_of(format):
    for name, layout in LAYOUTS.items():
        for formats, components in layout["members"]:
            if format in formats:
                return name, layout, components
    raise KeyError(format)


def half(value):
    return struct.unpack("<H", struct.pack("<e", value))[0]


def chart(c, x, y, W, H):
    u, v = x / W, y / H
    value = 0.25 + 0.3 * u + 0.2 * v * (1 + c) / 3
    r2 = ((u - 0.75) * W) ** 2 + ((v - 0.3) * H) ** 2
    if r2 < (0.2 * W) ** 2:
        value = 0.5 + 0.45 * math.cos(math.pi * r2 / (0.4 * W))
    elif 0.1 < u < 0.4 and 0.15 < v < 0.45:
        value = 0.85 - 0.1 * c
    elif (u - 0.25) ** 2 + (v - 0.7) ** 2 < 0.02:
        value = 0.1 + 0.05 * c
    if 0.5 < v < 0.9 and 0.5 < u < 0.95 and int(x) % 7 == 0:
        value = 0.95
    value += 0.04 * (math.sin(0.11 * x + 1.3 * c) * math.sin(0.07 * y) + 0.5 * math.sin(0.37 * x + 0.23 * y + c))
    return min(max(value, 0.0), 1.0)


def frame(case, layout, components):
    rng = random.Random(1)
    sx, sy = case["subsampling"]
    planes = 1 + max(component[0] for component in components)
    W, H = case["source"]
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
        scale = (W / width, H / height)
        for y in range(height):
            row = offsets[plane] + y * lines[plane]
            for x in range(width):
                if case["content"] == "box2":
                    level = sum(chart(c, 2 * x * scale[0] + dx, 2 * y * scale[1] + dy, 2 * W, 2 * H) for dx in (0, 1) for dy in (0, 1)) / 4
                elif case["content"] == "chart":
                    level = chart(c, x * scale[0], y * scale[1], W, H)
                else:
                    level = rng.random()
                if "float" in layout["flags"]:
                    value = half(level) if depth == 16 else struct.unpack("<I", struct.pack("<f", level))[0]
                elif case["content"] != "noise":
                    value = round(level * (2**depth - 1)) << shift
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


def table(piece):
    if not piece:
        return [0.0] * 11
    upper, lower, threshold = video_shaders.segments(piece)
    return [*upper, *lower, threshold]


def bits(value):
    return format(struct.unpack("<Q", struct.pack("<d", float(value)))[0], "x")


def facts(case, layout, components, offsets, lines):
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
    eotf = video_shaders.segments(transfer.get("eotf", [[0] * 5]))
    curve = table(transfer.get("eotf"))
    power = eotf[0] == eotf[1] and eotf[0][2] == 1 and eotf[0][3] == 0 and eotf[0][4] == 0 and eotf[2] < 0
    if shape == "curve" and same and case["transfer"] == (13 if sdr else 8):
        shape = "identity"
    elif shape == "curve" and same and sdr and power and eotf[0][1] in (1, 2.4):
        scale, gamma = eotf[0][0], eotf[0][1]
        curve = [1.055 * scale ** (1 / 2.4), gamma / 2.4, 1, 0, 0.055, 12.92 * scale, gamma, 1, 0, 0, (0.0031308 / scale) ** (1 / gamma)]
    elif sdr or shape == "log":
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
    red = case["format"][6:].index("r") if model == "bayer" else 0
    system = matrix["system"] if yuv else model
    W, H = case["source"]
    words = [*offsets, *lines, W, H, -(-W >> sx), -(-H >> sy), *case["target"], case["dither"], case["phase"]]
    numbers = [2.0**-sx, 2.0**-sy, site[0] * ((1 << sx) - 1) * 2.0**-sx, site[1] * ((1 << sy) - 1) * 2.0**-sy]
    numbers += [value for row in decode for value in row] + bias + [red % 2, red // 2, kr, kb]
    numbers += curve + table(transfer.get("oetf")) + table(transfer.get("inverse"))
    numbers += [value for row in to_output for value in row]
    numbers += [transfer.get("decades", 0), 10000 / WHITE, 1000 / WHITE, *to_xyz[1]]
    return [case["format"], system, shape, conversion, output, case["filter"], *(format(word, "x") for word in words), *map(bits, numbers)]


def prepare(case, directory, compiler):
    name, layout, components = layout_of(case["format"])
    data, offsets, lines = frame(case, layout, components)
    arguments = facts(case, layout, components, offsets, lines)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "data.bin").write_bytes(data)
    (directory / "template.ubo").write_bytes(bytes(16))
    compiled = subprocess.run([compiler, *arguments], check=True, capture_output=True)
    (directory / "template.spv").write_bytes(compiled.stdout)
    (directory / "compile").write_text(re.search(r"compile (\d+) ns", compiled.stderr.decode()).group(1))
    (directory / "size").write_text("%d %d" % case["target"])
    sx, sy = case["subsampling"]
    W, H = case["source"]
    TW, TH = case["target"]
    planar = layout["model"] in ("yuv", "rgb") and all(component[1] == 1 and component[2] == 0 and component[3] == 0 and component[4] == 8 for component in components)
    planar = planar and sorted(component[0] for component in components) == list(range(len(components)))
    if planar:
        rows = [f"{W} {H} {TW} {TH} {len(components)} {int(layout['model'] == 'rgb')} {int(case['range'] == 2 or layout['model'] == 'rgb')} {int(bool(sx or sy))}"]
        for plane in range(len(components)):
            c = next(c for c, component in enumerate(components) if component[0] == plane)
            chroma = layout["model"] == "yuv" and c in (1, 2)
            rows.append(f"{-(-W >> sx) if chroma else W} {-(-H >> sy) if chroma else H} {offsets[plane]} {lines[plane]} {c} 1")
        (directory / "placebo.txt").write_text("\n".join(rows) + "\n")
    if case["content"] == "box2":
        truth = array.array("f", (chart(c, x, y, TW, TH) for y in range(TH) for x in range(TW) for c in range(3)))
        (directory / "truth.raw").write_bytes(truth.tobytes())
    (directory / "optimum.ubo").write_bytes(struct.pack("<4I", *offsets) + struct.pack("<4I", *lines) + struct.pack("<4I", W, H, -(-W >> sx), -(-H >> sy)) + struct.pack("<4f", WHITE, *case["target"], 0))
    hand = HERE / "hand" / case.get("hand", case["name"])
    stage = "comp" if hand.with_suffix(".comp").exists() else "frag"
    (directory / f"optimum.{stage}").write_text(hand.with_suffix(f".{stage}").read_text())
    if stage == "comp":
        (directory / "optimum.kind").write_text("kernel")
    subprocess.run(["glslangValidator", "--quiet", "--target-env", "vulkan1.1", "-V", "-S", stage, str(directory / f"optimum.{stage}"), "-o", str(directory / "optimum.spv")], check=True)
    return "_".join((name, *arguments[1:6]))


def measure(harness, vertex, directories, rounds):
    quiet = dict(os.environ, RADV_DEBUG="nocache", MESA_SHADER_CACHE_DISABLE="true")
    text = subprocess.run([str(harness), str(vertex), str(DRAWS), str(rounds), *map(str, directories)], check=True, capture_output=True, text=True, env=quiet).stdout
    results = {str(directory): {"times": {}, "stats": {}, "build": {}} for directory in directories}
    for directory, shader, value in re.findall(r"^build (\S+) (\w+) ([\d.]+)$", text, re.M):
        results[directory]["build"][shader] = float(value)
    for directory, value in re.findall(r"^difference (\S+) (\S+)$", text, re.M):
        results[directory]["difference"] = float(value)
    for directory, value in re.findall(r"^rmse (\S+) (\S+)$", text, re.M):
        results[directory]["rmse"] = float(value)
    for directory, shader, rest in re.findall(r"^(\S+) stats (\w+) (.*)$", text, re.M):
        results[directory]["stats"][shader] = {key: int(value) for key, value in re.findall(r"(\w[\w ]*?)=(\d+)", rest)}
    for directory, target, shader, value in re.findall(r"^time (\S+) (\w+) (\w+) ([\d.]+)$", text, re.M):
        results[directory]["times"][(target, shader)] = float(value)
    return results


def recoverable(x, y, W, H):
    """Away from the chart's zone plate and one-pixel lines, which a 2x box
    takes past the source's Nyquist and no scaler brings back."""
    u, v = x / W, y / H
    return ((u - 0.75) * W) ** 2 + ((v - 0.3) * H) ** 2 >= (0.2 * W) ** 2 and not (0.5 < v < 0.9 and 0.5 < u < 0.95)


def error(path, TW, TH, truth):
    raw = path.read_bytes()
    data = memoryview(raw).cast("f" if len(raw) == TW * TH * 16 else "e")
    total = 0.0
    count = 0
    for y in range(0, TH, 2):
        for x in range(0, TW, 2):
            if not recoverable(x, y, TW, TH):
                continue
            count += 1
            at, known = (y * TW + x) * 4, (y * TW + x) * 3
            for c in range(3):
                d = data[at + c] - truth[known + c]
                total += d * d
    return math.sqrt(total / (count * 3))


def compare(placebo, cases, directories, results, rounds):
    """libplacebo on the same frames: per-frame GPU time of every preset and,
    where the case knows its truth, each scaler's RMSE against it."""
    chosen = [(case, directory) for case, directory in zip(cases, directories) if (directory / "placebo.txt").exists()]
    if not chosen:
        return
    times = {}
    for preset in ("fast", "default", "high_quality"):
        text = subprocess.run([placebo, preset, str(rounds), *(str(directory) for case, directory in chosen)], check=True, capture_output=True, text=True).stdout
        for directory, gpu, wall in re.findall(rf"^placebo (\S+) {preset} gpu ([\d.e+]+) wall ([\d.e+]+) format \S+$", text, re.M):
            times[(directory, preset)] = (float(gpu), float(wall))
    print("versus libplacebo (GPU us per frame into rgba16f; RMSE against the truth where known)")
    for case, directory in chosen:
        ours = results[str(directory)]["times"][("rgba16f", "template")]
        line = f"{case['name']:28} ours {ours:8.1f}"
        for preset in ("fast", "default", "high_quality"):
            gpu, wall = times.get((str(directory), preset), (float("nan"), float("nan")))
            line += f"  {preset} {gpu:8.1f} ({ours / gpu:.2f}x)"
        if (directory / "truth.raw").exists():
            TW, TH = case["target"]
            truth = memoryview((directory / "truth.raw").read_bytes()).cast("f")
            line += f"  rmse ours {error(directory / 'template.raw', TW, TH, truth):.4f} hand {error(directory / 'optimum.raw', TW, TH, truth):.4f}"
            for preset in ("fast", "default", "high_quality"):
                line += f" {preset} {error(directory / f'placebo_{preset}.raw', TW, TH, truth):.4f}"
        print(line)


def geomean(values):
    return math.exp(sum(math.log(value) for value in values) / len(values))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--work", default=str(SUITE / ".build" / "video_bench"))
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--placebo", default="")
    parser.add_argument("cases", nargs="*")
    args = parser.parse_args()
    work = Path(args.work)
    cases = [{**SIZES, **dict(zip(FIELDS, entry))} for entry in CORPUS if not args.cases or entry[1] in args.cases]
    if args.prepare:
        for case in cases:
            (work / case["name"] / "variant").write_text(prepare(case, work / case["name"], args.compiler))
        return
    work.mkdir(parents=True, exist_ok=True)
    harness = work / "harness"
    flags = " ".join(os.environ.get(name, "") for name in ("CPPFLAGS", "CFLAGS"))
    libraries = " ".join(os.environ.get(name, "") for name in ("CTRFLAGS", "LDFLAGS"))
    subprocess.run(f"cc {flags} -o {harness} {HERE / 'harness.c'} $(pkg-config --cflags --libs vulkan) {libraries}", shell=True, check=True)
    vertex = work / "fullscreen.vert.spv"
    subprocess.run(["glslangValidator", "--quiet", "--target-env", "vulkan1.1", "-V", str(HERE / "fullscreen.vert"), "-o", str(vertex)], check=True)
    pending, running = list(cases), []
    while pending or running:
        while pending and len(running) < (os.cpu_count() or 1):
            case = pending.pop(0)
            running.append(subprocess.Popen([sys.executable, __file__, "--prepare", "--compiler", args.compiler, "--work", str(work), case["name"]]))
        running[0].wait()
        if running[0].returncode:
            raise SystemExit(f"preparing a case failed: {running[0].args}")
        running = [process for process in running if process.poll() is None or process.returncode]
    directories = [work / case["name"] for case in cases]
    results = measure(harness, vertex, directories, args.rounds)
    rows = []
    for case, directory in zip(cases, directories):
        result = results[str(directory)]
        best, stats = result["times"], result["stats"]
        row = dict(case, variant=(directory / "variant").read_text(), difference=result["difference"], rmse=result["rmse"], stats=stats, best=best)
        row["r8"] = best[("r8", "template")] / best[("r8", "optimum")]
        row["rgba16f"] = best[("rgba16f", "template")] / best[("rgba16f", "optimum")]
        row["instructions"] = stats["template"]["Instructions"] / stats["optimum"]["Instructions"]
        row["compile"] = int((directory / "compile").read_text()) / 1e3
        row["build"] = result["build"]
        rows.append(row)
        print(
            f"{case['name']:22} r8 {row['r8']:.3f}  rgba16f {row['rgba16f']:.3f}  instructions {stats['optimum']['Instructions']}/{stats['template']['Instructions']}"
            f"  loads {stats['optimum']['VMEM']}/{stats['template']['VMEM']}  difference {row['difference']:.3g} rmse {row['rmse']:.2g}"
            f"  build {row['compile']:.0f} us + {row['build']['template']:.2f} ms (hand {row['build']['optimum']:.2f} ms)  {row['variant']}"
        )
    if args.placebo:
        compare(args.placebo, cases, directories, results, args.rounds)
    groups = [("all", [row for row in rows if row["group"] not in ("scale", "approx", "down", "dither", "sota", "kernel")])] + [(group, [row for row in rows if row["group"] == group]) for group in dict.fromkeys(row["group"] for row in rows)]
    for label, members in groups:
        if members:
            print(
                f"{label:22} r8 {geomean([row['r8'] for row in members]):.3f}  rgba16f {geomean([row['rgba16f'] for row in members]):.3f}  instructions {geomean([row['instructions'] for row in members]):.3f}"
                f"  build {geomean([row['compile'] for row in members]):.0f} us + {geomean([row['build']['template'] for row in members]):.2f} ms (hand {geomean([row['build']['optimum'] for row in members]):.2f} ms)  ({len(members)} cases)"
            )


if __name__ == "__main__":
    main()
