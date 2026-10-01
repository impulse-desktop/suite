#pragma once

#include "renderer.h"

#include <std/str/view.h>
#include <std/sys/types.h>

#include <imgui.h>

namespace stl {
    class ObjPool;
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

    virtual void open(const UiOptions& options) = 0;
    virtual bool next(UiEvent& event) = 0;
    virtual void requestFullscreen(bool on) = 0;
    virtual void requestResize(u32 width, u32 height) = 0;

    virtual RenderImage* uploadImage(stl::ObjPool& pool, u32 width, u32 height, const void* rgba, bool hdr) = 0;
    virtual RenderImage* importImage(stl::ObjPool& pool, SharedImage& source, bool hdr) = 0;

    virtual ImTextureRef loadTexture(u32 width, u32 height, const void* rgba) = 0;
    virtual void releaseTexture(ImTextureRef texture) = 0;
    virtual u32 maxTextureSide() = 0;

    virtual bool drawErrorPanel(stl::StringView message) = 0;

    virtual void trace(stl::StringView what) = 0;
    virtual void timing(stl::StringView line) = 0;
};

int runTool(stl::StringView name, int (*tool)(stl::ObjPool& pool, Ui& ui, int argc, char** argv), int argc, char** argv);
