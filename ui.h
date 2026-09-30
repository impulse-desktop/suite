#pragma once

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

// the scale: the window's content scale, followed as the window moves
// between outputs, times IM_SCALE, a user's factor over it (the tests set
// it on an output of scale 1). Nothing here knows how the platform found
// its scale. A window is asked for in the platform's logical units, which
// its output scales by itself: before the window exists the content scale
// counts as 1, so a design size then is the user's factor alone, as it
// should be
float uiScale();
// IM_SCALE, once at start
void initUiScale();
// the window's content scale; true when the scale changed, so the style
// is to be applied again
bool followContentScale(float contentScale);

// a design length in whole pixels, never under one for a positive length;
// positions are sums of these, so they stay whole too
float px(Design d);
int pxi(Design d);
ImVec2 px(Design w, Design h);

// ImGui's style at the scale: its defaults through its own scaling
ImGuiStyle uiStyle();
// the style into the current context, with the input thresholds
void applyUiStyle();
