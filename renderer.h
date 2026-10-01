#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>
#include <std/lib/buffer.h>

#include <imgui.h>

namespace stl {
    class ObjPool;
}

namespace plt {
    struct Window;
}

struct SharedImage {
    u32 width = 0;
    u32 height = 0;

    static SharedImage* create(stl::ObjPool& pool, stl::StringView description, intptr_t handle);
};

struct ImagePixels {
    u32 width = 0;
    u32 height = 0;
    stl::Buffer rgba;
    stl::Buffer rgb16;
};

enum class PixelLayout : u8 {
    Rgba8,
    Bgra8,
    Rgb10A2,
    Bgr10A2
};

void unpackPixels(const void* data, u32 width, u32 height, size_t stride, PixelLayout layout, ImagePixels& out);
void checkImageSize(u32 width, u32 height, u32 limit);
void checkImageRegion(u32 width, u32 height, int x0, int y0, int x1, int y1);

struct RenderImage {
    virtual void draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) = 0;
    virtual void read(int x0, int y0, int x1, int y1, ImagePixels& out) = 0;
};

struct RendererOptions {
    bool hdr = false;
    float sdrWhiteNits = 203.f;
    SharedImage* shared = nullptr;
};

struct Renderer {
    virtual void beginFrame(u32 width, u32 height) = 0;
    virtual bool endFrame(ImDrawData* draw) = 0;
    virtual u32 maxTextureSide() = 0;
    virtual u32 maxTextures() = 0;
    virtual RenderImage* upload(stl::ObjPool& pool, u32 width, u32 height, const void* rgba, bool hdr) = 0;
    virtual RenderImage* import(stl::ObjPool& pool, SharedImage& source, bool hdr) = 0;

    static Renderer* create(stl::ObjPool& pool, plt::Window& window, const RendererOptions& options = {});
};
