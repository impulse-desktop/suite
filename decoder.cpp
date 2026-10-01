#include "decoder.h"

#include "error.h"

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

    struct Instance {
        w2c_decode wasm;

        Instance();
        ~Instance() noexcept;

        u8* at(u64 offset, u64 length);
    };

    struct Impl final: Image {
        Instance instance;
        const void* data_;
        size_t length_;
        u32 width_;
        u32 height_;

        Impl(StringView file, StringView name);

        const void* data() const override;
        size_t length() const override;
        u32 width() const override;
        u32 height() const override;
    };
}

extern "C" void decodeTrapHandler(wasm_rt_trap_t code) {
    fail(StringView(StringBuilder() << StringView(u8"the decoder trapped: ") << StringView(wasm_rt_strerror(code))));
}

Instance::Instance() {
    wasm2c_decode_instantiate(&wasm);
}

Instance::~Instance() noexcept {
    wasm2c_decode_free(&wasm);
}

u8* Instance::at(u64 offset, u64 length) {
    wasm_rt_memory_t* memory = w2c_decode_memory(&wasm);

    if (offset + length > memory->size) {
        fail(StringView(u8"the decoder answered out of its memory"));
    }

    return (u8*)memory->data + offset;
}

Impl::Impl(StringView file, StringView name) {
    if (file.length() > 0xffffffffu - name.length()) {
        fail(StringView(u8"the file is too large for the decoder"));
    }

    u32 total = (u32)(file.length() + name.length());
    u32 in = w2c_decode_malloc(&instance.wasm, total);

    if (!in) {
        fail(StringView(u8"the decoder is out of memory"));
    }

    memcpy(instance.at(in, total), file.data(), file.length());
    memcpy(instance.at(in + file.length(), name.length()), name.data(), name.length());

    u32 res = w2c_decode_decode(&instance.wasm, in, (u32)file.length(), in + (u32)file.length(), (u32)name.length());

    w2c_decode_free(&instance.wasm, in);

    if (!res) {
        fail(StringView(u8"not an image the decoder reads"));
    }

    u32 header[2];

    memcpy(header, instance.at(res, sizeof(header)), sizeof(header));

    u32 width = header[0];
    u32 height = header[1];
    u64 bytes = (u64)width * height * 4;

    if (!width || !height || width > maxSide || height > maxSide || bytes > maxBytes) {
        fail(StringView(StringBuilder() << StringView(u8"the decoder answered an image of ") << (i64)width << StringView(u8"x") << (i64)height));
    }

    data_ = instance.at((u64)res + 8, bytes);
    length_ = (size_t)bytes;
    width_ = width;
    height_ = height;
}

const void* Impl::data() const {
    return data_;
}

size_t Impl::length() const {
    return length_;
}

u32 Impl::width() const {
    return width_;
}

u32 Impl::height() const {
    return height_;
}

Image* decode(ObjPool& pool, StringView file, StringView name) {
    return pool.make<Impl>(file, name);
}
