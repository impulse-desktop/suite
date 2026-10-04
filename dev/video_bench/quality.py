#!/usr/bin/env python3

"""How close the player's video comes to a real picture, and how fast, next
to libplacebo, over scale factors and the frame formats decoders hand out.
The pieces are nodes of build.py's video_quality and video_speed targets;
this file is what those nodes run between the C tools.

Every crop of corpus/ (960x540) is a truth. A factor above 1 scales the crop
down in linear light to 960/f x 540/f, stores it in the frame format and
scales it back to 960x540; a factor below 1 stores the crop itself and
shrinks it to 960f x 540f, the truth then being the crop's area average in
linear light. YUV formats are BT.709 limited range with left-sited chroma
and the sRGB transfer, so both sides decode the same curve; bgra is sRGB.
Ours is what the player draws: compile()'s layer merged into the
compositor of gpu/compose.comp, run by dev/compositor's harness over a
frame the video fills; libplacebo draws with its fast, default and
high-quality presets. Both write 8-bit sRGB, as a display takes it, each
with its own dithering. Each result is compared with its truth: PSNR and
RMSE in linear light over RGB, the mean OKLab difference and its chroma
part, SSIM of luma and SSIMULACRA 2 (higher is better: 100 is identical,
90 is visually lossless).

The speed matrix times videos of the usual sizes drawn into the usual
windows and displays: GPU time per frame, the best of 20, ours the
compositor's whole frame. The frames and shaders are made first, side by
side; the timings then run one after another.

  ix run set/pg/libs lib/ffmpeg/7 lib/openal bin/wabt bin/jxl bin/glslang lib/placebo/7 --vulkan=amd/radv \\
      lib/vulkan/drivers --vulkan=amd/radv -- ./build -B BUILD video_quality video_speed

leaves BUILD/video_quality/quality.{txt,json} and BUILD/video_speed/speed.{txt,json}.
"""

import argparse
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.dont_write_bytecode = True

TILE = 24
CHUNK = 16
VARIANTS = ("ours", "placebo_fast", "placebo_default", "placebo_high_quality")


def case(fmt, source, target):
    rgb = fmt == "bgra"
    subsampling = (1, 1) if fmt in ("yuv420p", "nv12", "yuv420p10le", "p010le") else (0, 0)
    return {"format": fmt, "subsampling": subsampling, "matrix": 0 if rgb else 1, "range": 2 if rgb else 1, "transfer": 13, "primaries": 1, "location": 0 if rgb else 1, "output": "sdr", "source": source, "target": target, "origin": (0, 0), "tile": TILE, "content": "noise"}


def shader(corpus, compiler, host, fmt, source, target, directory):
    """The player's layer for the frame, merged into the compositor."""
    import bench

    spec = importlib.util.spec_from_file_location("compositor", HERE.parent / "compositor" / "bench.py")
    compositor = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(compositor)
    words = subprocess.run([corpus, "layout", fmt, *map(str, source)], check=True, capture_output=True, text=True).stdout.split()
    offsets, lines = [int(word) for word in words[1:5]], [int(word) for word in words[6:10]]
    _, layout, components = bench.layout_of(fmt)
    layer = subprocess.run([compiler, *bench.facts(case(fmt, source, target), layout, components, offsets, lines)], check=True, capture_output=True).stdout
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "layer.spv").write_bytes(compositor.merge(Path(host).read_bytes(), layer))


def filled(path, size):
    """A compositor frame without ImGui whose video covers it."""
    path.write_bytes(struct.pack("<6I4f", 0x46434D49, size[0], size[1], 0, 0, 1, 0.0, 0.0, float(size[0]), float(size[1])))


def compose(args, directory, layer, rounds):
    text = subprocess.run([args.compositor, args.plain, str(rounds), str(directory / "frame.bin"), str(directory), str(layer), str(directory / "data.bin")], check=True, capture_output=True, text=True).stdout
    return float(re.search(r"^compose gpu ([\d.]+) us", text, re.M).group(1))


def render(args):
    """One variant of one format and factor over every picture: each frame
    made in the format and drawn, a chunk at a time, and scored against its
    truth."""
    pictures = [args.pictures[i:i + 3] for i in range(0, len(args.pictures), 3)]
    scratch = Path(args.out).with_suffix("")
    scratch.mkdir(parents=True, exist_ok=True)
    scores = scratch / "scores.txt"
    source, target = tuple(args.sizes[:2]), tuple(args.sizes[2:])
    for start in range(0, len(pictures), CHUNK):
        chunk = pictures[start:start + CHUNK]
        directories = [scratch / name for name, _, _ in chunk]
        for (name, picture, _), directory in zip(chunk, directories):
            directory.mkdir()
            subprocess.run([args.corpus, "input", picture, str(directory), *map(str, source), args.format, *map(str, target)], check=True)
        if args.variant == "ours":
            for directory in directories:
                filled(directory / "frame.bin", target)
                compose(args, directory, args.layer, 1)
            drawn = "composed.ppm"
        else:
            subprocess.run([args.placebo, args.variant[len("placebo_"):], "0", *map(str, directories)], check=True)
            drawn = f"{args.variant}.raw"
        subprocess.run([args.corpus, "metric", str(scores), *(word for (name, _, truth), directory in zip(chunk, directories) for word in (name, truth, str(directory / drawn)))], check=True)
        for directory in directories:
            shutil.rmtree(directory)
    lines = [line.split() for line in scores.read_text().splitlines()]
    shutil.rmtree(scratch)
    pictures = {words[0]: {words[i]: float(words[i + 1]) for i in range(1, len(words), 2)} for words in lines}
    Path(args.out).write_text(json.dumps({"format": args.format, "factor": args.factor, "variant": args.variant, "pictures": pictures}) + "\n")


def report(out_json, out_text, paths):
    parts = [json.loads(Path(path).read_text()) for path in paths]
    rows = [{"name": name, "format": part["format"], "factor": part["factor"], "variant": part["variant"], **values} for part in parts for name, values in sorted(part["pictures"].items())]
    out_json.write_text(json.dumps(rows, indent=1) + "\n")
    table = {(row["format"], row["factor"], row["variant"], row["name"]): row for row in rows}
    variants = [variant for variant in VARIANTS if any(row["variant"] == variant for row in rows)]
    names = sorted({row["name"] for row in rows})
    text = []
    for fmt in dict.fromkeys(row["format"] for row in rows):
        text.append(f"{fmt}: mean ssimulacra2 (psnr) over {len(names)} pictures; wins: pictures where ours beats placebo default on ssimulacra2")
        text.append(f"  {'factor':>7} " + " ".join(f"{variant:>20}" for variant in variants) + "   wins")
        for factor in sorted({row["factor"] for row in rows if row["format"] == fmt}):
            cells = []
            for variant in variants:
                chosen = [table[(fmt, factor, variant, name)] for name in names]
                cells.append(f"{sum(row['ssimulacra2'] for row in chosen) / len(chosen):11.2f} ({sum(row['psnr'] for row in chosen) / len(chosen):6.2f})")
            wins = sum(table[(fmt, factor, "ours", name)]["ssimulacra2"] > table[(fmt, factor, "placebo_default", name)]["ssimulacra2"] for name in names) if "placebo_default" in variants else 0
            text.append(f"  {factor:>7g} " + " ".join(f"{cell:>20}" for cell in cells) + f"   {wins}/{len(names)}")
    out_text.write_text("\n".join(text) + "\n")


def pair_of(text):
    video, screen = (tuple(int(value) for value in size.split("x")) for size in text.split(":"))
    return video, screen


def cases(args):
    """The CPU side of one format's timings: every video and screen size's
    description, compositor frame and layer, and a noise frame of each video
    size."""
    root = Path(args.directory)
    (root / "frames").mkdir(parents=True, exist_ok=True)
    for text in args.pairs:
        video, screen = pair_of(text)
        directory = root / "cases" / text.replace(":", "_")
        directory.mkdir(parents=True, exist_ok=True)
        subprocess.run([args.corpus, "input", "noise", str(directory), *map(str, video), args.format, *map(str, screen)], check=True)
        (directory / "dump").unlink()
        frame = root / "frames" / f"{video[0]}x{video[1]}.bin"
        if frame.exists():
            (directory / "data.bin").unlink()
        else:
            (directory / "data.bin").rename(frame)
        filled(directory / "frame.bin", screen)
        shader(args.corpus, args.compiler, args.host, args.format, video, screen, directory)


def speed(args):
    """Every format, size and variant one after another, after all the CPU
    work: on an APU a busy CPU slows the GPU down, and processes timing side
    by side stretch each other's numbers."""
    rows = []
    for fmt in args.formats:
        root = Path(args.cases) / fmt
        scratch = Path(args.json).parent / "frames" / fmt
        cases = []
        for text in args.pairs:
            video, screen = pair_of(text)
            directory = scratch / text.replace(":", "_")
            directory.mkdir(parents=True)
            os.link(root / "frames" / f"{video[0]}x{video[1]}.bin", directory / "data.bin")
            for name in ("size", "placebo.txt", "frame.bin"):
                os.link(root / "cases" / directory.name / name, directory / name)
            cases.append((video, screen, directory))
        times = {(str(directory), "ours"): compose(args, directory, root / "cases" / directory.name / "layer.spv", args.rounds) for _, _, directory in cases}
        for preset in args.presets:
            text = subprocess.run([args.placebo, preset, str(args.rounds), *(str(directory) for _, _, directory in cases)], check=True, capture_output=True, text=True).stdout
            times.update({(directory, f"placebo_{preset}"): float(value) for directory, value in re.findall(rf"^placebo (\S+) {preset} gpu ([\d.]+)", text, re.M)})
        rows += [{"format": fmt, "video": video, "screen": screen, **{variant: times[(str(directory), variant)] for variant in VARIANTS if (str(directory), variant) in times}} for video, screen, directory in cases]
        shutil.rmtree(scratch)
    Path(args.json).write_text(json.dumps(rows, indent=1) + "\n")
    text = []
    for fmt in args.formats:
        text.append(f"{fmt}: GPU us per frame")
        text.append(f"  {'video':>9} -> {'screen':<9} {'factor':>6} " + " ".join(f"{variant:>20}" for variant in VARIANTS))
        for row in rows:
            if row["format"] == fmt:
                (w, h), (tw, th) = row["video"], row["screen"]
                cells = [f"{row[variant]:20.1f}" if row.get(variant) else f"{'-':>20}" for variant in VARIANTS]
                text.append(f"  {w:>4}x{h:<4} -> {tw:>4}x{th:<4} {tw / w:6.3g} " + " ".join(cells))
    Path(args.text).write_text("\n".join(text) + "\n")


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    one = commands.add_parser("shader")
    one.add_argument("corpus")
    one.add_argument("compiler")
    one.add_argument("host")
    one.add_argument("format")
    one.add_argument("sizes", type=int, nargs=4)
    one.add_argument("directory")
    one = commands.add_parser("render")
    one.add_argument("out")
    one.add_argument("variant", choices=VARIANTS)
    one.add_argument("format")
    one.add_argument("factor", type=float)
    one.add_argument("sizes", type=int, nargs=4)
    one.add_argument("pictures", nargs="+", help="NAME SOURCE.ppm TRUTH.ppm for each picture")
    one.add_argument("--corpus", required=True)
    one.add_argument("--layer", default="")
    one.add_argument("--compositor", default="")
    one.add_argument("--plain", default="")
    one.add_argument("--placebo", default="")
    one = commands.add_parser("report")
    one.add_argument("json")
    one.add_argument("text")
    one.add_argument("parts", nargs="+")
    one = commands.add_parser("cases")
    one.add_argument("corpus")
    one.add_argument("compiler")
    one.add_argument("host")
    one.add_argument("format")
    one.add_argument("directory")
    one.add_argument("pairs", nargs="+", help="VIDEOWxH:SCREENWxH")
    one = commands.add_parser("speed")
    one.add_argument("json")
    one.add_argument("text")
    one.add_argument("--formats", nargs="+", required=True)
    one.add_argument("--pairs", nargs="+", required=True, help="VIDEOWxH:SCREENWxH")
    one.add_argument("--presets", nargs="*", default=[])
    one.add_argument("--cases", required=True)
    one.add_argument("--compositor", required=True)
    one.add_argument("--plain", required=True)
    one.add_argument("--placebo", default="")
    one.add_argument("--rounds", type=int, default=20)
    args = parser.parse_args()
    if args.command == "shader":
        shader(args.corpus, args.compiler, args.host, args.format, tuple(args.sizes[:2]), tuple(args.sizes[2:]), Path(args.directory))
    elif args.command == "render":
        render(args)
    elif args.command == "report":
        report(Path(args.json), Path(args.text), args.parts)
    elif args.command == "cases":
        cases(args)
    else:
        speed(args)


if __name__ == "__main__":
    main()
