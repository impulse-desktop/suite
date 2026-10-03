#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>
#include <std/lib/buffer.h>

#include <imgui.h>

namespace stl {
    class ObjPool;
    struct Runable;
}

namespace plt {
    struct Window;
    struct Platform;
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
    stl::Buffer rgbaf;
};

enum class PixelLayout : u8 {
    Rgba8,
    Bgra8,
    Rgb10A2,
    Bgr10A2,
    Rgba16f
};

void unpackPixels(const void* data, u32 width, u32 height, size_t stride, PixelLayout layout, ImagePixels& out);
void checkImageSize(u32 width, u32 height, u32 limit);
void checkImageRegion(u32 width, u32 height, int x0, int y0, int x1, int y1);

struct RenderShader {};

struct RenderImage {
    // A bound image's producer calls prepare after filling its CPU buffer,
    // before transferring the image to the UI through a Channel.
    virtual void prepare() = 0;
    virtual void draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) = 0;
    virtual void underlay(ImVec2 lo, ImVec2 hi) = 0;
    virtual void read(int x0, int y0, int x1, int y1, ImagePixels& out) = 0;
    virtual void shadeWith(RenderShader& shader) = 0;
};

struct RendererOptions {
    bool hdr = false;
    float sdrWhiteNits = 203.f;
    SharedImage* shared = nullptr;
};

struct Renderer {
    virtual bool beginFrame(u32 width, u32 height) = 0;
    virtual bool endFrame(ImDrawData* draw) = 0;

    virtual u32 maxTextureSide() = 0;
    virtual u32 maxTextures() = 0;

    virtual RenderImage* upload(stl::ObjPool& pool, u32 width, u32 height, const void* rgba, bool hdr) = 0;
    virtual RenderImage* import(stl::ObjPool& pool, SharedImage& source, bool hdr) = 0;
    virtual RenderImage* bind(stl::ObjPool& pool, u32 width, u32 height, const void* data, size_t size, size_t stride, stl::Runable& retired) = 0;
    virtual RenderImage* shade(stl::ObjPool& pool, RenderShader& shader, u32 width, u32 height, const void* data, size_t size, stl::Runable& retired) = 0;

    virtual RenderShader* compileShader(stl::ObjPool& pool, const void* code, size_t size) = 0;

    static Renderer* create(stl::ObjPool& pool, plt::Platform& platform, plt::Window& window, const RendererOptions& options = {});
};
