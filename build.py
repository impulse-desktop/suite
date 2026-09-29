import build
import build.flags as flags
import os


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
    "filter": {"descr": "glob restricting which scenarios run", "default": "*"},
    "runtime": {"descr": "a short dir for the scenarios' runtime dirs (Wayland sockets)", "default": ""},
})


wayland_client = pkg_config("wayland-client")
xkb = pkg_config("xkbcommon")
vulkan = pkg_config("vulkan")
png = pkg_config("libpng")
jxl = pkg_config("libjxl")
display_info = pkg_config("libdisplay-info")

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
    ("screenshot_scene", "frag"),
    ("screenshot_output", "frag"),
]:
    shader_rules.append(command(
        name=f"shader_{shader}",
        inputs=[f"$(S)/{shader}.{stage}"],
        outputs=[f"$(B)/shaders/{shader}.spv.h"],
        descr='SH',
        cmd=[
            "glslangValidator", "-V", f"$(S)/{shader}.{stage}",
            "--variable-name", f"{shader}_spv", "-o", f"$(B)/shaders/{shader}.spv.h",
        ],
    ))


imgui = library(
    name="imgui",
    srcs=build.glob("$(S)/ext/imgui/*.cpp"),
    deps=[vulkan],
)


im_sources = build.glob("$(S)/*.cpp")
# the vendored libraries' own dependencies come along by name: an imported
# graph hands over its archive, not what the archive wants linked
im_deps = [
    *shader_rules, imgui, plt, libstd,
    wayland_client, xkb, vulkan, png, jxl, display_info, system,
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
# Each tst/scenarios/*.py drives im_test as a client of its own headless
# Sway through the driver's virtual devices (tst/support/devices.cpp) and
# checks what it draws and what it leaves on disk; tst/run.py runs them all
# and writes results.json. -Dfilter=GLOB picks scenarios, -Druntime=DIR
# keeps their Wayland sockets under a short path.
e2e_protocols = []
e2e_protocol_headers = []
for xml, name in [
    ("wlr-virtual-pointer-unstable-v1", "virtual-pointer"),
    ("virtual-keyboard-unstable-v1", "virtual-keyboard"),
]:
    source = f"$(S)/tst/support/{xml}.xml"
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
    srcs=[{"src": "$(S)/tst/support/devices.cpp", "inputs": e2e_protocol_headers}],
    cflags=["-I$(B)/e2e-protocol"],
    deps=[*e2e_protocols, wayland_client, xkb],
)

e2e_cmd = [
    "python3", "$(S)/tst/run.py",
    "--binary", "$(B)/im_test",
    "--devices", "$(B)/e2e/devices",
    "--artifacts", "$(B)/e2e-results",
    "--filter", flags.filter or "*",
]
if flags.runtime:
    e2e_cmd += ["--runtime", flags.runtime]

e2e = command(
    name="e2e",
    inputs=sorted(build.glob("$(S)/tst/*.py") + build.glob("$(S)/tst/scenarios/*.py")),
    outputs=["$(B)/e2e-results/results.json"],
    deps=[im_test, devices],
    cmd=[e2e_cmd],
    descr="TS",
    color="cyan",
)
group("test", e2e)
