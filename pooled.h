#pragma once

#include <std/mem/obj_pool.h>

// a one-off cleanup adopted by the pool: the escape hatch for foreign
// resources without a dedicated registration. Capture by value — by
// pool-death time the registering scope is long gone.
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
