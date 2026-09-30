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
// memory access checked, so a coder reading a hostile file can reach nothing
// but its own memory. One decoder serves one thread. A file that traps the
// module (a coder that cannot take it) is reported and the module is thrown
// away; the next decode starts a fresh one.
struct Decoder {
    // the file's bytes and its name (the coder is picked by the extension
    // when the bytes do not say); false with the reason in error
    virtual bool decode(stl::StringView file, stl::StringView name, DecodedImage& out, stl::Buffer& error) = 0;

    // once per process, on the main thread, before any decoder is made: the
    // runtime's signal handler, for the guard pages around every module's
    // memory
    static void initProcess();
    static Decoder* create(stl::ObjPool& pool);
};
