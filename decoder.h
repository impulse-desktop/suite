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

// The image scaled down to fit a side, each pixel the mean of the source
// pixels it covers; the image itself when it fits.
Image* shrink(stl::ObjPool& pool, Image* image, u32 side);

// The image as a thumbnail: cut to the box of its pixels at least half
// opaque, the transparent margins and a soft shadow off, then shrunk to
// fit the side; the image itself when there is nothing to cut and it fits.
Image* thumbnail(stl::ObjPool& pool, Image* image, u32 side);
