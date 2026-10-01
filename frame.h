#pragma once

#include "ui.h"

#include <std/str/view.h>
#include <std/sys/types.h>

#include <plt/window.h>

namespace stl {
    class ObjPool;
}

namespace plt {
    struct Platform;
}

struct ImGuiPlt;
struct ImDrawData;

float scaleFromEnv();
float scaledPx(Design d, float scale);
ImGuiStyle scaledStyle(float scale);
void setupImGuiContext(stl::ObjPool& pool, float scale);
int drawErrorPanel(stl::StringView tool, float scale, stl::StringView msg);
void clampWindowSize(const plt::WindowInfo& info, int& w, int& h);

struct Renderer {
    virtual void beginFrame(u32 width, u32 height) = 0;
    virtual bool endFrame(ImDrawData* draw) = 0;
    virtual u32 maxTextureSide() = 0;
    virtual u32 maxTextures() = 0;

    static Renderer* create(stl::ObjPool& pool, plt::Window& window);
};

struct UiFrame {
    virtual int frame() = 0;
};

struct FrameDriver final: plt::FrameCallback, plt::WindowEvents {
    plt::Platform* platform = nullptr;
    plt::Window* window = nullptr;
    ImGuiPlt* imgui = nullptr;
    Renderer* renderer = nullptr;
    UiFrame* ui = nullptr;
    stl::StringView tool;
    bool traceFrames = false;
    int action = 0;
    u32 presentedWidth = 0;
    u32 presentedHeight = 0;
    u64 frameBegan = 0;
    u64 frameCount = 0;

    bool frame(const plt::WindowInfo& info) override;
    void close() override;
};

int runUi(FrameDriver& driver);
