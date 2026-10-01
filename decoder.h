#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>
#include <std/lib/buffer.h>

namespace stl {
    class ObjPool;
}

struct DecodedImage {
    u32 width = 0;
    u32 height = 0;
    stl::Buffer rgba;
};

struct Decoder {
    virtual void decode(stl::StringView file, stl::StringView name, DecodedImage& out) = 0;

    static Decoder* create(stl::ObjPool& pool);
};
