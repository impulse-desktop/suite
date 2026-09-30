#include "decoder.h"

#include "util.h"

#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

#include <string.h>

// the generated header's core type names: libstd already has u8..u64 (its
// u8 is char8_t, which nothing the header declares uses), the rest come
// from here
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
    // what a decode may answer: a side a Vulkan image can have, a gigabyte
    // of pixels at most
    constexpr u32 maxSide = 65535;
    constexpr u64 maxBytes = 1u << 30;

    struct DecoderImpl final: Decoder {
        w2c_decode instance;

        DecoderImpl();
        ~DecoderImpl() noexcept;

        void decode(StringView file, StringView name, DecodedImage& out) override;

        // a range of the module's memory: the module answers, it is not trusted
        u8* at(u64 offset, u64 length);
    };
}

// the runtime's trap (an access past the memory, an unreachable, a
// recursion past the depth limit) lands here from inside the module's C
// frames, compiled to be unwound through, and leaves as the decode's
// exception (WASM_RT_TRAP_HANDLER in build.py)
extern "C" void decodeTrapHandler(wasm_rt_trap_t code) {
    fail(sv(StringBuilder() << "the decoder trapped: "_sv << StringView(wasm_rt_strerror(code))));
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
        fail("the decoder answered out of its memory"_sv);
    }

    return (u8*)memory->data + offset;
}

void DecoderImpl::decode(StringView file, StringView name, DecodedImage& out) {
    if (file.length() > 0xffffffffu - name.length()) {
        fail("the file is too large for the decoder"_sv);
    }

    // the file, then its name right behind it, in one allocation
    u32 total = (u32)(file.length() + name.length());
    u32 in = w2c_decode_malloc(&instance, total);

    if (!in) {
        fail("the decoder is out of memory"_sv);
    }

    memcpy(at(in, total), file.data(), file.length());
    memcpy(at(in + file.length(), name.length()), name.data(), name.length());

    u32 res = w2c_decode_decode(&instance, in, (u32)file.length(), in + (u32)file.length(), (u32)name.length());

    w2c_decode_free(&instance, in);

    if (!res) {
        fail("not an image the decoder reads"_sv);
    }

    // {u32 width; u32 height; u8 rgba[]}
    u32 header[2];

    memcpy(header, at(res, sizeof(header)), sizeof(header));

    u32 width = header[0];
    u32 height = header[1];
    u64 bytes = (u64)width * height * 4;

    if (!width || !height || width > maxSide || height > maxSide || bytes > maxBytes) {
        fail(sv(StringBuilder() << "the decoder answered an image of "_sv << (i64)width << "x"_sv << (i64)height));
    }

    out.width = width;
    out.height = height;
    out.rgba = Buffer(at((u64)res + 8, bytes), (size_t)bytes);
    w2c_decode_free(&instance, res);
}

Decoder* Decoder::create(ObjPool& pool) {
    return pool.make<DecoderImpl>();
}
