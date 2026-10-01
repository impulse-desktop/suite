#pragma once

#include <std/sys/types.h>

namespace stl {
    class ObjPool;
    class StringView;
}

namespace plt {
    struct Window;
    struct Platform;
}

struct Renderer;
struct RendererOptions;
struct SharedImage;

Renderer* createVulkanRenderer(stl::ObjPool& pool, plt::Platform& platform, plt::Window& window, const RendererOptions& options);
SharedImage* createVulkanSharedImage(stl::ObjPool& pool, stl::StringView description, intptr_t handle);
