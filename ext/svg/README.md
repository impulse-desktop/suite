# svg.wasm

[impulse-desktop/svg](https://github.com/impulse-desktop/svg), the SVG
renderer, as one pure WebAssembly module, from
[impulse-desktop/decode](https://github.com/impulse-desktop/decode). Built
by [ix](https://github.com/pg83/ix) as `lib/svg/wasm` for the
`wasm32-none` target (sha256 of the module
0465a5995286e9faa283ac2549a68639bcec584402ce954fd2c0f30dd2ef10e3). The
build behind it proves the module imports nothing and draws a small
drawing through it on WAMR, twice in one instance, checking its pixels.

The module exports its `memory`, `malloc`, `free` and the page exports of
pdf.wasm and djvu.wasm, a drawing being one page:

```
svg_open(data, len) -> doc
svg_pages(doc) -> 1
svg_width(doc, page) -> CSS pixels
svg_height(doc, page) -> CSS pixels
svg_render(doc, page, width, height) -> image | 0
svg_close(doc)
svg_error() -> the cause of the last trap, a C string
struct image { uint32 width; uint32 height; uint8 rgba[]; }
```

taking the file's bytes in the module's memory, which the document copies,
so the caller may `free` them once `svg_open` returns. The page's size is
the drawing's own, in CSS pixels. A render is the drawing scaled to fit
the asked size, as an `<img>` of that size shows it: 8-bit RGBA, not
premultiplied, transparent where nothing is drawn, in a block to be
`free`d by the caller. A file that is no SVG traps the module, as does an
image of no pixel or past 2^26; `svg_error` then names the `SvgError` of
svg.h, and the instance is thrown away. The module has no raster decoder:
PNG and JPEG images inside a drawing are left out. The stack is 8 MB; the
static constructors run on the first `svg_open` of an instance.
