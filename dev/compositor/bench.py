#!/usr/bin/env python3

"""How fast the tiled compute compositor of gpu/compose.comp draws headless
ImGui frames, and the merge of a video layer into its host that the video
stand shares. The pieces are nodes of build.py's compositor_speed target;
this file is what those nodes run between the C tools.

A scene is a frame compositor_dump makes: demo, play, menu (play with its
menu open over the video) and view, and play again as native and shrunk.
Where it shows a video, the video is a yuv420p noise frame as the video
stand makes them, drawn into the scene's video rectangle with the programs
compile() makes: a kernel for the tiles the video covers, one for its edge,
and its layer merged into the compositor for the tiles where the interface
lies over it; play's and menu's video is 1280x720, native's its rectangle's
own size, shrunk's 4K. The scenes are
made side by side; the timings then run one after another, with
dev/compositor's harness: the GPU time of the whole frame and the CPU time
of its tile programs, the best of 20, and for a scene with video its tiles
alone. Each scene's composed.ppm is what it drew.

  ix run set/pg/libs lib/ffmpeg/7 lib/openal bin/wabt bin/glslang lib/vulkan/drivers --vulkan=amd/radv -- \\
      ./build -B BUILD compositor_speed

leaves BUILD/compositor_speed/speed.{txt,json}.
"""

import argparse
import importlib.util
import json
import re
import struct
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
FORMAT = "yuv420p"
DUMPED = {"native": "play", "shrunk": "play"}


TYPES = set(range(19, 40))
CONSTANTS = {41, 42, 43, 44, 46}
AGGREGATES = {28, 29, 30}
ANNOTATIONS = {71, 72}


def parse(data):
    words = list(struct.unpack("<%dI" % (len(data) // 4), data))
    out, at = [], 5
    while at < len(words):
        count = words[at] >> 16
        out.append(words[at:at + count])
        at += count
    return words[:5], out


def ids(ins):
    op = ins[0] & 0xffff
    w = ins[1:]
    if op in (19, 20, 26):
        return [0]
    if op in (24, 25, 27):
        return [0, 1]
    if op == 21 or op == 22:
        return [0]
    if op == 23:
        return [0, 1]
    if op in (28, 29, 30, 33):
        return list(range(len(w)))
    if op == 32:
        return [0, 2]
    if op in (41, 42, 43):
        return [0, 1]
    if op == 59:
        return [0, 1] + ([3] if len(w) > 3 else [])
    if op == 54:
        return [0, 1, 3]
    if op == 12:
        return [0, 1, 2] + list(range(4, len(w)))
    if op == 81:
        return [0, 1, 2]
    if op in (71, 72, 247, 249):
        return [0]
    if op == 250:
        return [0, 1, 2]
    if op == 246:
        return [0, 1]
    if op in (253, 56):
        return []
    if op in (248, 254, 55, 61, 62, 65, 80, 124, 224) or 109 <= op <= 200:
        return list(range(len(w)))
    raise ValueError("opcode %d" % op)


def key(ins, mapping):
    op = ins[0] & 0xffff
    w = list(ins[1:])
    for i in ids(ins)[1:] if op not in (19, 20, 21, 22, 26) else []:
        w[i] = ("id", mapping.get(w[i], w[i]))
    return (op, tuple(w[1:]))


def merge(host_bytes, layer_bytes, name=b"layer("):
    head, host = parse(host_bytes)
    _, layer = parse(layer_bytes)
    bound = head[3]
    names = {}
    for ins in host:
        if ins[0] & 0xffff == 5:
            names[ins[1]] = struct.pack("<%dI" % (len(ins) - 2), *ins[2:])
    placeholder = next(i for i, n in names.items() if n.startswith(name))
    known = {}
    glsl = None
    for ins in host:
        op = ins[0] & 0xffff
        if op in TYPES and op not in AGGREGATES:
            known[key(ins, {})] = ins[1]
        if op == 11:
            glsl = ins[1]
    mapping = {}
    caps, notes, globals_, body = [], [], [], []
    params = []
    host_caps = {ins[1] for ins in host if ins[0] & 0xffff == 17}

    def fresh():
        nonlocal bound
        bound += 1
        return bound - 1

    def remap(ins):
        out = list(ins)
        for i in ids(ins):
            out[1 + i] = mapping[out[1 + i]]
        return out

    for ins in layer:
        op = ins[0] & 0xffff
        if op == 17:
            if ins[1] not in host_caps:
                caps.append(ins)
            continue
        if op == 11:
            mapping[ins[1]] = glsl
            continue
        if op in (14, 15, 16, 5, 6, 3, 4, 7):
            continue
        if op in ANNOTATIONS:
            notes.append(ins)
            continue
        if op in TYPES or op in CONSTANTS or (op == 59 and ins[3] != 7):
            result = 1 if op in TYPES else 2
            if op in TYPES and op not in AGGREGATES:
                k = key(ins, mapping)
                if k in known:
                    mapping[ins[result]] = known[k]
                    continue
            mapping[ins[result]] = fresh()
            globals_.append(remap(ins))
            if op in TYPES and op not in AGGREGATES:
                known[key(ins, mapping)] = mapping[ins[result]]
            continue
        if op == 54:
            continue
        if op == 55:
            params.append(ins[2])
            continue
        body.append(ins)
    host_params = []
    out_functions = []
    inside = False
    for ins in host:
        op = ins[0] & 0xffff
        if op == 54 and ins[2] == placeholder:
            inside = True
            out_functions.append(ins)
            continue
        if inside:
            if op == 55:
                host_params.append(ins[2])
                out_functions.append(ins)
            if op == 56:
                inside = False
                for p, h in zip(params, host_params):
                    mapping[p] = h
                for b in body:
                    for i in ids(b):
                        v = b[1 + i]
                        if v not in mapping:
                            mapping[v] = fresh()
                    out_functions.append(remap(b))
            continue
        out_functions.append(ins)
    notes = [remap(n) for n in notes]
    result = []
    last_note = max(i for i, ins in enumerate(host) if ins[0] & 0xffff in ANNOTATIONS)
    first_function = next(i for i, ins in enumerate(host) if ins[0] & 0xffff == 54)
    first_cap = max(i for i, ins in enumerate(host) if ins[0] & 0xffff == 17)
    for i, ins in enumerate(host[:first_function]):
        result.append(ins)
        if i == first_cap:
            result.extend(caps)
        if i == last_note:
            result.extend(notes)
    result.extend(globals_)
    function_start = next(i for i, ins in enumerate(out_functions) if ins[0] & 0xffff == 54)
    result.extend(out_functions[function_start:])
    head = list(head)
    head[1] = max(head[1], 0x00010300)
    head[3] = bound
    words = head + [w for ins in result for w in ins]
    return struct.pack("<%dI" % len(words), *words)


def rectangle(frame):
    data = frame.read_bytes()
    if struct.unpack("<I", data[-4:])[0] == 0:
        return None
    return [int(value) for value in struct.unpack("<4f", data[-16:])]


def scene(args):
    """One scene's frame and, where it shows a video, the video's frame and
    programs."""
    directory = Path(args.directory)
    directory.mkdir(parents=True, exist_ok=True)
    frame = directory / "frame.bin"
    with frame.open("wb") as handle:
        subprocess.run([args.dump, DUMPED.get(args.name, args.name), *args.size.split("x")], check=True, stdout=handle)
    rect = rectangle(frame)
    if (rect is None) != (args.source == "-"):
        raise SystemExit(f"{args.name}: the scene's video is not the one asked for")
    if rect is None:
        return
    target = (rect[2] - rect[0], rect[3] - rect[1])
    source = target if args.source == "native" else tuple(int(value) for value in args.source.split("x"))
    subprocess.run([args.corpus, "input", "noise", str(directory), *map(str, source), FORMAT, *map(str, target)], check=True)
    for name in ("dump", "placebo.txt", "size"):
        (directory / name).unlink()
    spec = importlib.util.spec_from_file_location("quality", HERE.parent / "video_bench" / "quality.py")
    quality = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(quality)
    quality.shader(args.corpus, args.compiler, args.host, FORMAT, source, target, directory)


def speed(args):
    """Every scene one after another, after all the CPU work: processes
    timing side by side stretch each other's numbers."""
    rows = []
    for path, source in zip(args.scenes, args.sources, strict=True):
        directory = Path(path)
        out = Path(args.json).parent / directory.name
        out.mkdir(parents=True, exist_ok=True)
        video = [str(directory), str(directory / "data.bin")] if (directory / "data.bin").exists() else []
        text = subprocess.run([args.compositor, args.plain, str(args.rounds), str(directory / "frame.bin"), str(out), *video], check=True, stdout=subprocess.PIPE, text=True).stdout
        compose = re.search(r"^compose gpu ([\d.]+) us, cpu ([\d.]+) us", text, re.M)
        program = re.search(r"^program cpu [\d.]+ us, (\d+) ops \((\d+) triangles\), \d+ list entries, \d+ tiles: (\d+) plain, (\d+) inside, (\d+) edge, (\d+) mixed", text, re.M)
        alone = re.search(r"^alone: video tiles ([\d.]+) us", text, re.M)
        rect = rectangle(directory / "frame.bin")
        shown = None if rect is None else [rect[2] - rect[0], rect[3] - rect[1]]
        rows.append({
            "scene": directory.name,
            "video": shown if source == "native" else None if source == "-" else [int(value) for value in source.split("x")],
            "shown": shown,
            "gpu": float(compose.group(1)),
            "cpu": float(compose.group(2)),
            "video_tiles": float(alone.group(1)) if alone else None,
            "ops": int(program.group(1)),
            "triangles": int(program.group(2)),
            "tiles": dict(zip(("plain", "inside", "edge", "mixed"), map(int, program.group(3, 4, 5, 6)))),
        })
    Path(args.json).write_text(json.dumps(rows, indent=1) + "\n")
    text = [f"compositor: us per frame, the best of {args.rounds}"]
    text.append(f"  {'scene':<8} {'video':>22} {'gpu':>9} {'cpu':>9} {'video tiles':>12} {'ops':>6} {'plain':>6} {'inside':>6} {'edge':>6} {'mixed':>6}")
    for row in rows:
        video = "-" if row["video"] is None else "%dx%d -> %dx%d" % (*row["video"], *row["shown"])
        tiles = "-" if row["video_tiles"] is None else f"{row['video_tiles']:.1f}"
        counts = " ".join(f"{row['tiles'][kind]:>6}" for kind in ("plain", "inside", "edge", "mixed"))
        text.append(f"  {row['scene']:<8} {video:>22} {row['gpu']:9.1f} {row['cpu']:9.1f} {tiles:>12} {row['ops']:>6} {counts}")
    Path(args.text).write_text("\n".join(text) + "\n")


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    one = commands.add_parser("scene")
    one.add_argument("dump")
    one.add_argument("corpus")
    one.add_argument("compiler")
    one.add_argument("host")
    one.add_argument("name")
    one.add_argument("size", help="WxH")
    one.add_argument("source", help="the video's WxH, native for its rectangle's own size, - for a scene without one")
    one.add_argument("directory")
    one = commands.add_parser("speed")
    one.add_argument("json")
    one.add_argument("text")
    one.add_argument("--compositor", required=True)
    one.add_argument("--plain", required=True)
    one.add_argument("--rounds", type=int, default=20)
    one.add_argument("--scenes", nargs="+", required=True)
    one.add_argument("--sources", nargs="+", required=True, help="each scene's video as scene takes it")
    args = parser.parse_args()
    if args.command == "scene":
        scene(args)
    else:
        speed(args)


if __name__ == "__main__":
    main()
