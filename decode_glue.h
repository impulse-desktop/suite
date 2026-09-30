#pragma once

#include <stddef.h>

// The calls into the generated module, from C: the runtime's try macro names
// its thread-local state with the keyword C gives it, which a C++ unit
// cannot repeat. A module lives in a buffer of decodeModuleSize() bytes:
// decodeModuleInit instantiates it there, decodeModuleFree tears it down.
// decodeModuleRun feeds a file and its name to decode(), checks the answer
// against the module's memory and hands the pixels out, RGBA8 rows in the
// module's memory, good until the module is freed.

#ifdef __cplusplus
extern "C" {
#endif

    enum DecodeStatus {
        DecodeDone,
        // the module trapped: the reason names the trap, and the module is not
        // to be trusted any more
        DecodeTrapped,
        // no coder took the file
        DecodeRefused,
        // the module's memory could not hold the file
        DecodeNoMemory,
        // the module answered an image that is not in its memory, or none a
        // texture could hold; width and height say what it claimed
        DecodeBadAnswer
    };

    size_t decodeModuleSize(void);
    void decodeModuleInit(void* module);
    void decodeModuleFree(void* module);
    enum DecodeStatus decodeModuleRun(void* module, const void* file, size_t fileLength, const void* name, size_t nameLength, const unsigned char** pixels, unsigned* width, unsigned* height, const char** reason);

#ifdef __cplusplus
}
#endif
