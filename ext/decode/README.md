# decode.wasm

ImageMagick and its coders (PNG, JPEG, WebP, TIFF, JPEG 2000, JPEG XL, GIF,
BMP, PNM/PAM, TGA, PCX, SGI, MIFF) as one pure WebAssembly module, from
[pg83/decode](https://github.com/pg83/decode): release 1,
`https://github.com/pg83/decode/releases/download/1/decode.wasm.zstd`,
decompressed (sha256 of the module
40e0bcec0760cd018fd804d1ddebe5d89987b8d5c699b4b7c3d11f7fd0e3d738). The
build behind it proves the module imports nothing and decodes every
format against the host ImageMagick.

The module exports its `memory`, `malloc`, `free` and

```
decode(data, len, name, name_len) -> image | 0
struct image { uint32 width; uint32 height; uint8 rgba[]; }
```

taking the file's bytes and its name in the module's memory and answering a
block there, to be `free`d by the caller: 8-bit sRGB RGBA with the file's
orientation applied, the first frame of an animation, or 0 for a file no
coder takes. A broken file may trap the module; its memory is then not to
be trusted and the instance is thrown away. The stack is 8 MB; the static
constructors run on the first call of an instance.

The suite compiles it to C with wasm2c (see `build.py`), so the decoder is
an ordinary library of the binary whose every memory access is checked.
