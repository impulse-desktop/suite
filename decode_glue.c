#include "decode_glue.h"

#include <stdint.h>
#include <string.h>

#include <decode.h>
#include <wasm-rt-exceptions.h>
#include <wasm-rt-impl.h>

// what a decode may answer: a side a Vulkan image can have, a gigabyte of
// pixels at most
#define DECODE_MAX_SIDE 65535u
#define DECODE_MAX_BYTES (1ull << 30)

size_t decodeModuleSize(void) {
    return sizeof(w2c_decode);
}

void decodeModuleInit(void* module) {
    wasm2c_decode_instantiate((w2c_decode*)module);
}

void decodeModuleFree(void* module) {
    wasm2c_decode_free((w2c_decode*)module);
}

// a range of the module's memory, or NULL: the module answers, it is not
// trusted
static unsigned char* at(w2c_decode* m, uint64_t offset, uint64_t length) {
    wasm_rt_memory_t* memory = w2c_decode_memory(m);

    if (offset + length > memory->size) {
        return NULL;
    }

    return memory->data + offset;
}

enum DecodeStatus decodeModuleRun(void* module, const void* file, size_t fileLength, const void* name, size_t nameLength, const unsigned char** pixels, unsigned* width, unsigned* height, const char** reason) {
    w2c_decode* m = (w2c_decode*)module;

    *pixels = NULL;
    *width = 0;
    *height = 0;
    *reason = "";

    if (fileLength > 0xffffffffu - nameLength) {
        return DecodeNoMemory;
    }

    // a trap comes back here by longjmp
    wasm_rt_trap_t trap = wasm_rt_impl_try();

    if (trap) {
        *reason = wasm_rt_strerror(trap);

        return DecodeTrapped;
    }

    // the file, then its name right behind it, in one allocation
    uint32_t total = (uint32_t)(fileLength + nameLength);
    uint32_t in = w2c_decode_malloc(m, total);
    unsigned char* input = in ? at(m, in, total) : NULL;

    if (!input) {
        return DecodeNoMemory;
    }

    memcpy(input, file, fileLength);
    memcpy(input + fileLength, name, nameLength);

    uint32_t res = w2c_decode_decode(m, in, (uint32_t)fileLength, in + (uint32_t)fileLength, (uint32_t)nameLength);

    w2c_decode_free(m, in);

    if (!res) {
        return DecodeRefused;
    }

    // {uint32 width; uint32 height; uint8 rgba[]}
    const unsigned char* header = at(m, res, 8);

    if (!header) {
        return DecodeBadAnswer;
    }

    uint32_t dims[2];

    memcpy(dims, header, sizeof(dims));
    *width = dims[0];
    *height = dims[1];

    uint64_t bytes = (uint64_t)dims[0] * dims[1] * 4;

    if (!dims[0] || !dims[1] || dims[0] > DECODE_MAX_SIDE || dims[1] > DECODE_MAX_SIDE || bytes > DECODE_MAX_BYTES) {
        return DecodeBadAnswer;
    }

    *pixels = at(m, (uint64_t)res + 8, bytes);

    return *pixels ? DecodeDone : DecodeBadAnswer;
}
