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
