# pdf.wasm

PDFium, Chrome's PDF engine, as one pure WebAssembly module: no V8, no
XFA, no Skia, the AGG rasterizer, its own FreeType with the built-in
fonts. Built by [ix](https://github.com/pg83/ix) as `lib/pdf/ium/wasm` for
the `wasm32-none` target (sha256 of the module
20c3960519c88a56c63d70c788a1a7a3d8f615ae58aa2aa37b66af709ef0d9d2). The
build behind it proves the module imports nothing and renders a page of
text through it on WAMR, twice in one instance.

The module exports its `memory`, `malloc`, `free` and

```
pdf_open(data, len) -> doc | 0
pdf_pages(doc) -> count
pdf_width(doc, page) -> points | 0
pdf_height(doc, page) -> points | 0
pdf_render(doc, page, width, height) -> image | 0
pdf_close(doc)
struct image { uint32 width; uint32 height; uint8 rgba[]; }
```

taking the file's bytes in the module's memory, which the document reads
from until `pdf_close`, so the caller keeps them there. A render is the
page scaled to the asked size on white, with its annotations, 8-bit sRGB
RGBA in a block to be `free`d by the caller. A broken file may trap the
module; its memory is then not to be trusted and the instance is thrown
away. The stack is 8 MB; the static constructors and the library's
initialization run on the first `pdf_open` of an instance.
