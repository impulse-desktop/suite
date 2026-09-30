#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>
#include <std/lib/buffer.h>

namespace stl {
    class ObjPool;
}

// what a decode gives back: RGBA8 rows, sRGB, the file's orientation applied
struct DecodedImage {
    u32 width = 0;
    u32 height = 0;
    stl::Buffer rgba;
};

// ImageMagick as a sandboxed library: ext/decode/decode.wasm, a pure wasm
// module of ImageMagick and its coders, compiled to C by wasm2c with every
// load and store checked against the module's memory in the code itself and
// its call depth counted, so a coder reading a hostile file can reach
// nothing but its own memory, and no signal handler is involved. One
// decoder serves one thread. A file that traps the module (a coder that
// cannot take it, an access past the memory, a recursion past the depth
// limit) is reported and the module is thrown away; the next decode starts
// a fresh one.
struct Decoder {
    // the file's bytes and its name (the coder is picked by the extension
    // when the bytes do not say); false with the reason in error
    virtual bool decode(stl::StringView file, stl::StringView name, DecodedImage& out, stl::Buffer& error) = 0;

    static Decoder* create(stl::ObjPool& pool);
};
