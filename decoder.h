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
// nothing but its own memory, and no signal handler is involved. A decoder
// is one instance of the module, in the pool that made it, serving one
// thread. A decode that fails throws (ToolError with the reason: a file no
// coder takes, a trap of the module) and the decoder is spent with it: it
// goes with its pool, and the next decode takes a new one.
struct Decoder {
    // the file's bytes and its name (the coder is picked by the extension
    // when the bytes do not say)
    virtual void decode(stl::StringView file, stl::StringView name, DecodedImage& out) = 0;

    static Decoder* create(stl::ObjPool& pool);
};
