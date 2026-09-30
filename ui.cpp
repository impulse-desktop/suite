#include "ui.h"

#include "util.h"

#include <math.h>
#include <stdlib.h>

using namespace stl;

namespace {
    // the scale is the output's, as the window reports it, times the
    // user's factor from IM_SCALE
    float gContent = 1.f;
    float gUser = 1.f;
    float gScale = 1.f;
    // ImGui's drag and double-click thresholds, as a design length: its
    // ScaleAllSizes leaves them alone
    constexpr Design mouseThreshold = 6_d;
}

float uiScale() {
    return gScale;
}

void initUiScale() {
    if (const char* s = getenv("IM_SCALE")) {
        double v = parseFloat(StringView(s));

        if (v > 0.0) {
            gUser = (float)v;
            gScale = gUser * gContent;
        }
    }
}

bool followContentScale(float contentScale) {
    if (contentScale <= 0.f || contentScale == gContent) {
        return false;
    }

    gContent = contentScale;
    gScale = gUser * gContent;

    return true;
}

float px(Design d) {
    float v = floorf(d.value * gScale + .5f);

    return d.value > 0.f && v < 1.f ? 1.f : v;
}

int pxi(Design d) {
    return (int)px(d);
}

ImVec2 px(Design w, Design h) {
    return ImVec2(px(w), px(h));
}

// from a fresh default every time: ScaleAllSizes compounds
ImGuiStyle uiStyle() {
    ImGuiStyle style;

    style.ScaleAllSizes(gScale);
    style.FontScaleMain = gScale;

    return style;
}

void applyUiStyle() {
    ImGui::GetStyle() = uiStyle();

    ImGuiIO& io = ImGui::GetIO();

    io.MouseDragThreshold = px(mouseThreshold);
    io.MouseDoubleClickMaxDist = px(mouseThreshold);
}
