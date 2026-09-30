#include "decoder.h"

#include "util.h"
#include "decode_glue.h"

#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

using namespace stl;

namespace {
    struct DecoderImpl final: Decoder {
        void* module;

        explicit DecoderImpl(void* buffer);
        ~DecoderImpl() noexcept;

        void decode(StringView file, StringView name, DecodedImage& out) override;
    };
}

DecoderImpl::DecoderImpl(void* buffer)
    : module(buffer)
{
    decodeModuleInit(module);
}

DecoderImpl::~DecoderImpl() noexcept {
    decodeModuleFree(module);
}

void DecoderImpl::decode(StringView file, StringView name, DecodedImage& out) {
    const unsigned char* pixels = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    const char* reason = "";

    switch (decodeModuleRun(module, file.data(), file.length(), name.data(), name.length(), &pixels, &width, &height, &reason)) {
        case DecodeDone:
            break;
        case DecodeTrapped:
            fail(sv(StringBuilder() << "the decoder trapped: "_sv << StringView(reason)));
        case DecodeRefused:
            fail("not an image the decoder reads"_sv);
        case DecodeNoMemory:
            fail("the decoder is out of memory"_sv);
        case DecodeBadAnswer:
            fail(sv(StringBuilder() << "the decoder answered an image of "_sv << (i64)width << "x"_sv << (i64)height));
    }

    out.width = width;
    out.height = height;
    out.rgba = Buffer(pixels, (size_t)width * height * 4);
}

Decoder* Decoder::create(ObjPool& pool) {
    return pool.make<DecoderImpl>(pool.allocate(decodeModuleSize()));
}
