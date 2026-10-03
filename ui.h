#pragma once

#include "renderer.h"

#include <std/str/view.h>
#include <std/sys/types.h>

#include <imgui.h>

namespace stl {
    class ObjPool;
    struct Runable;
}

namespace plt {
    struct Platform;
}

struct Design {
    float value;
};

constexpr Design operator""_d(long double v) {
    return {(float)v};
}

constexpr Design operator""_d(unsigned long long v) {
    return {(float)v};
}

struct UiOptions {
    Design width = 640_d;
    Design height = 480_d;
    RendererOptions renderer;
};

struct UiEvent {
    enum class Kind : u8 {
        Frame,
        Close
    };

    Kind kind = Kind::Frame;
};

struct Ui {
    virtual float px(Design d) = 0;
    ImVec2 px(Design w, Design h);

    static Ui* create(stl::ObjPool& pool, stl::StringView name, const UiOptions& options = {});
    virtual int run(stl::Runable& body) = 0;
    virtual bool next(UiEvent& event) = 0;
    virtual plt::Platform* platform() = 0;
    // May be called from a worker after create(); delivery is on the UI thread.
    virtual void requestFrame() = 0;
    virtual void requestFullscreen(bool on) = 0;
    virtual void requestResize(u32 width, u32 height) = 0;

    virtual RenderImage* uploadImage(stl::ObjPool& pool, u32 width, u32 height, const void* rgba, bool hdr) = 0;
    virtual RenderImage* importImage(stl::ObjPool& pool, SharedImage& source, bool hdr) = 0;
    virtual RenderImage* bindImage(stl::ObjPool& pool, u32 width, u32 height, const void* data, size_t size, size_t stride, stl::Runable& retired) = 0;
    virtual RenderImage* shadeImage(stl::ObjPool& pool, RenderShader& shader, u32 width, u32 height, const void* data, size_t size, stl::Runable& retired) = 0;

    virtual RenderShader* compileShader(stl::ObjPool& pool, const void* code, size_t size) = 0;

    virtual ImTextureRef loadTexture(u32 width, u32 height, const void* rgba) = 0;
    virtual void releaseTexture(ImTextureRef texture) = 0;
    virtual u32 maxTextureSide() = 0;
    virtual bool software() = 0;

    virtual bool drawErrorPanel(stl::StringView message) = 0;

    virtual void trace(stl::StringView what) = 0;
    virtual void timing(stl::StringView line) = 0;
};
