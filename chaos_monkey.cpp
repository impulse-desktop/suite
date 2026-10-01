#include "chaos_monkey.h"

#include "util.h"

#include <std/mem/obj_pool.h>

#include <stdlib.h>

using namespace stl;

namespace {
    struct EncoderChaos final: ChaosMonkey {
        int allocSkip = -1;
        int outputSkip = -1;
        EncoderChaos();
        bool encoderAlloc(bool pending) override;
        bool encoderOutput(bool produced) override;
    };
}

EncoderChaos::EncoderChaos() {
#ifdef IM_FOR_TESTS
    const char* value = getenv("IM_CHAOS");
    StringView script(value ? value : "");
    while (!script.empty()) {
        StringView word, rest, fault, arg;
        if (script.split(' ', word, rest)) {
            script = rest;
        } else {
            word = script;
            script = {};
        }
        if (word.split('=', fault, arg)) {
            if (fault == "encoder-alloc"_sv) {
                allocSkip = (int)arg.stou();
            } else if (fault == "encoder-output"_sv) {
                outputSkip = (int)arg.stou();
            }
        }
    }
#endif
}

bool EncoderChaos::encoderAlloc(bool pending) {
    return !(allocSkip >= 0 && allocSkip-- == 0) && pending;
}

bool EncoderChaos::encoderOutput(bool produced) {
    return !(outputSkip >= 0 && outputSkip-- == 0) && produced;
}

ChaosMonkey* ChaosMonkey::create(ObjPool& pool) {
    return pool.make<EncoderChaos>();
}
