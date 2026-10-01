#pragma once

#include <std/str/view.h>

namespace stl {
    class ObjPool;
}

struct ChaosMonkey {
    virtual bool encoderAlloc(bool pending) = 0;
    virtual bool encoderOutput(bool produced) = 0;
    static ChaosMonkey* create(stl::ObjPool& pool);
};
