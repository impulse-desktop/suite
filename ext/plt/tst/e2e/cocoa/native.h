#pragma once

#include "window.h"

namespace stl {
    class ObjPool;
}

namespace plt::e2e {
    struct MetalCanvas {
        virtual bool paint(const WindowInfo& info, u32 color) = 0;
        virtual void describe() = 0;
        static MetalCanvas* create(stl::ObjPool& owner, const RenderContext& context);
    };
}
