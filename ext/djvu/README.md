# djvu.wasm

DjVuLibre, the DjVu codec, as one pure WebAssembly module, driven through
its C++ API, single-threaded and without exceptions. Built by
[ix](https://github.com/pg83/ix) as `lib/djvulibre/wasm` for the
`wasm32-none` target (sha256 of the module
c82b47f7d5fc5b3d1f9d734fd45fe98f232faf4b3936f1d21ed8bbd414c2197d). The
build behind it proves the module imports nothing and renders a page the
host's c44 encoded through it on WAMR, twice in one instance.

The module exports its `memory`, `malloc`, `free` and

```
djvu_open(data, len) -> doc | 0
djvu_pages(doc) -> count
djvu_width(doc, page) -> points | 0
djvu_height(doc, page) -> points | 0
djvu_render(doc, page, width, height) -> image | 0
djvu_close(doc)
djvu_error() -> the cause of the last trap, a C string
struct image { uint32 width; uint32 height; uint8 rgba[]; }
```

taking the file's bytes in the module's memory, which the document copies,
so the caller may `free` them once `djvu_open` returns. A page's size is in
points, its pixels over its resolution, as a PDF page's. A render is the
page scaled to the asked size, 8-bit sRGB RGBA in a block to be `free`d by
the caller. A broken file traps the module, which is what the library's
exceptions became; `djvu_error` then names the cause, and the instance is
thrown away. The stack is 8 MB; the static constructors run on the first
`djvu_open` of an instance.
