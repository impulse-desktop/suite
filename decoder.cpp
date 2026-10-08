#include "decoder.h"

#include "error.h"

#include <std/alg/defer.h>
#include <std/alg/minmax.h>
#include <std/lib/buffer.h>
#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

#include <string.h>

#define WASM_RT_CORE_TYPES_DEFINED
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef float f32;
typedef double f64;

extern "C" {
#include <decode.h>
}

using namespace stl;

namespace {
    constexpr u32 maxSide = 65535;
    constexpr u64 maxBytes = 1u << 30;

    struct ImageImpl final: Image {
        w2c_decode wasm;
        const void* data_ = nullptr;
        size_t length_;
        u32 width_;
        u32 height_;

        ImageImpl(StringView file, StringView name);
        ~ImageImpl() noexcept;

        const void* data() const override;
        size_t length() const override;
        u32 width() const override;
        u32 height() const override;

        u8* at(u64 offset, u64 length);
    };

    struct ThumbImage final: Image {
        Buffer rgba;
        u32 width_;
        u32 height_;

        ThumbImage(const Image& image, u32 left, u32 top, u32 cw, u32 ch, u32 side);

        const void* data() const override;
        size_t length() const override;
        u32 width() const override;
        u32 height() const override;
    };
}

ThumbImage::ThumbImage(const Image& image, u32 left, u32 top, u32 cw, u32 ch, u32 side) {
    u32 sw = image.width();

    if (cw <= side && ch <= side) {
        width_ = cw;
        height_ = ch;
    } else {
        width_ = cw >= ch ? side : (u32)max<u64>(1, (u64)side * cw / ch);
        height_ = ch >= cw ? side : (u32)max<u64>(1, (u64)side * ch / cw);
    }

    rgba.zero((size_t)width_ * height_ * 4);

    const unsigned char* src = (const unsigned char*)image.data();
    unsigned char* dst = (unsigned char*)rgba.mutData();

    for (u32 dy = 0; dy < height_; dy++) {
        u32 y0 = (u32)((u64)dy * ch / height_);
        u32 y1 = (u32)max<u64>(y0 + 1, (u64)(dy + 1) * ch / height_);

        for (u32 dx = 0; dx < width_; dx++) {
            u32 x0 = (u32)((u64)dx * cw / width_);
            u32 x1 = (u32)max<u64>(x0 + 1, (u64)(dx + 1) * cw / width_);
            u64 sum[4] = {};

            for (u32 y = y0; y < y1; y++) {
                const unsigned char* row = src + ((size_t)(top + y) * sw + left + x0) * 4;

                for (u32 x = x0; x < x1; x++) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    sum[3] += row[3];
                    row += 4;
                }
            }

            u64 count = (u64)(x1 - x0) * (y1 - y0);
            unsigned char* px = dst + ((size_t)dy * width_ + dx) * 4;

            px[0] = (unsigned char)((sum[0] + count / 2) / count);
            px[1] = (unsigned char)((sum[1] + count / 2) / count);
            px[2] = (unsigned char)((sum[2] + count / 2) / count);
            px[3] = (unsigned char)((sum[3] + count / 2) / count);
        }
    }
}

const void* ThumbImage::data() const {
    return rgba.data();
}

size_t ThumbImage::length() const {
    return rgba.length();
}

u32 ThumbImage::width() const {
    return width_;
}

u32 ThumbImage::height() const {
    return height_;
}

Image* shrink(ObjPool& pool, Image* image, u32 side) {
    u32 sw = image->width();
    u32 sh = image->height();

    if (sw <= side && sh <= side) {
        return image;
    }

    return pool.make<ThumbImage>(*image, 0, 0, sw, sh, side);
}

Image* thumbnail(ObjPool& pool, Image* image, u32 side) {
    u32 sw = image->width();
    u32 sh = image->height();
    const unsigned char* src = (const unsigned char*)image->data();
    u32 x0 = sw;
    u32 y0 = sh;
    u32 x1 = 0;
    u32 y1 = 0;

    // the box of the pixels with any alpha
    for (u32 y = 0; y < sh; y++) {
        const unsigned char* row = src + (size_t)y * sw * 4 + 3;

        for (u32 x = 0; x < sw; x++, row += 4) {
            if (*row) {
                x0 = min(x0, x);
                x1 = max(x1, x + 1);
                y0 = min(y0, y);
                y1 = max(y1, y + 1);
            }
        }
    }

    // nothing visible at all: the image as it is
    if (x1 <= x0 || y1 <= y0) {
        x0 = 0;
        y0 = 0;
        x1 = sw;
        y1 = sh;
    }

    if (x0 == 0 && y0 == 0 && x1 == sw && y1 == sh && sw <= side && sh <= side) {
        return image;
    }

    return pool.make<ThumbImage>(*image, x0, y0, x1 - x0, y1 - y0, side);
}

extern "C" void decodeTrapHandler(wasm_rt_trap_t code) {
    fail(StringView(StringBuilder() << StringView(u8"the decoder trapped: ") << StringView(wasm_rt_strerror(code))));
}

ImageImpl::ImageImpl(StringView file, StringView name) {
    if (file.length() > 0xffffffffu - name.length()) {
        fail(StringView(u8"the file is too large for the decoder"));
    }

    wasm2c_decode_instantiate(&wasm);
    ScopedGuard cleanup = [&] {
        if (data_ == nullptr) {
            wasm2c_decode_free(&wasm);
        }
    };

    u32 total = (u32)(file.length() + name.length());
    u32 in = w2c_decode_malloc(&wasm, total);

    if (!in) {
        fail(StringView(u8"the decoder is out of memory"));
    }

    memcpy(at(in, total), file.data(), file.length());
    memcpy(at(in + file.length(), name.length()), name.data(), name.length());

    u32 res = w2c_decode_decode(&wasm, in, (u32)file.length(), in + (u32)file.length(), (u32)name.length());

    w2c_decode_free(&wasm, in);

    if (!res) {
        fail(StringView(u8"not an image the decoder reads"));
    }

    u32 header[2];

    memcpy(header, at(res, sizeof(header)), sizeof(header));

    u32 width = header[0];
    u32 height = header[1];
    u64 bytes = (u64)width * height * 4;

    if (!width || !height || width > maxSide || height > maxSide || bytes > maxBytes) {
        fail(StringView(StringBuilder() << StringView(u8"the decoder answered an image of ") << (i64)width << StringView(u8"x") << (i64)height));
    }

    length_ = (size_t)bytes;
    width_ = width;
    height_ = height;
    data_ = at((u64)res + 8, bytes);
}

ImageImpl::~ImageImpl() noexcept {
    wasm2c_decode_free(&wasm);
}

const void* ImageImpl::data() const {
    return data_;
}

size_t ImageImpl::length() const {
    return length_;
}

u32 ImageImpl::width() const {
    return width_;
}

u32 ImageImpl::height() const {
    return height_;
}

u8* ImageImpl::at(u64 offset, u64 length) {
    wasm_rt_memory_t* memory = w2c_decode_memory(&wasm);

    if (offset + length > memory->size) {
        fail(StringView(u8"the decoder answered out of its memory"));
    }

    return (u8*)memory->data + offset;
}

Image* decode(ObjPool& pool, StringView file, StringView name) {
    return pool.make<ImageImpl>(file, name);
}
