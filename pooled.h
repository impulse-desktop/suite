#pragma once

#include <std/mem/obj_pool.h>

template <typename F>
void pooledGuard(stl::ObjPool& p, F f) {
    struct Guard {
        F f;

        Guard(F g)
            : f(g)
        {
        }

        ~Guard() noexcept {
            f();
        }
    };

    p.make<Guard>(f);
}
