#include "frame.h"

#include "util.h"
#include "pooled.h"
#include "imgui_plt.h"

#include <std/ios/sys.h>
#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <stdlib.h>
#include <imgui.h>
#include <plt/platform.h>

using namespace stl;

namespace {
    constexpr Design mouseThreshold = 6_d;
}

float scaleFromEnv() {
    if (const char* s = getenv("IM_SCALE")) {
        double v = parseFloat(StringView(s));

        if (v > 0.0) {
            return (float)v;
        }
    }

    return 1.f;
}

float scaledPx(Design d, float scale) {
    float v = floorf(d.value * scale + .5f);

    return d.value > 0.f && v < 1.f ? 1.f : v;
}

ImGuiStyle scaledStyle(float scale) {
    ImGuiStyle style;

    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;

    return style;
}

void setupImGuiContext(ObjPool& pool, float scale) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    pooledGuard(pool, [] {
        ImGui::DestroyContext();
    });

    ImGuiIO& io = ImGui::GetIO();

    io.IniFilename = nullptr;
    ImGui::GetStyle() = scaledStyle(scale);
    io.MouseDragThreshold = scaledPx(mouseThreshold, scale);
    io.MouseDoubleClickMaxDist = scaledPx(mouseThreshold, scale);
}

int drawErrorPanel(StringView tool, float scale, StringView msg) {
    ImGuiViewport* vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);

    int result = 0;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(28, 28, 32, 255));
    ImGui::Begin("##err", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    float pad = scaledPx(24_d, scale);

    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImGui::BeginGroup();

    StringBuilder heading;

    heading << "im "_sv << tool;
    ImGui::TextDisabled("%s", heading.cStr());
    ImGui::Spacing();
    ImGui::PushTextWrapPos(vp->Size.x - pad);
    ImGui::TextUnformatted((const char*)msg.data(), (const char*)msg.data() + msg.length());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Spacing();

    if (ImGui::Button("Exit", ImVec2(scaledPx(120_d, scale), 0.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        result = -1;
    }

    ImGui::EndGroup();
    ImGui::End();
    ImGui::PopStyleColor();

    return result;
}

void clampWindowSize(const plt::WindowInfo& info, int& w, int& h) {
    int maxW = (int)info.screenPixelWidth * 9 / 10;
    int maxH = (int)info.screenPixelHeight * 9 / 10;

    if (w > maxW) {
        w = maxW;
    }

    if (h > maxH) {
        h = maxH;
    }
}

bool FrameDriver::frame(const plt::WindowInfo& info) {
    u64 began = nowNs();
    u64 gap = frameBegan ? began - frameBegan : 0;

    frameBegan = began;
    renderer->beginFrame(info.width, info.height);

    if (info.width != presentedWidth || info.height != presentedHeight) {
        presentedWidth = info.width;
        presentedHeight = info.height;
        traceTool(tool, sv(StringBuilder() << "presenting "_sv << (i64)info.width << "x"_sv << (i64)info.height));
    }

    u64 begun = nowNs();

    imgui->newFrame(*window);
    ImGui::NewFrame();

    u64 opened = nowNs();
    int result = ui->frame();
    u64 drew = nowNs();

    ImGui::Render();

    u64 rendered = nowNs();
    bool presented = renderer->endFrame(ImGui::GetDrawData());

    if (traceFrames) {
        StringBuilder text;

        text << "im frame "_sv << (i64)frameCount++ << ": gap "_sv;
        appendMs(text, gap);
        text << " begin "_sv;
        appendMs(text, begun - began);
        text << " new "_sv;
        appendMs(text, opened - begun);
        text << " tool "_sv;
        appendMs(text, drew - opened);
        text << " render "_sv;
        appendMs(text, rendered - drew);
        text << " end "_sv;
        appendMs(text, nowNs() - rendered);

        if (!presented) {
            text << " unpresented"_sv;
        }

        sysE << sv(text) << endL;
    }

    if (result != 0) {
        action = result;
        platform->stop();
    } else {
        window->requestFrame();
    }

    return presented;
}

void FrameDriver::close() {
    action = -1;
    platform->stop();
}

int runUi(FrameDriver& driver) {
    driver.action = 0;
    driver.window->requestShow();
    driver.window->requestFrame();
    driver.platform->run();

    return driver.action ? driver.action : -1;
}
