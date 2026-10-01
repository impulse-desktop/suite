#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

#include <imgui.h>

namespace stl {
    class ObjPool;
}

// One scale for the whole ui. ImGui's widgets and font take it through
// ImGui's own factors (ScaleAllSizes, FontScaleMain), what the tools draw
// by hand takes it through px(): a design length times the scale. The
// image's own pixels never do: at 100% a texel is a pixel, thumbnails
// follow the pixel width of their column, the pointer and the window size
// come from the platform in pixels already.
struct Design {
    float value;
};

constexpr Design operator""_d(long double v) {
    return {(float)v};
}

constexpr Design operator""_d(unsigned long long v) {
    return {(float)v};
}

// the scale: IM_SCALE, 1 without it. Nothing else: the tools draw pixels
// and know nothing of how an output scales its own logical units
float uiScale();
// IM_SCALE, once at start
void initUiScale();

// a design length in whole pixels, never under one for a positive length;
// positions are sums of these, so they stay whole too
float px(Design d);
int pxi(Design d);
ImVec2 px(Design w, Design h);

// ImGui's style at the scale: its defaults through its own scaling
ImGuiStyle uiStyle();
// the style into the current context, with the input thresholds
void applyUiStyle();

// The tool's window, and all a tool knows of what is behind it: the
// events it iterates and ImGui. The tool runs on a fiber of its own; the
// platform's loop, the device and the frames run on the main stack and
// switch to the tool when an event is there for it, and back when it
// asks for the next. One window per process.

struct UiOptions {
    // the window's size, a design length: the platform's own units and
    // the screen's limits are the runtime's business
    Design width = 640_d;
    Design height = 480_d;
};

struct UiEvent {
    enum class Kind : u8 {
        // a frame is the tool's: its ImGui calls, from here to the next
        // next(), are what the window shows
        Frame,
        // the window is asked to go: by the compositor, the close button
        Close
    };

    Kind kind = Kind::Frame;
};

struct Ui {
    // the next event, once there is one: the tool sleeps until then.
    // false: the window is gone, nothing more will come
    virtual bool next(UiEvent& event) = 0;

    // a texture of these pixels, RGBA, 8 bits a channel, rows tightly
    // packed; copied, the tool's own may go at once. ImGui draws it by
    // the reference (AddImage), from the next frame shown on. Throws past
    // the textures a window holds at once
    virtual ImTextureRef loadTexture(u32 width, u32 height, const void* rgba) = 0;
    // the tool is done with it: from here the reference is dead to the
    // tool, the device's copy goes once no frame in flight reads it
    virtual void releaseTexture(ImTextureRef texture) = 0;
    // the longest side a texture may have
    virtual u32 maxTextureSide() = 0;

    virtual void requestFullscreen(bool on) = 0;

    // the window, shown, owned by the pool; one per process. Throws where
    // the window or its device cannot be had
    static Ui* create(stl::ObjPool& pool, const UiOptions& options);
};

// a full-window panel that replaces the tool's ui (not an overlay) when
// something goes wrong: reads like a message from the compositor, a
// heading, the error text, and a single Exit button; -1 on exit
int drawErrorPanel(stl::StringView msg);

// main's: the tool on its fiber under its name, the platform's loop until
// it returns, its result. The tool makes what it holds, its window among
// it, in the pool it is handed: the pool dies once the loop is over, never
// inside an event, which the tool may return from. An exception out of
// the tool is reported under its name and gives 1
int runTool(stl::StringView name, int (*tool)(stl::ObjPool& pool, int argc, char** argv), int argc, char** argv);
