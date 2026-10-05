#!/usr/bin/env python3
"""The video shaders dev/air/check.m runs on a Mac: each one compiled by
video_shader both as AIR and as MSL, a manifest naming them, and the
compositor's host as renderer_metal.mm compiles it, for the layers to be
linked into.

usage: samples.py VIDEO_SHADER RENDERER_METAL OUT
"""

import itertools
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "video_bench"))
sys.dont_write_bytecode = True

import bench

FORMATS = [
    ("yuv420p", (1, 1), 1, 1, 1, 1, 1),
    ("yuva420p", (1, 1), 1, 1, 1, 1, 1),
    ("nv12", (1, 1), 1, 1, 1, 1, 1),
    ("p010le", (1, 1), 9, 1, 16, 9, 1),
    ("yuv420p10le", (1, 1), 9, 1, 18, 9, 1),
    ("yuv444p", (0, 0), 1, 1, 1, 1, 1),
    ("yuyv422", (1, 0), 6, 1, 6, 6, 1),
    ("bgra", (0, 0), 0, 2, 13, 1, 0),
    ("gbrp", (0, 0), 0, 2, 13, 1, 0),
    ("gray", (0, 0), 0, 2, 13, 1, 0),
    ("ya8", (0, 0), 0, 2, 13, 1, 0),
    ("pal8", (0, 0), 0, 2, 13, 1, 0),
    ("bayer_rggb8", (0, 0), 0, 2, 13, 1, 0),
    ("bayer_rggb16le", (0, 0), 0, 2, 13, 1, 0),
]
SIZES = [
    ((1920, 1080), (1280, 720), "down15"),
    ((3840, 2160), (480, 270), "down8"),
    ((1920, 1080), (1920, 1080), "native"),
    ((1280, 720), (2560, 1440), "up2"),
    ((960, 540), (24, 14), "down40"),
]
KINDS = [("inside", "srgb"), ("edge", "pq"), ("edge", "linear"), ("mixed", "srgb"), ("mixed", "wide")]
OFFSETS = [0, 8388608, 12582912, 16777216]
LINES = [8192, 4096, 4096, 8192]
HOST_OLD = "    LAYER_SHARED\n    shown = LAYER_CALL(inside.xy, origin - frame.video, words);\n"
HOST_NEW = "    shown = layer(inside.xy, origin - frame.video, words);\n"
DECLARATION = "#ifdef LAYER\n[[visible]] float4 layer(uint2 local, int2 origin, const device uint* words);\n#endif\n\n"


def host(renderer):
    text = open(renderer).read()
    start = text.index('composeSource = R"metal(') + len('composeSource = R"metal(')
    source = text[start:text.index(')metal"', start)]
    if HOST_OLD in source:
        source = source.replace(HOST_OLD, HOST_NEW).replace("kernel void compose(", DECLARATION + "kernel void compose(", 1)
    return source


def main():
    compiler, renderer, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "compose.metal"), "w") as f:
        f.write(host(renderer))
    jobs = []
    for (fmt, sub, matrix, rng, transfer, prim, loc), (source, target, label), output in itertools.product(FORMATS, SIZES, ("sdr", "hdr")):
        case = {"format": fmt, "subsampling": sub, "matrix": matrix, "range": rng, "transfer": transfer, "primaries": prim,
                "location": loc, "output": output, "source": source, "tile": 24}
        for tiles, surface in KINDS:
            jobs.append((f"{tiles}_{surface}_{fmt}_{label}_{output}", tiles, surface, target, case))

    def run(job):
        name, tiles, surface, target, case = job
        _, layout, components = bench.layout_of(case["format"])
        facts = bench.facts(case, layout, components, OFFSETS, LINES)
        for language, suffix in (("air", "metallib"), ("msl", "metal")):
            command = [compiler, "--target", language, "--output", surface, "--tiles", tiles, "--size", f"{target[0]}x{target[1]}", *facts]
            made = subprocess.run(command, capture_output=True)
            if made.returncode:
                raise RuntimeError(f"{name}: {made.stderr.decode()}")
            with open(os.path.join(out, f"{name}.{suffix}"), "wb") as f:
                f.write(made.stdout)
        return f"{name} {tiles} {surface} {target[0]} {target[1]}\n"

    with ThreadPoolExecutor(os.cpu_count()) as pool:
        lines = list(pool.map(run, jobs))
    with open(os.path.join(out, "manifest.txt"), "w") as f:
        f.writelines(lines)
    print(f"{len(lines)} samples in {out}")


main()
