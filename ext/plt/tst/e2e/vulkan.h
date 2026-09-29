#pragma once

#include "app.h"

namespace plt::e2e {
    struct VulkanPresenter {
        virtual bool paint(App& app, const WindowInfo& info) = 0;
        static VulkanPresenter* create(stl::ObjPool& owner, const RenderContext& context);
    };
}
