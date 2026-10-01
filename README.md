# impulse suite

[![CI](https://github.com/impulse-desktop/suite/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/impulse-desktop/suite/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/impulse-desktop/suite/branch/main/graph/badge.svg)](https://app.codecov.io/gh/impulse-desktop/suite/tree/main)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-informational)](STYLE.md)

The desktop tools of the impulse desktop, one binary: `im`. Each tool is a
client drawn with the vendored ImGui, on Vulkan/Wayland on Linux or
Metal/Cocoa on macOS, windowed through the vendored
[plt](https://github.com/pg83/plt) platform layer, and lives in its own process.

```
im screenshot fd:3      # the screenshot editor the compositor spawns
imscreenshot shot.imw   # the same tool, by its link
im view ~/pictures      # the image viewer on a directory
imview photo.jpg        # on one file, among its directory's
```

A link named `im<tool>` runs that tool under its own name; `im <tool>`
does the same from the one binary. Tools so far:

- `screenshot` — crops and saves the image the
  [shell](https://github.com/impulse-desktop/shell) hands over on the
  screenshot chord: a self-describing memfd, or on KMS the scanout dma-buf
  itself, encoded to PNG or JPEG XL, SDR or HDR.
- `view` — shows images: a directory's, by name, or the named files. The
  list on the left is a column of thumbnails, each as wide as the list;
  the image sits in the middle; the panel on the right folds the image's
  properties (dimensions, type, the zoom with a menu of presets, rotation,
  position) and the file's (name, folder, size, modification time) under
  headers.
  Arrows, j/k, Space and Backspace walk the list, Home/End (g/G) jump to
  its ends, a thumbnail click selects; the wheel zooms about the pointer,
  a drag pans, `-`/`=` step the zoom, `1` is 1:1, `0`/`w` fit; `r`/`R`
  turn the image, `f` fullscreen, Tab hides the list, `i` the properties,
  `q`/Escape leave. The whole directory is read at the start, every
  file's bytes and a thumbnail decoded from each through the sandboxed
  decoder below; nothing touches the disk after, the shown image decodes
  from memory when selected, all on the one thread.

Single-threaded by design. Every object lives in a pool; the C++ standard
library is not used, the vocabulary comes from
[libstd](https://github.com/pg83/std). The codebase follows
[STYLE.md](STYLE.md).

## Rendering

`renderer.h` is the graphics boundary. `FrameDriver` drives ImGui and a
`Renderer`; the renderer owns its device, presentation and resources. The
suite's Vulkan implementation, including dma-buf import, HDR pipelines,
readback and GPU fault injection, lives in `renderer_vulkan.cpp`. No Vulkan
types or backend calls enter the tools or public headers. The vendored
ImGui backend and build-time GLSL shaders stay separate.

Both renderers implement the same image operations: upload RGBA8 pixels,
import a native shared image, draw it into an ImGui draw list, and read a
rectangle back. Images belong to their supplied pool, which must be
destroyed before the renderer's pool (or be the same pool). `read` returns
RGBA8 and RGB16 in the source encoding; the latter preserves 10-bit source
precision without passing through 8-bit pixels or display tone mapping.
HDR images contain BT.2020/PQ samples and need an HDR renderer; ordinary
images contain sRGB samples.

`SharedImage::create` is the native input boundary: Linux takes the shell's
layout description and a dma-buf fd, duplicates the descriptor and chooses
the exporting GPU. Metal takes an empty description and an `IOSurfaceRef`
cast to `intptr_t`, retains the surface, and supports RGBA8, BGRA8 and
`l10r` packed 10-bit sources. The producer must finish writing before the
image is imported or read. Screenshot's UI, crop and encoders use only
these image operations.

HDR rendering blends the image and UI in linear BT.2020. Vulkan encodes the
result into a PQ swapchain; Metal presents RGBA16Float through an extended
linear BT.2020 CoreAnimation layer with EDR enabled. Both use the configured
SDR white level. The Metal HDR shaders and draw submission are local to its
renderer; the upstream ImGui backend is unchanged.

## Building

```
./build            # .build/im and the im<tool> links
./build im_test    # the test build: fault seam and trace lines compiled in
```

The build wants clang, `pkg-config`, `wayland-scanner`, `glslangValidator`,
`wasm2c` (wabt, with the wasm2c runtime it ships) and the development
files of wayland-client, vulkan, libpng and libjxl. `dev/style.py` formats
the tree.

Images are decoded by ImageMagick, vendored as one pure WebAssembly module
(`ext/decode/decode.wasm`, from [pg83/decode](https://github.com/pg83/decode))
that wasm2c turns into C at build time: a sandboxed library of the binary,
every memory access checked, with no interpreter or JIT behind it.

## macOS

The viewer and `im ui` build for macOS as well, on plt's Cocoa backend,
drawing with Metal through ImGui's own Metal backend: `build.py` takes the
target from the runner (`--target aarch64-apple-darwin11`, the triple an ix
darwin realm names), links the frameworks Metal and Cocoa need, and leaves
out what is Linux's: Vulkan, the screenshot tool with its encoders, the
compositor's devices and the scenarios. On a Mac, the host tools
(`bin/wabt`, `bin/pkg/config`) are enough:

```
./build --target aarch64-apple-darwin11
```

From Linux the same build cross-checks through the overlay in `dev/ix`,
which puts the darwin libraries into a host realm; the compiler is the
toolchain's own, on PATH by hand:

```
IX_PATH=$PWD/dev/ix:{builtin} ix run set/suite/darwin -- sh -c \
  'PATH=$(echo "$CPPFLAGS" | grep -o "/ix/store/[^ ]*-bin-clang-[0-9]*/share/include" | head -1 | sed "s|/share/include|/bin|"):$PATH \
   ./build --target aarch64-apple-darwin11 -B .build-darwin im'
```

## Testing

```
./build test                      # every scenario, under its own headless Sway
./build test -Dfilter='save_*'    # some of them
```

`./build renderer_test` builds a renderer contract check at
`.build/e2e/renderer_test`. It checks upload, cropped readback, channel
precision, bounds and drawing. The Linux scenario `renderer_pixels` runs
it under Sway. On a Mac, run it directly, then with `--hdr` to exercise
HDR shader compilation and submission; both modes also import and read an
IOSurface. `--pixels` checks pixel conversion without a window or GPU.
Cross-compilation checks the Metal host code, but executing its shaders
and checking EDR output require a Mac.

Each scenario in `tst/` runs `im_test` as a client of an isolated
headless Sway (drawn by pixman), drives it through virtual input devices
(`tst/devices.cpp`, the wlroots virtual pointer and keyboard) and checks
what grim captures and what lands on disk; `tst/session.py` is the
fixture. Every scenario is a node of the build graph: `dev/run_test.py`
runs it and writes a JSON verdict with the tails of its logs, and the
final `test` node (`dev/aggregate_tests.py`) reads every verdict, prints
the failures and fails the build. So `-j N` runs scenarios side by side,
a scenario reruns only when it, the harness or the tools changed, and
`-Dshard=K/N` gives a CI job its stable slice; `-Devidence=DIR` keeps
what a failed scenario captured (its PNGs and logs) under `DIR/<name>/`
for a person to look at. The tool's own Vulkan has
to present to that Sway: lavapipe does, over wl_shm (`VK_DRIVER_FILES`
names its ICD in CI); a hardware driver wants the dma-bufs a GPU-rendered
Sway offers. `-Druntime=DIR` keeps the scenarios' Wayland sockets under a
short path; the verdicts land in `.build/test-results/`.

A scenario that finds the compositor short of what it needs (the HDR
ones want `wp_color_manager_v1`, for the HDR10 swapchain) ends as
skipped, saying what; the verdict lists it. Sway offers colour
management from 1.12 and only under wlroots' Vulkan renderer:
`IM_E2E_RENDERER=vulkan` picks it, and the Alpine job runs so.

CI runs the scenarios in stock containers, under GCC with glibc, Clang
with Alpine's musl, ASan, UBSan and with coverage on both Ubuntu and
Alpine, merged (only Alpine's sway runs the HDR scenarios); `dev/ci.sh MODE`
(build, test, asan, ubsan, coverage) reproduces a job with the host's
`CC`/`CXX` in `.build/ci-MODE`, `dev/ci_linux.sh` is the container
recipe.
