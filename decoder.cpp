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

    struct DecoderImpl final: Decoder {
        w2c_decode instance;

        DecoderImpl();
        ~DecoderImpl() noexcept;

        void decode(StringView file, StringView name, DecodedImage& out) override;

        u8* at(u64 offset, u64 length);
    };
}

extern "C" void decodeTrapHandler(wasm_rt_trap_t code) {
    fail(StringView(StringBuilder() << StringView(u8"the decoder trapped: ") << StringView(wasm_rt_strerror(code))));
}

DecoderImpl::DecoderImpl() {
    wasm2c_decode_instantiate(&instance);
}

DecoderImpl::~DecoderImpl() noexcept {
    wasm2c_decode_free(&instance);
}

u8* DecoderImpl::at(u64 offset, u64 length) {
    wasm_rt_memory_t* memory = w2c_decode_memory(&instance);

    if (offset + length > memory->size) {
        fail(StringView(u8"the decoder answered out of its memory"));
    }

    return (u8*)memory->data + offset;
}

void DecoderImpl::decode(StringView file, StringView name, DecodedImage& out) {
    if (file.length() > 0xffffffffu - name.length()) {
        fail(StringView(u8"the file is too large for the decoder"));
    }

    u32 total = (u32)(file.length() + name.length());
    u32 in = w2c_decode_malloc(&instance, total);

    if (!in) {
        fail(StringView(u8"the decoder is out of memory"));
    }

    memcpy(at(in, total), file.data(), file.length());
    memcpy(at(in + file.length(), name.length()), name.data(), name.length());

    u32 res = w2c_decode_decode(&instance, in, (u32)file.length(), in + (u32)file.length(), (u32)name.length());

    w2c_decode_free(&instance, in);

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

    out.width = width;
    out.height = height;
    out.rgba = Buffer(at((u64)res + 8, bytes), (size_t)bytes);
    w2c_decode_free(&instance, res);
}

Decoder* Decoder::create(ObjPool& pool) {
    return pool.make<DecoderImpl>();
}
