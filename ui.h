#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

#include <imgui.h>

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

    // the window, shown; it lives as long as the tool. Throws where the
    // window or its device cannot be had
    static Ui& open(const UiOptions& options);
};

// main's: the tool on its fiber under its name, the platform's loop until
// it returns, its result. An exception out of the tool is reported under
// its name and gives 1
int runTool(stl::StringView name, int (*tool)(int argc, char** argv), int argc, char** argv);
