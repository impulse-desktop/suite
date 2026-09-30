import build
import build.flags as flags
import fnmatch
import glob
import hashlib
import os
import shutil
import subprocess


std_build = os.path.join("ext", "libstd", "build.py")
plt_build = os.path.join("ext", "plt", "build.py")


build.cflags += ["-O2", "-g"]
build.cxxflags += ["-std=c++23"]

build.includes += [
    # <plt/...>: the vendored platform layer's headers by their namespaced path
    "$(S)/ext",
    "$(S)/ext/imgui",
    "$(B)/shaders",
]

flags.allow({
    "filter": {"descr": "glob restricting which scenarios build", "default": ""},
    "shard": {"descr": "K/N: run only the K-th of N stable slices of the scenarios (0-based)", "default": ""},
    "runtime": {"descr": "a short dir for the scenarios' runtime dirs (Wayland sockets)", "default": ""},
    "evidence": {"descr": "a dir that keeps a failed scenario's captures and logs, under its name", "default": ""},
})


wayland_client = pkg_config("wayland-client")
xkb = pkg_config("xkbcommon")
vulkan = pkg_config("vulkan")
png = pkg_config("libpng")
jxl = pkg_config("libjxl")

libstd = import_build(std_build, "libstd.a", extra_cflags=["-Wno-error"])
plt = import_build(
    plt_build,
    "libplt.a",
    extra_cflags=["-Wno-error"],
    extra_cppflags=["-Dno_vendored_std", "-I$(S)/../libstd"],
)
system = dependency(ldflags=["-lm"])
# Vulkan's canonical `VkFoo info{VK_STRUCTURE_TYPE_FOO}` initialization zeros
# the remaining aggregate fields by design; Clang otherwise diagnoses every
# such declaration under -Wextra.
warning_flags = ["-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers"]


# shaders are named after the .cpp that creates their pipeline;
# fullscreen.vert is the shared fullscreen-triangle vertex stage
shader_rules = []
for shader, stage in [
    ("fullscreen", "vert"),
    ("gpu_scene", "frag"),
    ("gpu_image", "vert"),
    ("gpu_image", "frag"),
    ("gpu_output", "frag"),
]:
    shader_rules.append(command(
        name=f"shader_{shader}_{stage}",
        inputs=[f"$(S)/{shader}.{stage}"],
        outputs=[f"$(B)/shaders/{shader}_{stage}.spv.h"],
        descr='SH',
        cmd=[
            "glslangValidator", "-V", f"$(S)/{shader}.{stage}",
            "--variable-name", f"{shader}_{stage}_spv", "-o", f"$(B)/shaders/{shader}_{stage}.spv.h",
        ],
    ))


imgui = library(
    name="imgui",
    srcs=build.glob("$(S)/ext/imgui/*.cpp"),
    deps=[vulkan],
)


# ---- the image decoder: ImageMagick as a pure wasm module, compiled to C --
# ext/decode/decode.wasm (ImageMagick and its coders built for wasm32-none,
# from pg83/decode) imports nothing and exports decode(data, len, name,
# name_len) -> {u32 width; u32 height; u8 rgba[]} or 0, with malloc and free.
# wasm2c (wabt) turns it into C with every memory access checked, and with
# the runtime it ships next to itself the module becomes an ordinary library
# of the one binary: no interpreter, no JIT, and no way out of its own memory
# for a coder that reads a hostile file. The runtime's public header sits in
# include/ (a distribution's wabt) or with the runtime's sources (ix's).
wasm2c = shutil.which("wasm2c")
if wasm2c is None:
    raise RuntimeError("wasm2c (wabt) is required: the image decoder is compiled from ext/decode/decode.wasm")
wabt_prefix = os.path.dirname(os.path.dirname(os.path.realpath(wasm2c)))
wasm_rt = os.path.join(wabt_prefix, "share", "wabt", "wasm2c")
wasm_rt_header = next(
    (d for d in (os.path.join(wabt_prefix, "include"), wasm_rt) if os.path.exists(os.path.join(d, "wasm-rt.h"))),
    None,
)
if wasm_rt_header is None or not os.path.exists(os.path.join(wasm_rt, "wasm-rt-impl.c")):
    raise RuntimeError(f"the wasm2c runtime (wasm-rt.h, wasm-rt-impl.c) is missing under {wabt_prefix}")
wabt_version = subprocess.check_output([wasm2c, "--version"], text=True).strip()

decode_dir = "$(B)/decode"
decode_shards = 16
# the runtime's sources and headers come along into the generated tree: one
# include directory, and the shards see the runtime of the wasm2c that made them
decode_runtime = sorted({
    *glob.glob(os.path.join(wasm_rt, "wasm-rt*")),
    *glob.glob(os.path.join(wasm_rt_header, "wasm-rt*.h")),
})
decode_runtime_files = [f"{decode_dir}/{os.path.basename(path)}" for path in decode_runtime]
decode_headers = [f"{decode_dir}/decode.h", f"{decode_dir}/decode-impl.h"]
decode_sources = [f"{decode_dir}/decode_{i}.c" for i in range(decode_shards)]
decode_c = command(
    name="decode_c",
    inputs=["$(S)/ext/decode/decode.wasm"],
    outputs=[*decode_sources, *decode_headers, *decode_runtime_files],
    cmd=[
        ["wasm2c", "$(S)/ext/decode/decode.wasm", "--module-name", "decode", "--num-outputs", str(decode_shards), "-o", f"{decode_dir}/decode.c"],
        ["cp", *decode_runtime, f"{decode_dir}/"],
    ],
    # another wabt generates other C and ships another runtime
    env={"WABT_VERSION": wabt_version},
    descr="WC",
)

# 170 MB of generated C: its warnings are the generator's, and debug info
# for it would outweigh the binary
decode = library(
    name="decode",
    srcs=[
        {"src": src, "inputs": [*decode_headers, *decode_runtime_files]}
        for src in [*decode_sources, *[path for path in decode_runtime_files if path.endswith(".c")]]
    ],
    cflags=["-g0", "-w"],
    includes=[decode_dir],
    public_cppflags=[f"-I{decode_dir}"],
    deps=[decode_c],
)


im_sources = build.glob("$(S)/*.cpp")
# the vendored libraries' own dependencies come along by name: an imported
# graph hands over its archive, not what the archive wants linked
im_deps = [
    *shader_rules, imgui, decode, plt, libstd,
    wayland_client, xkb, vulkan, png, jxl, system,
]

# one binary, every tool: `im screenshot ...`, and a link named after the
# tool (imscreenshot) that runs it under its own name
im = program(
    name="im",
    srcs=im_sources,
    cflags=warning_flags,
    deps=im_deps,
)

# the same tools rebuilt for the tests (IM_FOR_TESTS): the fault seam and
# the trace lines compile in, and frame pointers are kept so a hung tool's
# stack can be walked
im_test = program(
    name="im_test",
    output="$(B)/im_test",
    srcs=im_sources,
    cppflags=["-DIM_FOR_TESTS=1"],
    cflags=[*warning_flags, "-fno-omit-frame-pointer"],
    deps=im_deps,
)

tools = ["screenshot"]

links = command(
    name="links",
    outputs=[f"$(B)/im{tool}" for tool in tools],
    deps=[im],
    cmd=[["ln", "-sfn", "im", f"$(B)/im{tool}"] for tool in tools],
    descr="LN",
)


install(im, links)


# ---- the scenarios: the tools under a real compositor ----------------------
# Each tst/*.py but the fixture (session.py) drives im_test as a client of
# its own headless Sway through the driver's virtual devices
# (tst/devices.cpp) and checks what it draws and what it leaves on disk.
# Each scenario is a command node: dev/run_test.py runs it and writes a
# JSON verdict, always exiting 0 so a failure does not abort the graph. One
# final `test` node depends on every scenario node, reads the verdicts and
# fails `./build test`. -Dfilter=GLOB restricts which scenarios build,
# -Druntime=DIR keeps their Wayland sockets under a short path, and
# -Devidence=DIR keeps what a failed one captured, outside the graph.
e2e_protocols = []
e2e_protocol_headers = []
for xml, name in [
    ("wlr-virtual-pointer-unstable-v1", "virtual-pointer"),
    ("virtual-keyboard-unstable-v1", "virtual-keyboard"),
]:
    source = f"$(S)/tst/{xml}.xml"
    header = f"$(B)/e2e-protocol/{name}-client.h"
    code = f"$(B)/e2e-protocol/{name}-code.h"
    e2e_protocol_headers += [header, code]
    e2e_protocols.append(command(
        name=f"protocol_{name}",
        inputs=[source],
        outputs=[header, code],
        cmd=[
            ["wayland-scanner", "client-header", source, header],
            ["wayland-scanner", "private-code", source, code],
        ],
        cflags=["-I$(B)/e2e-protocol"],
        descr="WL",
    ))

devices = program(
    name="devices",
    output="$(B)/e2e/devices",
    srcs=[{"src": "$(S)/tst/devices.cpp", "inputs": e2e_protocol_headers}],
    cflags=["-I$(B)/e2e-protocol"],
    deps=[*e2e_protocols, wayland_client, xkb],
)

# the scenarios read a saved JPEG XL through this, as the image's own
# code values
jxl_dump = program(
    name="jxl_dump",
    output="$(B)/e2e/jxl_dump",
    srcs=["$(S)/tst/jxl_dump.cpp"],
    deps=[jxl],
)

# and name the Vulkan device a shared buffer is imported on through this
device_uuid = program(
    name="device_uuid",
    output="$(B)/e2e/device_uuid",
    srcs=["$(S)/tst/device_uuid.cpp"],
    deps=[vulkan],
)
helpers = [devices, jxl_dump, device_uuid]

# -Dshard=K/N splits the scenarios into N slices by a hash of the name, so
# CI jobs can run them side by side; the slice a scenario falls in does not
# move when others are added
shard_index, shard_count = (int(part) for part in flags.shard.split("/")) if flags.shard else (0, 1)
# the fixture and the runner: any change to the harness re-runs every scenario
harness = ["$(S)/tst/session.py", "$(S)/dev/run_test.py"]

test_nodes = []
test_verdicts = []
for scenario in sorted(set(build.glob("$(S)/tst/*.py")) - set(harness)):
    name = os.path.basename(scenario)[:-len(".py")]
    if flags.filter and not fnmatch.fnmatch(name, flags.filter):
        continue
    if int(hashlib.sha1(name.encode()).hexdigest(), 16) % shard_count != shard_index:
        continue
    out = f"$(B)/test-results/{name}.json"
    cmd = [
        "python3", "$(S)/dev/run_test.py",
        "--scenario", scenario,
        "--binary", "$(B)/im_test",
        "--helpers", "$(B)/e2e",
        "--out", out,
    ]
    if flags.runtime:
        cmd += ["--runtime", flags.runtime]
    if flags.evidence:
        cmd += ["--evidence", flags.evidence]
    test_verdicts.append(out)
    test_nodes.append(command(
        name=f"test_{name}",
        inputs=[scenario, *harness],
        outputs=[out],
        deps=[im_test, *helpers],
        cmd=cmd,
        descr="TS",
        color="cyan",
    ))

if test_nodes:
    test = command(
        name="test",
        inputs=["$(S)/dev/aggregate_tests.py"],
        outputs=["$(B)/test-results/verdict.txt"],
        deps=test_nodes,
        cmd=[
            "python3", "$(S)/dev/aggregate_tests.py",
            "--out", "$(B)/test-results/verdict.txt",
            *test_verdicts,
        ],
        descr="OK",
        color="light-green",
    )
