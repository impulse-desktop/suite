# ImSuite

[![CI](https://github.com/impulse-desktop/suite/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/impulse-desktop/suite/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/impulse-desktop/suite/branch/main/graph/badge.svg)](https://app.codecov.io/gh/impulse-desktop/suite/tree/main)
[![release](https://img.shields.io/github/v/release/impulse-desktop/suite?label=release&color=blue)](https://github.com/impulse-desktop/suite/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++26](https://img.shields.io/badge/C%2B%2B-26-informational)](STYLE.md)

The desktop tools of the impulse desktop in one binary, `im`: a
screenshot editor, an image viewer, a video player, a PDF and DjVu reader,
a text editor and a file chooser.

## Why

The impulse desktop is a compositor, [shell](https://github.com/impulse-desktop/shell),
and these tools. The shell runs `imscreenshot` on the screenshot key, the
[portal](https://github.com/impulse-desktop/portal) runs `im choose` when
an application asks for a file, and the viewer, the player, the reader
and the editor open what the user opens. Every tool is a small native
program in a process of its own, drawn on Vulkan straight to Wayland,
with no toolkit underneath. Images, documents and file types are read by
sandboxed decoders, so a hostile file cannot reach past the decoder's own
memory.

## Running

Each tool runs as `im <tool> ...`, or by its link `im<tool>`:

| Command | What it does |
|---|---|
| `im view FILE\|DIR...` | shows images: a directory's, or the files named |
| `im play FILE` | plays a video |
| `im read FILE` | reads a PDF or DjVu document |
| `im edit FILE` | edits a text file, or starts a new one |
| `im choose [DIR]` | lets the user pick a file and prints its path |
| `im screenshot FILE` | crops and saves a screenshot the shell hands over |
| `im ui` | the widget demo |

`im choose` takes `--save`, `--directory`, `--multiple`, `--title T`,
`--name N` and `--filter 'Label|*.ext|type/*'`; it prints the chosen
paths and exits 0, or exits 1 when cancelled.

`im screenshot` saves to `$XDG_PICTURES_DIR/screenshots` (else
`~/Pictures/screenshots`) as JPEG XL. `IM_SHOT_DIR`, `IM_SHOT_NAME` (a
strftime pattern) and `IM_SHOT_FORMAT=png` change where and how.
`IM_SCALE` sets the interface scale of any tool.

The tools need a Wayland compositor and a Vulkan driver.

## Building

Linux. You need clang 21 or GCC 16, python3, pkg-config,
wayland-scanner, glslangValidator, `wasm2c` from wabt with the runtime it
ships, and the development files of wayland-client, xkbcommon, vulkan,
libpng, libjxl, FFmpeg (libavformat, libavcodec, libavutil,
libswresample) and OpenAL.

```
./build          # .build/im and the im<tool> links
./build -j 8 im  # the binary alone, on 8 jobs
```

The decoders are WebAssembly modules vendored under `ext/`.
`-Ddecode_wasm=PATH`, `-Dpdf_wasm=PATH`, `-Ddjvu_wasm=PATH` and
`-Dmagic_wasm=PATH` build with other ones instead.

On [IX](https://github.com/pg83/ix), `ix build bin/im/suite` builds the
package, with modules IX builds itself. `bin/im/pulse` is the whole
desktop.

## License

MIT, see [LICENSE](LICENSE). The vendored code under `ext/` keeps its
own license: ImGui, the text editor widget, libstd and plt are MIT; the
WebAssembly modules are ImageMagick, PDFium, libmagic and DjVuLibre, the
last under the GPL, version 2 or later.
