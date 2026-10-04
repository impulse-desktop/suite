#!/usr/bin/env python3

"""What the dev tools share to compile the player's video layer through the
video_shader program the build makes (shader.cpp's compile()): the storage
layouts of gpu/video_shaders.py, a frame laid out in one of them, and the
facts of a frame in the order video_shader reads them.
"""

import math
import random
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SUITE = HERE.parent.parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(SUITE / "gpu"))

import video_shaders

WHITE = 203.0
SIZES = {"source": (1920, 1080), "target": (3840, 2160), "content": "noise"}

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
    curve = table(transfer.get("eotf"))
    if shape == "curve" and same and case["transfer"] == (13 if sdr else 8):
        shape = "identity"
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
    words = [*offsets, *lines, W, H, -(-W >> sx), -(-H >> sy), *case["target"], *case.get("origin", (0, 0)), case.get("tile", 24)]
    numbers = [2.0**-sx, 2.0**-sy, site[0] * ((1 << sx) - 1) * 2.0**-sx, site[1] * ((1 << sy) - 1) * 2.0**-sy]
    numbers += [value for row in decode for value in row] + bias + [red % 2, red // 2, kr, kb]
    numbers += curve + table(transfer.get("oetf")) + table(transfer.get("inverse"))
    numbers += [value for row in to_output for value in row]
    numbers += [transfer.get("decades", 0), 10000 / WHITE, 1000 / WHITE, *to_xyz[1]]
    return [case["format"], system, shape, conversion, output, *(format(word, "x") for word in words), *map(bits, numbers)]
