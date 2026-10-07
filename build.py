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
build.cxxflags += ["-std=c++26"]

build.includes += [
    # the suite's own headers, for the one source compiled from a copy in $(B)
    "$(S)",
    # <plt/...>: the vendored platform layer's headers by their namespaced path
    "$(S)/ext",
    "$(S)/ext/imgui",
    "$(B)/shaders",
]

# ImGui's codepoints 32-bit, for every unit that includes it: the text
# editor (ext/textedit) keeps a file's characters in ImWchar, and nothing
# beyond the Basic Multilingual Plane is lost there
build.cppflags += ["-DIMGUI_USE_WCHAR32"]

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
    build.cflags += ["-mmacosx-version-min=14.0"]
    build.ldflags += ["-mmacosx-version-min=14.0"]
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
    # musl gives a thread 128 KiB of stack unless the program asks for more
    # in PT_GNU_STACK, and lavapipe compiles shaders through LLVM on threads
    # of its own: a merged compositor recursed through 700 frames there and
    # died; glibc's threads get 8 MiB and ignore this
    build.ldflags += ["-Wl,-z,stack-size=8388608"]

# the screenshot tool's encoders, Linux's alone; the scenarios read JPEG XL too
if darwin:
    encoders = []
else:
    jxl = pkg_config("libjxl")
    encoders = [pkg_config("libpng"), jxl]

libstd = import_build(std_build, "libstd.a", extra_cflags=["-Wno-error", "-Wno-deprecated-declarations"])
plt = import_build(
    plt_build,
    "libplt.a",
    extra_cflags=["-Wno-error"],
    extra_cppflags=["-Dno_vendored_std", "-I$(S)/../libstd"],
)
system = dependency(ldflags=["-lm"])
media = [pkg_config(name) for name in ("libavformat", "libavcodec", "libavutil", "libswresample", "openal")]
# Vulkan's canonical `VkFoo info{VK_STRUCTURE_TYPE_FOO}` initialization zeros
# the remaining aggregate fields by design; Clang otherwise diagnoses every
# such declaration under -Wextra.
warning_flags = ["-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers"]


# the compositor of every frame, gpu/compose.comp: compose is its own tile
# program in 8x8 groups; compose_layer is the host a video layer that
# compile() makes is merged into for the tiles where the interface lies over
# the video, one 24x24 group a tile, its layer() empty; the tiles the video
# alone covers run compile()'s own kernels
shader_rules = []
for shader, defines in [] if darwin else [
    ("compose", []),
    ("compose_layer", ["-DGROUP=24", "-DLAYER"]),
    ("compose_generic", ["-DGROUP=24", "-DGENERIC", "-DGENERIC_KERNEL"]),
    ("compose_generic_layer", ["-DGROUP=24", "-DGENERIC", "-DLAYER"]),
]:
    shader_rules.append(command(
        name=f"shader_{shader}",
        inputs=["$(S)/gpu/compose.comp", "$(S)/gpu/video_generic.glsl"],
        outputs=[f"$(B)/shaders/{shader}_comp.spv.h"],
        descr='SH',
        cmd=[
            "glslangValidator", "--target-env", "vulkan1.1", "-V", *defines, "-I$(S)/gpu", "$(S)/gpu/compose.comp",
            "--variable-name", f"{shader}_comp_spv", "-o", f"$(B)/shaders/{shader}_comp.spv.h",
        ],
    ))

# the generic video kernel for Metal, the same gpu/video_generic.glsl in
# Metal's spelling, which the Metal renderer in renderer.cpp compiles behind
# its compositor
shader_rules.append(command(
    name="shader_generic_msl",
    inputs=["$(S)/gpu/video_shaders.py", "$(S)/gpu/video_generic.glsl"],
    outputs=["$(B)/shaders/video_generic.inc"],
    descr="SH",
    cmd=["python3", "$(S)/gpu/video_shaders.py", "msl", "$(S)/gpu/video_generic.glsl", "$(B)/shaders/video_generic.inc"],
))


# the player's color conversion is compiled at run time (shader.cpp) from
# each frame's facts; video_shaders.py renders the storage layouts and the
# codes of gpu/tables.py for the player to match as codes.inc, the data
# codes.cpp includes for the tables codes.h declares
codes = command(
    name="codes",
    inputs=["$(S)/gpu/video_shaders.py", "$(S)/gpu/tables.py"],
    outputs=["$(B)/shaders/codes.inc"],
    descr="SH",
    cmd=["python3", "$(S)/gpu/video_shaders.py", "codes", "$(B)/shaders/codes.inc"],
)


# ImGui's core; the compositors of renderer.cpp draw what it lists, in
# place of its renderer backends
imgui = library(
    name="imgui",
    srcs=build.glob("$(S)/ext/imgui/*.cpp"),
    deps=platform_deps,
)

# the text editor widget of im edit (ext/textedit), a Dear ImGui widget of
# its own with imgui_internal.h behind it
textedit = library(
    name="textedit",
    srcs=["$(S)/ext/textedit/TextEditor.cpp"],
    includes=["$(S)/ext/textedit"],
    public_cppflags=["-I$(S)/ext/textedit"],
    deps=[imgui],
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

# The runtime's sources and headers come along into the generated tree: one
# include directory, and the shards see the runtime of the wasm2c that made
# them. The runtime is compiled once, into the decoder's library; the other
# modules' libraries carry only their own C and link against it.
decode_runtime = sorted({
    *glob.glob(os.path.join(wasm_rt, "wasm-rt*")),
    *glob.glob(os.path.join(wasm_rt_header, "wasm-rt*.h")),
})

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


# A pure wasm module as a library of the binary: wasm2c turns it into C in
# the given number of shards, under the module's name. Many MB of generated
# C: its warnings are the generator's, and debug info for it would outweigh
# the binary; the exception a trap becomes unwinds through its frames.
# Every source and header here is an output of the one node, so a source
# names no inputs: naming the headers too repeats the producer among a
# shard's dependencies, and the runner then keeps a second copy of every
# shard for the scenarios, whose dependency on im_test lists it once
def wasm_library(name, module, shards, runtime):
    out_dir = f"$(B)/{name}"
    sources = [f"{out_dir}/{name}_{i}.c" for i in range(shards)]
    headers = [f"{out_dir}/{name}.h", f"{out_dir}/{name}-impl.h"]
    runtime_files = [f"{out_dir}/{os.path.basename(path)}" for path in decode_runtime]
    generated = command(
        name=f"{name}_c",
        inputs=[module],
        outputs=[*sources, *headers, *runtime_files],
        cmd=[
            ["wasm2c", module, "--module-name", name, "--num-outputs", str(shards), "-o", f"{out_dir}/{name}.c"],
            ["cp", *decode_runtime, f"{out_dir}/"],
        ],
        # another wabt generates other C and ships another runtime
        env={"WABT_VERSION": wabt_version},
        descr="WC",
    )
    return library(
        name=name,
        srcs=[*sources, *[path for path in runtime_files if runtime and path.endswith(".c")]],
        cflags=["-g0", "-w", "-fexceptions"],
        cppflags=decode_defines,
        includes=[out_dir],
        public_cppflags=[f"-I{out_dir}", *decode_defines],
        deps=[generated],
    )

# the image decoder with the runtime, then the page engines: PDFium
# (ext/pdf) and DjVuLibre (ext/djvu), the reader's, with the same
# exports under their own prefix (see their READMEs), and the MIME
# engine
decode = wasm_library("decode", "$(S)/ext/decode/decode.wasm", 16, True)
pdf = wasm_library("pdf", "$(S)/ext/pdf/pdf.wasm", 8, False)
djvu = wasm_library("djvu", "$(S)/ext/djvu/djvu.wasm", 4, False)
# libmagic (ext/magic), the viewer's MIME type of a file; under the name
# mime, as the realm may carry libmagic's own magic.h
mime = wasm_library("mime", "$(S)/ext/magic/magic.wasm", 4, False)


# Linux builds every tool over Vulkan; macOS the runtime's tools over Metal.
# renderer.cpp holds both renderers under one #if; its Metal half is
# Objective-C++, which clang compiles by the .mm extension alone, so macOS
# compiles the file through a copy named renderer.mm. A generated source
# is not scanned for its includes; the original is, as the copy's input,
# and names the generated video_generic.inc the Metal half includes
wayland_only = ["screenshot.cpp", "chaos_monkey.cpp"]
renderer_source = "$(S)/renderer.cpp"
if darwin:
    command(
        name="renderer_mm",
        inputs=["$(S)/renderer.cpp"],
        outputs=["$(B)/renderer.mm"],
        cmd=["cp", "$(S)/renderer.cpp", "$(B)/renderer.mm"],
        descr="CP",
    )
    renderer_source = {"src": "$(B)/renderer.mm", "inputs": ["$(S)/renderer.cpp"]}
im_sources = [renderer_source if os.path.basename(path) == "renderer.cpp" else path for path in build.glob("$(S)/*.cpp") if not (darwin and os.path.basename(path) in wayland_only)]
if darwin:
    warning_flags = [*warning_flags, "-fobjc-arc", "-fblocks"]
# the vendored libraries' own dependencies come along by name: an imported
# graph hands over its archive, not what the archive wants linked
im_deps = [
    *shader_rules, codes, imgui, textedit, pdf, djvu, mime, decode, plt, libstd,
    *platform_deps, *encoders, *media, system,
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
    srcs=["$(S)/tst/renderer.cpp", renderer_source, "$(S)/ui.cpp", "$(S)/error.cpp", "$(S)/number.cpp", "$(S)/timing.cpp", *(["$(S)/tst/renderer_metal.mm"] if darwin else [])],
    cflags=warning_flags,
    deps=im_deps,
)

# the video shaders against a CPU model of H.273, through the renderer's
# own calls: every layout, matrix, chroma location, transfer and primaries
video_test = program(
    name="video_test",
    output="$(B)/e2e/video_test",
    srcs=["$(S)/tst/video.cpp", "$(S)/shader.cpp", "$(S)/codes.cpp", renderer_source, "$(S)/ui.cpp", "$(S)/error.cpp", "$(S)/number.cpp", "$(S)/timing.cpp"],
    cflags=warning_flags,
    deps=im_deps,
)

# dev/video_bench compiles the player's video shaders through this
video_shader = program(
    name="video_shader",
    output="$(B)/dev/video_shader",
    srcs=["$(S)/dev/video_bench/emit.cpp", "$(S)/shader.cpp", "$(S)/codes.cpp", "$(S)/error.cpp"],
    cflags=warning_flags,
    deps=[codes, imgui, libstd],
)

# the headless ImGui frames compositor_speed times the compositor over
compositor_dump = program(
    name="compositor_dump",
    output="$(B)/dev/compositor_dump",
    srcs=["$(S)/dev/compositor/dump.cpp", "$(S)/error.cpp"],
    cflags=warning_flags,
    deps=[imgui, libstd],
)

imgui_frames_test = program(
    name="imgui_frames_test",
    output="$(B)/e2e/imgui_frames_test",
    srcs=["$(S)/tst/imgui_frames.cpp"],
    cflags=warning_flags,
    deps=[imgui, *platform_deps, system],
)


tools = ["view", "play", "read", "edit", "ui"] if darwin else ["screenshot", "view", "play", "read", "edit", "ui"]

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
# The compositor and the devices are Wayland's: macOS runs the scenarios
# that drive the renderer's own helpers, each in a window of its own, and
# tst/session.py starts no compositor there.
# the fixture and the runner: any change to the harness re-runs every scenario
harness = ["$(S)/tst/session.py", "$(S)/dev/run_test.py"]
if darwin:
    helpers = [renderer_test, video_test, imgui_frames_test]
    scenarios = [f"$(S)/tst/{name}.py" for name in ("renderer_pixels", "renderer_hdr", "video", "imgui_frames")]
else:
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
    helpers = [devices, jxl_dump, device_uuid, renderer_test, video_test, imgui_frames_test]
    scenarios = sorted(set(build.glob("$(S)/tst/*.py")) - set(harness))


# -Dshard=K/N splits the scenarios into N slices by a hash of the name, so
# CI jobs can run them side by side; the slice a scenario falls in does not
# move when others are added
shard_index, shard_count = (int(part) for part in flags.shard.split("/")) if flags.shard else (0, 1)
# a scenario with buckets becomes that many test nodes, each running the
# checks whose names hash into its bucket
buckets = {"video": 64}

runs = []
for scenario in scenarios:
    base = os.path.basename(scenario)[:-len(".py")]
    count = buckets.get(base, 0)
    runs += [(scenario, f"{base}_{k}", f"{k}/{count}") for k in range(count)] if count else [(scenario, base, "")]

test_nodes = []
test_verdicts = []
for scenario, name, bucket in runs:
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
    if bucket:
        cmd += ["--bucket", bucket]
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



# ---- the player's video next to libplacebo ----------------------------------
# `./build video_quality` scores what the player draws (its video programs
# and the compositor, dev/compositor/harness.c) and libplacebo's
# presets against every corpus picture at scale factors from 0.1 to 10, in
# the frame formats decoders hand out (dev/video_bench/quality.py tells how);
# `./build video_speed` times videos of the usual sizes drawn into the usual
# windows and displays, on its own: it needs none of the pictures. A quality
# node is one variant of one format at one factor over every picture, so a
# change to our shader redraws ours alone; frames, renders and metrics stay
# in the node, only the scores leave it. The speed matrix makes its frames
# and shaders side by side, a node to a format, and times in one node after
# them, one case after another: on an APU a busy CPU slows the GPU down, and
# processes timing side by side stretch each other's numbers. Linux only;
# without libplacebo, ours alone
if not darwin:
    libplacebo = pkg_config("libplacebo", required=False)
    bench_dir = "$(B)/dev/video_bench"
    bench_script = "$(S)/dev/video_bench/quality.py"
    bench_shader_inputs = [bench_script, "$(S)/dev/video_bench/bench.py", "$(S)/dev/compositor/bench.py", "$(S)/gpu/video_shaders.py", "$(S)/gpu/tables.py"]
    corpus_tool = program(
        name="corpus",
        output=f"{bench_dir}/corpus",
        srcs=["$(S)/dev/video_bench/corpus.c"],
        cflags=warning_flags,
        deps=[system],
    )
    compositor_tool = program(
        name="compositor",
        output=f"{bench_dir}/compositor",
        srcs=["$(S)/dev/compositor/harness.c"],
        cflags=warning_flags,
        deps=[vulkan, system],
    )
    compositor_hosts = command(
        name="compositor_hosts",
        inputs=["$(S)/gpu/compose.comp", "$(S)/gpu/video_generic.glsl"],
        outputs=[f"{bench_dir}/compose_plain.spv", f"{bench_dir}/compose_layer.spv", f"{bench_dir}/compose_generic.spv", f"{bench_dir}/compose_generic_layer.spv"],
        cmd=[
            ["glslangValidator", "--quiet", "--target-env", "vulkan1.1", "-V", "$(S)/gpu/compose.comp", "-o", f"{bench_dir}/compose_plain.spv"],
            ["glslangValidator", "--quiet", "--target-env", "vulkan1.1", "-V", "-DGROUP=24", "-DLAYER", "$(S)/gpu/compose.comp", "-o", f"{bench_dir}/compose_layer.spv"],
            ["glslangValidator", "--quiet", "--target-env", "vulkan1.1", "-V", "-DGROUP=24", "-DGENERIC", "-DGENERIC_KERNEL", "-I$(S)/gpu", "$(S)/gpu/compose.comp", "-o", f"{bench_dir}/compose_generic.spv"],
            ["glslangValidator", "--quiet", "--target-env", "vulkan1.1", "-V", "-DGROUP=24", "-DGENERIC", "-DLAYER", "-I$(S)/gpu", "$(S)/gpu/compose.comp", "-o", f"{bench_dir}/compose_generic_layer.spv"],
        ],
        descr="SH",
    )
    placebo_tool = program(
        name="placebo",
        output=f"{bench_dir}/placebo",
        srcs=["$(S)/dev/video_bench/placebo.c"],
        cflags=warning_flags,
        deps=[libplacebo, vulkan, system],
    ) if libplacebo else None
    presets = ["fast", "default", "high_quality"] if libplacebo else []
    ours_tools = ["--compositor", f"{bench_dir}/compositor", "--plain", f"{bench_dir}/compose_plain.spv"]
    shader_tools = [f"{bench_dir}/corpus", "$(B)/dev/video_shader", f"{bench_dir}/compose_layer.spv"]
    generic_tools = [f"{bench_dir}/compose_generic.spv", f"{bench_dir}/compose_generic_layer.spv"]
    generic_files = ["generic/facts.bin", "generic/generic.spv", "generic/generic_layer.spv"]
    video_formats = ["yuv420p", "nv12", "yuv420p10le", "p010le", "yuv444p", "bgra"]

    # a factor above 1 draws the crop shrunk by it back to the crop's size,
    # one below 1 shrinks the crop itself, against the crop shrunk exactly
    video_factors = [0.1, 0.15, 0.2, 0.25, 0.3, 0.4, 0.5, 0.6, 0.7, 0.75, 0.8, 0.9, 1, 1.1, 1.2, 1.25, 1.333, 1.5, 1.75, 2, 2.5, 3, 4, 5, 6, 8, 10]
    crop = (960, 540)

    def scaled(factor):
        if factor >= 1:
            return (round(crop[0] / factor), round(crop[1] / factor)), crop
        return crop, (round(crop[0] * factor), round(crop[1] * factor))

    quality_dir = "$(B)/video_quality"
    pictures = [os.path.basename(path)[:-len(".jxl")] for path in build.glob("$(S)/dev/video_bench/corpus/*.jxl")]
    shrinking = [factor for factor in video_factors if factor < 1]
    truth_of = {}
    decoded_nodes = []
    shrunk_nodes = []
    for name in pictures:
        decoded = f"{quality_dir}/truth/{name}.ppm"
        jxl = f"$(S)/dev/video_bench/corpus/{name}.jxl"
        truth = command(
            name=f"video_truth_{name}",
            inputs=[jxl],
            outputs=[decoded],
            cmd=["djxl", "--quiet", "--num_threads=1", jxl, decoded],
            descr="JX",
        )
        shrunk = {factor: f"{quality_dir}/truth/{name}_{factor:g}.ppm" for factor in shrinking}
        decoded_nodes.append(truth)
        shrunk_nodes.append(command(
            name=f"video_shrunk_{name}",
            outputs=list(shrunk.values()),
            deps=[corpus_tool, truth],
            cmd=[[f"{bench_dir}/corpus", "shrink", decoded, path, *map(str, scaled(factor)[1])] for factor, path in shrunk.items()],
            descr="SR",
        ))
        truth_of[name] = (decoded, shrunk)

    renders = []
    for fmt in video_formats:
        for factor in video_factors:
            tag = f"{fmt}_{factor:g}"
            sizes = [*map(str, scaled(factor)[0]), *map(str, scaled(factor)[1])]
            shaders = f"{quality_dir}/shaders/{tag}"
            shader_node = command(
                name=f"video_shader_{tag}",
                inputs=bench_shader_inputs,
                outputs=[*(f"{shaders}/{tiles}.spv" for tiles in ("inside", "edge", "mixed")), *(f"{shaders}/{file}" for file in generic_files)],
                deps=[corpus_tool, video_shader, compositor_hosts],
                cmd=["python3", bench_script, "shader", *shader_tools, *generic_tools, fmt, *sizes, shaders],
                descr="SH",
            )
            triples = [word for name in pictures for word in (name, truth_of[name][0], truth_of[name][1].get(factor, truth_of[name][0]))]
            for variant, deps, tools in [
                ("ours", [compositor_tool, compositor_hosts, shader_node], ["--layers", shaders, *ours_tools]),
                ("generic", [compositor_tool, compositor_hosts, shader_node], ["--layers", f"{shaders}/generic", *ours_tools]),
                *((f"placebo_{preset}", [placebo_tool], ["--placebo", f"{bench_dir}/placebo"]) for preset in presets),
            ]:
                out = f"{quality_dir}/scores/{tag}_{variant}.json"
                renders.append(command(
                    name=f"video_quality_{tag}_{variant}",
                    inputs=[bench_script],
                    outputs=[out],
                    deps=[corpus_tool, *deps, *decoded_nodes, *(shrunk_nodes if factor < 1 else [])],
                    cmd=["python3", bench_script, "render", out, variant, fmt, str(factor), *sizes, *triples, "--corpus", f"{bench_dir}/corpus", *tools],
                    descr="RN",
                ))
    group("video_quality", command(
        name="video_quality_report",
        inputs=[bench_script],
        outputs=[f"{quality_dir}/quality.json", f"{quality_dir}/quality.txt"],
        deps=renders,
        cmd=["python3", bench_script, "report", f"{quality_dir}/quality.json", f"{quality_dir}/quality.txt", *(render.outputs[0] for render in renders)],
        descr="QR",
    ))

    videos = [(854, 480), (1280, 720), (1920, 1080), (2560, 1440), (3840, 2160)]
    screens = [(1280, 720), (1824, 1026), (1920, 1080), (2560, 1440), (3840, 2160)]
    pairs = [f"{video[0]}x{video[1]}:{screen[0]}x{screen[1]}" for video in videos for screen in screens]
    speed_dir = "$(B)/video_speed"
    speed_cases = []
    for fmt in video_formats:
        directory = f"{speed_dir}/{fmt}"
        speed_cases.append(command(
            name=f"video_speed_cases_{fmt}",
            inputs=bench_shader_inputs,
            outputs=[
                *(f"{directory}/cases/{pair.replace(':', '_')}/{file}" for pair in pairs for file in ("size", "placebo.txt", "frame.bin", "inside.spv", "edge.spv", "mixed.spv", *generic_files)),
                *(f"{directory}/frames/{video[0]}x{video[1]}.bin" for video in videos),
            ],
            deps=[corpus_tool, video_shader, compositor_hosts],
            cmd=["python3", bench_script, "cases", *shader_tools, *generic_tools, fmt, directory, *pairs],
            descr="SC",
        ))
    group("video_speed", command(
        name="video_speed_report",
        inputs=[bench_script],
        outputs=[f"{speed_dir}/speed.json", f"{speed_dir}/speed.txt"],
        deps=[compositor_tool, compositor_hosts, *([placebo_tool] if libplacebo else []), *speed_cases],
        cmd=[
            "python3", bench_script, "speed", f"{speed_dir}/speed.json", f"{speed_dir}/speed.txt",
            "--formats", *video_formats, "--pairs", *pairs, "--presets", *presets, "--cases", speed_dir,
            *ours_tools, *(["--placebo", f"{bench_dir}/placebo"] if libplacebo else []),
        ],
        descr="SP",
    ))

    # `./build compositor_speed` times the compositor itself over the ImGui
    # scenes compositor_dump makes at 1920x1080, the video of play and of its
    # menu drawn from 1280x720, and play again from its rectangle's own size
    # and from 4K (dev/compositor/bench.py tells how): a node makes each
    # scene's frame and programs, one times them all after them
    compositor_script = "$(S)/dev/compositor/bench.py"
    compositor_dir = "$(B)/compositor_speed"
    scenes = [("demo", "-"), ("play", "1280x720"), ("native", "native"), ("shrunk", "3840x2160"), ("menu", "1280x720"), ("view", "-")]
    scene_nodes = []
    for scene, source in scenes:
        directory = f"{compositor_dir}/scenes/{scene}"
        scene_nodes.append(command(
            name=f"compositor_scene_{scene}",
            inputs=bench_shader_inputs,
            outputs=[f"{directory}/frame.bin", *(f"{directory}/{file}" for file in ("data.bin", "inside.spv", "edge.spv", "mixed.spv") if source != "-")],
            deps=[compositor_dump, corpus_tool, video_shader, compositor_hosts],
            cmd=["python3", compositor_script, "scene", "$(B)/dev/compositor_dump", *shader_tools, scene, "1920x1080", source, directory],
            descr="CS",
        ))
    group("compositor_speed", command(
        name="compositor_speed_report",
        inputs=[compositor_script],
        outputs=[f"{compositor_dir}/speed.json", f"{compositor_dir}/speed.txt", *(f"{compositor_dir}/{scene}/composed.ppm" for scene, _ in scenes)],
        deps=[compositor_tool, compositor_hosts, *scene_nodes],
        cmd=["python3", compositor_script, "speed", f"{compositor_dir}/speed.json", f"{compositor_dir}/speed.txt", *ours_tools, "--scenes", *(f"{compositor_dir}/scenes/{scene}" for scene, _ in scenes), "--sources", *(source for _, source in scenes)],
        descr="SP",
    ))
