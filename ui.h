#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

#include <imgui.h>

namespace stl {
    class ObjPool;
}

// A length in the design: what Ui::px turns into whole pixels at the ui's
// scale. ImGui's widgets and font take the same scale through ImGui's own
// factors; the image's own pixels never do: at 100% a texel is a pixel,
// the pointer and the window size come from the platform in pixels.
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

// All a tool has of the world but its arguments and its pool: the scale,
// its window and the events it iterates, its textures, the trace, and
// ImGui. The tool runs on a fiber of its own; the platform's loop, the
// device and the frames run on the main stack and switch to the tool when
// an event is there for it, and back when it asks for the next.
struct Ui {
    // a design length in whole pixels at the scale (IM_SCALE), never under
    // one for a positive length; positions are sums of these, so they stay
    // whole too
    virtual float px(Design d) = 0;

    ImVec2 px(Design w, Design h) {
        return ImVec2(px(w), px(h));
    }

    // the window, shown; once. Throws where the window or its device
    // cannot be had
    virtual void open(const UiOptions& options) = 0;
    // the next event, once there is one: the tool sleeps until then.
    // false: the window is gone, nothing more will come
    virtual bool next(UiEvent& event) = 0;
    virtual void requestFullscreen(bool on) = 0;

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

    // the panel in place of the tool's own ui when something went wrong:
    // the tool's name, the message, an Exit button; true once exit is asked
    virtual bool drawErrorPanel(stl::StringView message) = 0;

    // the test build's account of a step, `im <tool>: <what>`, for the
    // scenarios to wait on; nothing in the ordinary build
    virtual void trace(stl::StringView what) = 0;
    // a line of the frame trace on stderr, with IM_TRACE_FRAMES; nothing
    // without it
    virtual void timing(stl::StringView line) = 0;
};

// main's: the tool on its fiber under its name, with its Ui, the
// platform's loop until it returns, its result. The tool makes what it
// holds in the pool it is handed: the pool dies once the loop is over,
// never inside an event, which the tool may return from. An exception out
// of the tool is reported under its name and gives 1
int runTool(stl::StringView name, int (*tool)(stl::ObjPool& pool, Ui& ui, int argc, char** argv), int argc, char** argv);
