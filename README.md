# impulse suite

[![CI](https://github.com/impulse-desktop/suite/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/impulse-desktop/suite/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/impulse-desktop/suite/branch/main/graph/badge.svg)](https://app.codecov.io/gh/impulse-desktop/suite/tree/main)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-informational)](STYLE.md)

The desktop tools of the impulse desktop, one binary: `im`. Each tool is a
Wayland client drawn with the vendored ImGui on Vulkan, windowed through
the vendored [plt](https://github.com/pg83/plt) platform layer, and lives
in its own process.

```
im screenshot fd:3      # the screenshot editor the compositor spawns
imscreenshot shot.imw   # the same tool, by its link
```

A link named `im<tool>` runs that tool under its own name; `im <tool>`
does the same from the one binary. Tools so far:

- `screenshot` — crops and saves the image the
  [shell](https://github.com/impulse-desktop/shell) hands over on the
  screenshot chord: a self-describing memfd, or on KMS the scanout dma-buf
  itself, encoded to PNG or JPEG XL, SDR or HDR.

Single-threaded by design. Every object lives in a pool; the C++ standard
library is not used, the vocabulary comes from
[libstd](https://github.com/pg83/std). The codebase follows
[STYLE.md](STYLE.md).

## Building

```
./build            # .build/im and the im<tool> links
./build im_test    # the test build: fault seam and trace lines compiled in
```

The build wants clang, `pkg-config`, `wayland-scanner`, `glslangValidator`
and the development files of wayland-client, vulkan, libpng, libjxl and
libdisplay-info. `dev/style.py` formats the tree.

## Testing

```
./build test                      # every scenario, under its own headless Sway
./build test -Dfilter='save_*'    # some of them
```

Each `tst/scenarios/*.py` runs `im_test` as a client of an isolated
headless Sway (drawn by pixman), drives it through virtual input devices
(`tst/support/devices.cpp`, the wlroots virtual pointer and keyboard) and
checks what grim captures and what lands on disk; `tst/session.py` is the
fixture. Every scenario is a node of the build graph: `dev/run_test.py`
runs it and writes a JSON verdict with the tails of its logs, and the
final `test` node (`dev/aggregate_tests.py`) reads every verdict, prints
the failures and fails the build. So `-j N` runs scenarios side by side,
a scenario reruns only when it, the harness or the tools changed, and
`-Dshard=K/N` gives a CI job its stable slice. The tool's own Vulkan has
to present to that Sway: lavapipe does, over wl_shm (`VK_DRIVER_FILES`
names its ICD in CI); a hardware driver wants the dma-bufs a GPU-rendered
Sway offers. `-Druntime=DIR` keeps the scenarios' Wayland sockets under a
short path; the verdicts land in `.build/test-results/`.
