#include "ui.h"

#include "util.h"

#include <math.h>
#include <stdlib.h>

using namespace stl;

namespace {
    float gScale = 1.f;
    bool gFixed = false;
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
            gScale = (float)v;
            gFixed = true;
        }
    }
}

bool followContentScale(float contentScale) {
    if (gFixed || contentScale <= 0.f || contentScale == gScale) {
        return false;
    }

    gScale = contentScale;

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
