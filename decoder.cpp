#include "decoder.h"

#include "util.h"

#include <std/mem/obj_pool.h>
#include <std/str/builder.h>

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
#include <wasm-rt-exceptions.h>
#include <wasm-rt-impl.h>
}

using namespace stl;

namespace {
    // a module whose memory grew to this is thrown away after its decode:
    // ImageMagick's pixel cache of a large image grows the linear memory,
    // which never shrinks, and a fresh module costs a few milliseconds
    constexpr u64 keepBelowBytes = 256u << 20;
    // what a decode may answer: a side a Vulkan image can have, a gigabyte
    // of pixels at most
    constexpr u32 maxSide = 65535;
    constexpr u64 maxBytes = 1u << 30;

    struct DecoderImpl final: Decoder {
        w2c_decode instance;
        bool alive = false;

        ~DecoderImpl() noexcept;

        bool decode(StringView file, StringView name, DecodedImage& out, Buffer& error) override;

        wasm_rt_memory_t* memory();
        bool fits(u64 offset, u64 length);
        void drop();
    };

    // the runtime's per-thread state (the alternate stack its signal handler
    // runs on), once per thread that decodes; the main thread's came with
    // the runtime's own initialization
    thread_local bool threadReady = false;

    void readyThread() {
        if (!threadReady) {
            wasm_rt_init_thread();
            threadReady = true;
        }
    }
}

DecoderImpl::~DecoderImpl() noexcept {
    drop();
}

wasm_rt_memory_t* DecoderImpl::memory() {
    return w2c_decode_memory(&instance);
}

bool DecoderImpl::fits(u64 offset, u64 length) {
    return offset + length <= memory()->size;
}

void DecoderImpl::drop() {
    if (alive) {
        wasm2c_decode_free(&instance);
        alive = false;
    }
}

bool DecoderImpl::decode(StringView file, StringView name, DecodedImage& out, Buffer& error) {
    readyThread();

    if (file.length() > 0xffffffffu - name.length()) {
        error = Buffer("the file is too large for the decoder"_sv);

        return false;
    }

    if (!alive) {
        wasm2c_decode_instantiate(&instance);
        alive = true;
    }

    u32 total = (u32)(file.length() + name.length());
    u32 in = 0;
    u32 res = 0;

    // a trap comes back here by longjmp: nothing with a destructor lives
    // between this and the calls into the module
    wasm_rt_trap_t trap = (wasm_rt_trap_t)wasm_rt_impl_try();

    if (trap) {
        error = Buffer(sv(StringBuilder() << "the decoder trapped: "_sv << StringView(wasm_rt_strerror(trap))));
        drop();

        return false;
    }

    // the file, then its name right behind it, in one allocation
    in = w2c_decode_malloc(&instance, total);

    if (!in || !fits(in, total)) {
        error = Buffer("the decoder is out of memory"_sv);
        drop();

        return false;
    }

    memcpy(memory()->data + in, file.data(), file.length());
    memcpy(memory()->data + in + file.length(), name.data(), name.length());
    res = w2c_decode_decode(&instance, in, (u32)file.length(), in + (u32)file.length(), (u32)name.length());
    w2c_decode_free(&instance, in);

    if (!res) {
        error = Buffer("not an image the decoder reads"_sv);

        if (memory()->size >= keepBelowBytes) {
            drop();
        }

        return false;
    }

    // {u32 width; u32 height; u8 rgba[]}, checked against the memory it
    // lives in: the module answers, it is not trusted
    if (!fits(res, 8)) {
        error = Buffer("the decoder answered out of its memory"_sv);
        drop();

        return false;
    }

    u32 header[2];

    memcpy(header, memory()->data + res, sizeof(header));

    u32 width = header[0];
    u32 height = header[1];
    u64 bytes = (u64)width * height * 4;

    if (!width || !height || width > maxSide || height > maxSide || bytes > maxBytes || !fits((u64)res + 8, bytes)) {
        error = Buffer(sv(StringBuilder() << "the decoder answered an image of "_sv << width << "x"_sv << height));
        drop();

        return false;
    }

    out.width = width;
    out.height = height;
    out.rgba = Buffer(StringView((const u8*)memory()->data + res + 8, (size_t)bytes));
    w2c_decode_free(&instance, res);

    if (memory()->size >= keepBelowBytes) {
        drop();
    }

    return true;
}

void Decoder::initProcess() {
    wasm_rt_init();
    threadReady = true;
}

Decoder* Decoder::create(ObjPool& pool) {
    return pool.make<DecoderImpl>();
}
