#pragma once

#include "decoder.h"

namespace stl {
    class ObjPool;
}

struct SvgDocument {
    virtual float width() const = 0;
    virtual float height() const = 0;
    virtual Image* render(stl::ObjPool& pool, u32 width, u32 height) = 0;
};

struct SvgLibrary {
    virtual SvgDocument* parse(stl::ObjPool& pool, stl::StringView file) = 0;

    static SvgLibrary* create(stl::ObjPool& pool);
};
