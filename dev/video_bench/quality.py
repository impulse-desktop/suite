#!/usr/bin/env python3

"""How close the video scalers come to a real picture.

Every crop of corpus/ is scaled down in linear light to an input size,
stored as BT.709 limited-range 4:2:0 with left-sited chroma (sRGB transfer,
so both sides decode the same curve), and scaled back to the crop's size by
our shaders (fragment bilinear, fragment lanczos, the lanczos kernel) and by
libplacebo's fast, default and high-quality presets. To measure shrinking
(--downs) the crop itself is the input and its area average in linear
light at the smaller size the truth. Each result is
compared with its truth: PSNR and RMSE in linear light over RGB, the mean
OKLab difference and its chroma part, SSIM of luma and SSIMULACRA 2 (higher
is better: 100 is identical, 90 is visually lossless).

  ix run set/pg/libs bin/jxl lib/placebo/7 --vulkan=amd/radv lib/vulkan/drivers --vulkan=amd/radv -- \\
      python3 dev/video_bench/quality.py --compiler BUILD/dev/video_shader [--work DIR] [--ratios 1.333,2] [--downs 2] [NAME...]
"""

import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.dont_write_bytecode = True

import bench

CORPUS = HERE / "corpus"
TARGET = (960, 540)
OURS = [("bilinear", "bilinear", "fragment"), ("lanczos", "lanczos", "fragment"), ("kernel", "lanczos", "kernel")]
PRESETS = ("fast", "default", "high_quality")
VARIANTS = [name for name, _, _ in OURS] + [f"placebo_{preset}" for preset in PRESETS]
METRICS = ("psnr", "linear", "delta", "chroma", "ssim", "ssimulacra2")
BATCH = 48


def case(source, target, filter, stage):
    return {"format": "yuv420p", "subsampling": (1, 1), "matrix": 1, "range": 1, "transfer": 13, "primaries": 1, "location": 1, "output": "sdr", "source": source, "target": target, "filter": filter, "stage": stage, "dither": 0, "phase": 0}


def planes(W, H):
    sizes = [(W, H), (-(-W >> 1), -(-H >> 1)), (-(-W >> 1), -(-H >> 1))]
    offsets, lines, total = [], [], 0
    for width, height in sizes:
        offsets.append(total)
        lines.append((width + 63) // 64 * 64)
        total += (lines[-1] * height + 63) // 64 * 64
    return sizes, offsets, lines, total


def pack(raw, W, H):
    sizes, offsets, lines, total = planes(W, H)
    data = bytearray(total + 64)
    at = 0
    for (width, height), offset, line in zip(sizes, offsets, lines):
        for y in range(height):
            data[offset + y * line:offset + y * line + width] = raw[at:at + width]
            at += width
    return bytes(data), (offsets + [0])[:4], (lines + [0])[:4]


def build(work):
    flags = " ".join(os.environ.get(name, "") for name in ("CPPFLAGS", "CFLAGS"))
    libraries = " ".join(os.environ.get(name, "") for name in ("CTRFLAGS", "LDFLAGS"))
    tools = {}
    for name, source, extra in (("corpus", "corpus.c", "-lm"), ("harness", "harness.c", "$(pkg-config --cflags --libs vulkan)"), ("placebo", "placebo.c", "$(pkg-config --cflags --libs libplacebo vulkan)")):
        tools[name] = work / name
        subprocess.run(f"cc -O2 {flags} -o {tools[name]} {HERE / source} {extra} {libraries}", shell=True, check=True)
    tools["vertex"] = work / "fullscreen.vert.spv"
    subprocess.run(["glslangValidator", "--quiet", "--target-env", "vulkan1.1", "-V", str(HERE / "fullscreen.vert"), "-o", str(tools["vertex"])], check=True)
    return tools


def shader(compiler, directory, label, source, target, filter, stage, offsets, lines):
    name, layout, components = bench.layout_of("yuv420p")
    arguments = bench.facts(case(source, target, filter, stage), layout, components, offsets, lines)
    (directory / f"{label}.spv").write_bytes(subprocess.run([compiler, *arguments], check=True, capture_output=True).stdout)
    (directory / f"{label}.ubo").write_bytes(bytes(16))
    if stage == "kernel":
        (directory / f"{label}.kind").write_text("kernel")


def sizes(scale):
    if scale >= 1:
        return (round(TARGET[0] / scale), round(TARGET[1] / scale)), TARGET
    return TARGET, (round(TARGET[0] * scale), round(TARGET[1] * scale))


def prepare(tools, compiler, work, truth, name, ratio):
    (W, H), target = sizes(ratio)
    first, second = work / "runs" / f"{name}_{ratio:g}_a", work / "runs" / f"{name}_{ratio:g}_b"
    yuv = work / "runs" / f"{name}_{ratio:g}.yuv"
    first.mkdir(parents=True, exist_ok=True)
    second.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(tools["corpus"]), "input", str(truth), str(yuv), str(W), str(H), "8"], check=True)
    data, offsets, lines = pack(yuv.read_bytes(), W, H)
    yuv.unlink()
    for directory in (first, second):
        (directory / "data.bin").write_bytes(data)
        (directory / "size").write_text("%d %d" % target)
        (directory / "dump").write_text("")
    shader(compiler, first, "optimum", (W, H), target, *OURS[0][1:], offsets, lines)
    shader(compiler, first, "template", (W, H), target, *OURS[2 if ratio >= 1 else 1][1:], offsets, lines)
    shader(compiler, second, "optimum", (W, H), target, *OURS[1][1:], offsets, lines)
    shader(compiler, second, "template", (W, H), target, *OURS[1][1:], offsets, lines)
    if ratio < 1:
        subprocess.run([str(tools["corpus"]), "shrink", str(truth), str(work / "truth" / f"{name}_{ratio:g}.ppm"), *map(str, target)], check=True)
    plane_sizes, _, _, _ = planes(W, H)
    rows = [f"{W} {H} {target[0]} {target[1]} 3 0 0 1 13"]
    rows += [f"{width} {height} {offsets[p]} {lines[p]} {p} 1" for p, (width, height) in enumerate(plane_sizes)]
    (first / "placebo.txt").write_text("\n".join(rows) + "\n")
    return first, second


def measure(tools, truth, raw):
    words = subprocess.run([str(tools["corpus"]), "metric", str(truth), str(raw)], check=True, capture_output=True, text=True).stdout.split()
    return {words[i]: float(words[i + 1]) for i in range(0, len(words), 2)}


def run(tools, compiler, work, truths, jobs):
    with ThreadPoolExecutor(os.cpu_count() or 1) as pool:
        made = list(pool.map(lambda job: prepare(tools, compiler, work, truths[job[0]], *job), jobs))
    directories = [str(directory) for pair in made for directory in pair]
    subprocess.run([str(tools["harness"]), str(tools["vertex"]), "1", "0", *directories], check=True, capture_output=True)
    for preset in PRESETS:
        subprocess.run([str(tools["placebo"]), preset, "0", *(str(first) for first, _ in made)], check=True, capture_output=True)
    outputs = []
    for (name, ratio), (first, second) in zip(jobs, made):
        truth = truths[name] if ratio >= 1 else work / "truth" / f"{name}_{ratio:g}.ppm"
        outputs += [(name, ratio, "bilinear", first / "optimum.raw", truth), (name, ratio, "lanczos", second / "optimum.raw", truth)]
        outputs += [(name, ratio, "kernel", first / "template.raw", truth)] if ratio >= 1 else []
        outputs += [(name, ratio, f"placebo_{preset}", first / f"placebo_{preset}.raw", truth) for preset in PRESETS]
    with ThreadPoolExecutor(os.cpu_count() or 1) as pool:
        scores = list(pool.map(lambda output: measure(tools, output[4], output[3]), outputs))
    for first, second in made:
        for directory in (first, second):
            for path in directory.iterdir():
                path.unlink()
            directory.rmdir()
    return [(name, ratio, variant, score) for (name, ratio, variant, _, _), score in zip(outputs, scores)]


def report(results, ratios):
    for ratio in ratios:
        rows = [row for row in results if row[1] == ratio]
        names = sorted({row[0] for row in rows})
        table = {(row[0], row[2]): row[3] for row in rows}
        print(f"x{ratio:.3g} ({len(names)} pictures)          psnr   linear    delta   chroma     ssim  ssimulacra2  psnr>default  ssimulacra2>default")
        for variant in (variant for variant in VARIANTS if (names[0], variant) in table):
            means = [sum(table[(name, variant)][metric] for name in names) / len(names) for metric in METRICS]
            wins = [sum(table[(name, variant)][metric] > table[(name, "placebo_default")][metric] for name in names) for metric in ("psnr", "ssimulacra2")]
            print(f"  {variant:22} {means[0]:7.3f} {means[1]:8.5f} {means[2]:8.5f} {means[3]:8.5f} {means[4]:8.5f}  {means[5]:11.3f}  {wins[0]:3d}/{len(names)}       {wins[1]:3d}/{len(names)}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--work", default=str(HERE.parent.parent / ".build" / "video_quality"))
    parser.add_argument("--ratios", default="1.333,1.5,2,3")
    parser.add_argument("--downs", default="1.5,2,3")
    parser.add_argument("names", nargs="*")
    args = parser.parse_args()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    ratios = [float(value) for value in args.ratios.split(",") if value] + [1 / float(value) for value in args.downs.split(",") if value]
    manifest = json.loads((CORPUS / "manifest.json").read_text())
    names = [entry["name"] for entry in manifest if not args.names or entry["name"] in args.names]
    tools = build(work)
    truths = {}
    (work / "truth").mkdir(exist_ok=True)
    for name in names:
        truths[name] = work / "truth" / f"{name}.ppm"
        if not truths[name].exists():
            subprocess.run(["djxl", str(CORPUS / f"{name}.jxl"), str(truths[name])], check=True, capture_output=True)
    jobs = [(name, ratio) for ratio in ratios for name in names]
    results = []
    for start in range(0, len(jobs), BATCH):
        results += run(tools, args.compiler, work, truths, jobs[start:start + BATCH])
        print(f"{min(start + BATCH, len(jobs))}/{len(jobs)}", file=sys.stderr, flush=True)
    (work / "results.json").write_text(json.dumps([{"name": name, "ratio": ratio, "variant": variant, **score} for name, ratio, variant, score in results], indent=1) + "\n")
    report(results, ratios)


if __name__ == "__main__":
    main()
