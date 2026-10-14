#include "svg.h"

#include "error.h"

#include <std/alg/defer.h>
#include <std/lib/buffer.h>
#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <string.h>

#define WASM_RT_CORE_TYPES_DEFINED
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef float f32;
typedef double f64;

// the svg module, compiled under the name vector: svg.h is the suite's own;
// a trap throws out of it through the handler decoder.cpp defines
extern "C" {
#include <vector.h>
}

using namespace stl;

namespace {
    constexpr u32 causeLimit = 256;

    struct RasterImage final: Image {
        Buffer rgba;
        u32 width_;
        u32 height_;

        RasterImage(u32 width, u32 height);

        const void* data() const override;
        size_t length() const override;
        u32 width() const override;
        u32 height() const override;
    };

    // a drawing open in an instance of its own: one job parses and draws it
    struct DocumentImpl final: SvgDocument {
        w2c_vector wasm;
        u32 doc = 0;
        float width_ = 0.f;
        float height_ = 0.f;

        explicit DocumentImpl(StringView file);
        ~DocumentImpl() noexcept;

        float width() const override;
        float height() const override;
        Image* render(ObjPool& pool, u32 width, u32 height) override;

        u8* at(u64 offset, u64 length);
        Buffer cause();
    };

    struct LibraryImpl final: SvgLibrary {
        SvgDocument* parse(ObjPool& pool, StringView file) override;
    };

    bool xml(StringView file) {
        const u8* p = file.begin();
        const u8* end = file.end();

        if (end - p >= 3 && p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) {
            p += 3;
        }

        while (p != end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
            p++;
        }

        return p != end && *p == '<';
    }
}

RasterImage::RasterImage(u32 width, u32 height)
    : width_(width)
    , height_(height)
{
    rgba.zero((size_t)width * height * 4);
}

const void* RasterImage::data() const {
    return rgba.data();
}

size_t RasterImage::length() const {
    return rgba.length();
}

u32 RasterImage::width() const {
    return width_;
}

u32 RasterImage::height() const {
    return height_;
}

// the module opens the file or traps: a file that is no SVG, which the
// caller hands to the image decoder
DocumentImpl::DocumentImpl(StringView file) {
    if (file.length() > 0xffffffffu) {
        fail(StringView(u8"the file is too large for the SVG engine"));
    }

    u32 length = (u32)file.length();

    wasm2c_vector_instantiate(&wasm);
    ScopedGuard cleanup = [&] {
        if (!doc) {
            wasm2c_vector_free(&wasm);
        }
    };

    u32 data = w2c_vector_malloc(&wasm, length);

    if (!data) {
        fail(StringView(u8"the SVG engine is out of memory"));
    }

    memcpy(at(data, length), file.data(), length);
    // the document copies the file: the engine's own copy goes at once
    doc = w2c_vector_svg_open(&wasm, data, length);
    w2c_vector_free(&wasm, data);
    width_ = w2c_vector_svg_width(&wasm, doc, 0);
    height_ = w2c_vector_svg_height(&wasm, doc, 0);
}

DocumentImpl::~DocumentImpl() noexcept {
    // an instance that trapped is dead, and goes as it is
    wasm2c_vector_free(&wasm);
}

float DocumentImpl::width() const {
    return width_;
}

float DocumentImpl::height() const {
    return height_;
}

Image* DocumentImpl::render(ObjPool& pool, u32 width, u32 height) {
    if (!width || !height || (u64)width * height * 4 > 0x7fffffffu) {
        raiseError(StringView(StringBuilder() << StringView(u8"cannot draw the SVG at ") << (i64)width << StringView(u8"x") << (i64)height));
    }

    u64 bytes = (u64)width * height * 4;
    u32 res = 0;

    try {
        res = w2c_vector_svg_render(&wasm, doc, 0, width, height);
    } catch (...) {
        Buffer why = cause();

        raiseError(StringView(StringBuilder() << StringView(u8"the SVG engine failed: ") << (why.empty() ? StringView(u8"a trap") : StringView(why))));
    }

    if (!res) {
        raiseError(StringView(u8"the SVG engine is out of memory"));
    }

    ScopedGuard release = [&] {
        w2c_vector_free(&wasm, res);
    };

    const u8* block = at(res, 8 + bytes);
    u32 header[2];

    memcpy(header, block, sizeof(header));

    if (header[0] != width || header[1] != height) {
        fail(StringView(StringBuilder() << StringView(u8"the SVG engine drew ") << (i64)header[0] << StringView(u8"x") << (i64)header[1] << StringView(u8" for ") << (i64)width << StringView(u8"x") << (i64)height));
    }

    RasterImage* image = pool.make<RasterImage>(width, height);

    memcpy(image->rgba.mutData(), block + 8, bytes);

    return image;
}

u8* DocumentImpl::at(u64 offset, u64 length) {
    wasm_rt_memory_t* memory = w2c_vector_memory(&wasm);

    if (offset + length > memory->size) {
        fail(StringView(u8"the SVG engine answered out of its memory"));
    }

    return (u8*)memory->data + offset;
}

// the engine's cause of its trap, the name of the SvgError: a C string in
// its memory, read as far as the memory and the limit allow
Buffer DocumentImpl::cause() {
    u32 text = w2c_vector_svg_error(&wasm);
    wasm_rt_memory_t* memory = w2c_vector_memory(&wasm);

    if (!text || text >= memory->size) {
        return Buffer();
    }

    const u8* begin = (const u8*)memory->data + text;
    const u8* end = begin;
    const u8* limit = (const u8*)memory->data + (memory->size - text < causeLimit ? memory->size : (u64)text + causeLimit);

    while (end < limit && *end) {
        end++;
    }

    return Buffer(StringView(begin, end));
}

SvgDocument* LibraryImpl::parse(ObjPool& pool, StringView file) {
    if (!xml(file)) {
        return nullptr;
    }

    DocumentImpl* parsed = nullptr;

    try {
        parsed = pool.make<DocumentImpl>(file);
    } catch (...) {
        return nullptr;
    }

    if (!isfinite(parsed->width_) || !isfinite(parsed->height_) || parsed->width_ <= 0.f || parsed->height_ <= 0.f) {
        raiseError(StringView(u8"the SVG has no size"));
    }

    return parsed;
}

SvgLibrary* SvgLibrary::create(ObjPool& pool) {
    return pool.make<LibraryImpl>();
}
