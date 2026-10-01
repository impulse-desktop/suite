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


# ---- the platform ----------------------------------------------------------
# Linux draws through Wayland, plt's backend there, with Vulkan, and runs the
# scenarios under a compositor; macOS draws through Cocoa and Metal and only
# builds the viewer and im ui: the compositor the screenshot tool serves is
# not there, and neither are its Vulkan, its encoders and its fault seam
darwin = "apple-darwin" in build.target

if darwin:
    # the SDK's frameworks, as system headers so -Werror leaves them alone,
    # and to the linker; the frameworks Metal and plt's Cocoa backend stand
    # on: the imported plt graph brings its archive, not its link flags
    sdk_frameworks = os.path.join(os.environ["OSX_SDK"], "System", "Library", "Frameworks") if "OSX_SDK" in os.environ else None
    if sdk_frameworks:
        build.cppflags += [f"-iframework{sdk_frameworks}"]
    platform_deps = [dependency(ldflags=[
        *([f"-F{sdk_frameworks}"] if sdk_frameworks else []),
        "-Wl,-framework,AppKit",
        "-Wl,-framework,Carbon",
        "-Wl,-framework,CoreFoundation",
        "-Wl,-framework,CoreGraphics",
        "-Wl,-framework,CoreVideo",
        "-Wl,-framework,Foundation",
        "-Wl,-framework,IOKit",
        "-Wl,-framework,IOSurface",
        "-Wl,-framework,Metal",
        "-Wl,-framework,QuartzCore",
    ])]
else:
    wayland_client = pkg_config("wayland-client")
    xkb = pkg_config("xkbcommon")
    vulkan = pkg_config("vulkan")
    platform_deps = [wayland_client, xkb, vulkan]

# the screenshot tool's encoders, Linux's alone; the scenarios read JPEG XL too
if darwin:
    encoders = []
else:
    jxl = pkg_config("libjxl")
    encoders = [pkg_config("libpng"), jxl]

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
for shader, stage in [] if darwin else [
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


# ImGui's core and the platform's renderer backend: Vulkan's, or Metal's,
# under ARC as upstream builds it
imgui_core = [path for path in build.glob("$(S)/ext/imgui/*.cpp") if not path.endswith("imgui_impl_vulkan.cpp")]
imgui = library(
    name="imgui",
    srcs=[*imgui_core, "$(S)/ext/imgui/imgui_impl_metal.mm"] if darwin else [*imgui_core, "$(S)/ext/imgui/imgui_impl_vulkan.cpp"],
    cflags=["-fobjc-arc", "-fobjc-weak"] if darwin else [],
    deps=platform_deps,
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

# The runtime's shape, the same for the generated C, the runtime and the
# decoder that calls them: every load and store checked against the memory's
# size in the code itself (the alternative, guard pages around a reserved
# range with a SIGSEGV handler to catch the misses, is faster but makes the
# process's memory faults the runtime's business); the memory mapped, so
# its base never moves and the checks stay cheap; a call depth counted
# instead of a stack guard, at a limit well above the forty-odd frames the
# coders were seen to use, so the workers' native stacks stay small; and a
# trap handed to the decoder's own handler (decoder.cpp), which throws it
# as a C++ exception through the module's C frames, instead of the
# runtime's longjmp into its thread-local state; the runtime declares the
# handler by this name.
decode_defines = [
    "-DWASM_RT_USE_MMAP=1",
    "-DWASM_RT_MEMCHECK_BOUNDS_CHECK=1",
    "-DWASM_RT_MEMCHECK_GUARD_PAGES=0",
    "-DWASM_RT_MAX_CALL_STACK_DEPTH=250",
    "-DWASM_RT_TRAP_HANDLER=decodeTrapHandler",
]

# 170 MB of generated C: its warnings are the generator's, and debug info
# for it would outweigh the binary; the exception a trap becomes unwinds
# through its frames. Every source and header here is an output of the one
# node above, so a source names no inputs: naming the headers too repeats
# the producer among a shard's dependencies, and the runner then keeps a
# second copy of every shard for the scenarios, whose dependency on
# im_test lists it once
decode = library(
    name="decode",
    srcs=[*decode_sources, *[path for path in decode_runtime_files if path.endswith(".c")]],
    cflags=["-g0", "-w", "-fexceptions"],
    cppflags=decode_defines,
    includes=[decode_dir],
    public_cppflags=[f"-I{decode_dir}", *decode_defines],
    deps=[decode_c],
)


# Linux builds every tool over Vulkan; macOS the runtime's tools over Metal
wayland_only = ["renderer_vulkan.cpp", "screenshot.cpp", "chaos_monkey.cpp"]
im_sources = [path for path in build.glob("$(S)/*.cpp") if not (darwin and os.path.basename(path) in wayland_only)]
if darwin:
    im_sources.append("$(S)/renderer_metal.mm")
    warning_flags = [*warning_flags, "-fobjc-arc", "-fblocks"]
# the vendored libraries' own dependencies come along by name: an imported
# graph hands over its archive, not what the archive wants linked
im_deps = [
    *shader_rules, imgui, decode, plt, libstd,
    *platform_deps, *encoders, system,
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

renderer_test = program(
    name="renderer_test",
    output="$(B)/e2e/renderer_test",
    srcs=["$(S)/tst/renderer.cpp", "$(S)/renderer.cpp", "$(S)/ui.cpp", "$(S)/error.cpp", "$(S)/number.cpp", "$(S)/timing.cpp", *(["$(S)/renderer_metal.mm", "$(S)/tst/renderer_metal.mm"] if darwin else ["$(S)/renderer_vulkan.cpp"])],
    cflags=warning_flags,
    deps=im_deps,
)

imgui_frames_test = program(
    name="imgui_frames_test",
    output="$(B)/e2e/imgui_frames_test",
    srcs=["$(S)/tst/imgui_frames.cpp"],
    cflags=warning_flags,
    deps=[imgui, *platform_deps, system],
)


tools = ["view", "ui"] if darwin else ["screenshot", "view", "ui"]

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
# Linux only: the compositor and the devices are Wayland's.
if not darwin:
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
    helpers = [devices, jxl_dump, device_uuid, renderer_test, imgui_frames_test]

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
