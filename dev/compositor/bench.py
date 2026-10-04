#!/usr/bin/env python3

"""Composes headless ImGui frames (compositor_dump) three ways and compares
them: the player's pipeline now (the video kernel at its tile of 16 into a
cleared target, then the hardware ImGui pass in sRGB-coded blending), the
same blending in linear light (sRGB views of the target and textures), and
the tiled compute compositor of gpu/compose.comp with the video layer that
compile() makes merged into it. Prints the GPU and CPU time of each, the
compositor's tile programs and how far its pixels are from both.

The video is a yuv420p BT.709 frame of the video bench's chart, scaled into
the scene's video rectangle by the shaders the video_shader program makes.

  ix run set/pg/libs bin/glslang lib/vulkan/drivers --vulkan=amd/radv -- \\
      python3 dev/compositor/bench.py --dump BUILD/dev/compositor_dump --compiler BUILD/dev/video_shader [--work DIR] [--size 1920x1080] [--source 1280x720] [SCENE...]
"""

import argparse
import importlib.util
import json
import os
import struct
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
SUITE = HERE.parent.parent
spec = importlib.util.spec_from_file_location("video_bench", HERE.parent / "video_bench" / "bench.py")
video = importlib.util.module_from_spec(spec)
spec.loader.exec_module(video)


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
    if op in (71, 72):
        return [0]
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
        if op in TYPES or op in CONSTANTS or op == 59:
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


def programs(compiler, work, rect, source, host):
    name, layout, components = video.layout_of("yuv420p")
    case = dict(video.SIZES, format="yuv420p", subsampling=(1, 1), matrix=1, range=1, transfer=1, primaries=1, location=1, output="sdr")
    case.update(source=source, target=(rect[2] - rect[0], rect[3] - rect[1]), filter="lanczos", content="chart", origin=(rect[0], rect[1]))
    data = work / ("video_%dx%d.bin" % source)
    planes = data.with_suffix(".json")
    if not planes.exists():
        frame, offsets, lines = video.frame(case, layout, components)
        data.write_bytes(frame)
        planes.write_text(json.dumps([offsets, lines]))
    offsets, lines = json.loads(planes.read_text())

    def compiled(**facts):
        arguments = video.facts(dict(case, **facts), layout, components, offsets, lines)
        return subprocess.run([compiler, *arguments], check=True, capture_output=True).stdout

    legacy = work / ("legacy_%d_%d_%d_%d.spv" % tuple(rect))
    legacy.write_bytes(compiled(stage="kernel", tile=16, dither=8))
    layer = work / ("layer_%d_%d_%d_%d.spv" % tuple(rect))
    layer.write_bytes(merge(host.read_bytes(), compiled(stage="layer", tile=24, dither=0)))
    return [layer, legacy, data]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dump", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--work", default=str(SUITE / ".build" / "compositor"))
    parser.add_argument("--size", default="1920x1080")
    parser.add_argument("--source", default="1280x720")
    parser.add_argument("--rounds", type=int, default=20)
    parser.add_argument("scenes", nargs="*", default=["demo", "play", "menu", "view"])
    args = parser.parse_args()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    flags = " ".join(os.environ.get(name, "") for name in ("CPPFLAGS", "CFLAGS"))
    libraries = " ".join(os.environ.get(name, "") for name in ("CTRFLAGS", "LDFLAGS"))
    harness = work / "harness"
    subprocess.run(f"cc -O2 {flags} -o {harness} {HERE / 'harness.c'} $(pkg-config --cflags --libs vulkan) {libraries} -lm", shell=True, check=True)
    for name in ("imgui.vert", "imgui.frag"):
        subprocess.run(["glslangValidator", "--quiet", "-V", str(HERE / name), "-o", str(work / (name + ".spv"))], check=True)
    subprocess.run(["glslangValidator", "--quiet", "-V", "-DLINEAR", str(HERE / "imgui.frag"), "-o", str(work / "linear.frag.spv")], check=True)
    plain, host = work / "compose_plain.spv", work / "compose_layer.spv"
    subprocess.run(["glslangValidator", "--quiet", "--target-env", "vulkan1.1", "-V", str(SUITE / "gpu" / "compose.comp"), "-o", str(plain)], check=True)
    subprocess.run(["glslangValidator", "--quiet", "--target-env", "vulkan1.1", "-V", "-DGROUP=24", "-DLAYER", str(SUITE / "gpu" / "compose.comp"), "-o", str(host)], check=True)
    width, height = args.size.split("x")
    source = tuple(int(value) for value in args.source.split("x"))
    for scene in args.scenes:
        out = work / scene
        out.mkdir(exist_ok=True)
        frame = out / "frame.bin"
        with frame.open("wb") as handle:
            subprocess.run([args.dump, scene, width, height], check=True, stdout=handle)
        rect = rectangle(frame)
        extra = [] if rect is None else [str(path) for path in programs(args.compiler, work, rect, source, host)]
        print(f"== {scene} {args.size}" + ("" if rect is None else f", video {rect[2] - rect[0]}x{rect[3] - rect[1]} from {args.source}"), flush=True)
        subprocess.run([str(harness), str(work / "imgui.vert.spv"), str(work / "imgui.frag.spv"), str(work / "linear.frag.spv"), str(plain), str(args.rounds), str(frame), str(out), *extra], check=True)


if __name__ == "__main__":
    main()
