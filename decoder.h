#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

namespace stl {
    class ObjPool;
}

// RGBA8 pixels, length in bytes; storage lives with the pool passed to decode().
struct Image {
    virtual const void* data() const = 0;
    virtual size_t length() const = 0;
    virtual u32 width() const = 0;
    virtual u32 height() const = 0;
};

Image* decode(stl::ObjPool& pool, stl::StringView file, stl::StringView name);
