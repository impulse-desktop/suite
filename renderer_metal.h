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

Renderer* createMetalRenderer(stl::ObjPool& pool, plt::Platform& platform, plt::Window& window, const RendererOptions& options);
SharedImage* createMetalSharedImage(stl::ObjPool& pool, stl::StringView description, intptr_t handle);
