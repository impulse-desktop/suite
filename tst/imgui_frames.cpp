#include <math.h>
#include <float.h>
#include <imgui.h>
#include <stdio.h>
#include <stdlib.h>
#include <imgui_internal.h>

namespace {
    static void check(bool ok, const char* message) {
        if (!ok) {
            fprintf(stderr, "%s\n", message);
            abort();
        }
    }

    struct Probe;

    struct Timer {
        Probe* probe;
        double at = DBL_MAX;
    };

    struct Probe {
        Timer timers[128];
        int timerCount = 0;
        double now = 0;
        double lastFrame = 0;
        bool requested = true;
        int frames = 0;
        int clicks = 0;
        int presses = 0;
        bool text = false;
        bool focus = false;
        bool tooltip = false;
        bool tooltipVisible = false;
        bool cursorVisible = false;
        bool scroll = false;
        bool setScroll = false;
        float scrollY = 0;
        bool modal = false;
        bool openModal = false;
        bool closeModal = false;
        ImVec2 button;
        char value[64] = "text";

        Probe();
        ~Probe();
        void frame();
        void advance(double seconds);
        void settle();
        bool quiet() const;
        static Probe& get(ImGuiContext* ctx);
    };
}

Probe& Probe::get(ImGuiContext* ctx) {
    return *(Probe*)ctx->IO.BackendPlatformUserData;
}

Probe::Probe() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(400, 400);
    io.BackendPlatformUserData = this;
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    io.Fonts->SetTexID(1);
    ImGui::GetStyle().Colors[ImGuiCol_InputTextCursor] = ImVec4(1, 0, 1, 1);
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_RequestFrameFn = [](ImGuiContext* ctx) {
        get(ctx).requested = true;
    };
    pio.Platform_CreateFrameTimerFn = [](ImGuiContext* ctx) -> void* {
        Probe& p = get(ctx);
        check(p.timerCount < 128, "unbounded timer allocation");
        Timer& timer = p.timers[p.timerCount++];
        timer.probe = &p;
        return &timer;
    };
    pio.Platform_ScheduleFrameTimerFn = [](void* handle, float seconds) {
        Timer& timer = *(Timer*)handle;
        timer.at = timer.probe->now + seconds;
    };
    pio.Platform_CancelFrameTimerFn = [](void* handle) {
        ((Timer*)handle)->at = DBL_MAX;
    };
}

Probe::~Probe() {
    ImGui::GetIO().BackendPlatformUserData = nullptr;
    ImGui::GetPlatformIO().ClearPlatformHandlers();
    ImGui::DestroyContext();
}

void Probe::frame() {
    requested = false;
    frames++;
    ImGui::GetIO().DeltaTime = (float)(now - lastFrame);
    lastFrame = now;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(400, 400));
    ImGui::Begin("test", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    presses += ImGui::IsKeyPressed(ImGuiKey_A);
    if (text) {
        if (focus) {
            ImGui::SetKeyboardFocusHere();
            focus = false;
        }
        ImGui::InputText("text", value, sizeof(value));
    }
    if (ImGui::Button("button", ImVec2(100, 30))) {
        clicks++;
    }
    button = ImGui::GetItemRectMin();
    button.x += 20;
    button.y += 15;
    tooltipVisible = false;
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("delayed tooltip");
        tooltipVisible = true;
    }
    if (scroll) {
        if (setScroll) {
            ImGui::SetScrollY(150);
            setScroll = false;
        }
        scrollY = ImGui::GetScrollY();
        ImGui::Dummy(ImVec2(100, 1000));
    }
    if (openModal) {
        ImGui::OpenPopup("modal");
        openModal = false;
        modal = true;
    }
    if (modal && ImGui::BeginPopupModal("modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("modal contents");
        if (closeModal) {
            ImGui::CloseCurrentPopup();
            closeModal = false;
            modal = false;
        }
        ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::Render();
    cursorVisible = false;
    for (ImDrawList* list : ImGui::GetDrawData()->CmdLists) {
        for (const ImDrawVert& vertex : list->VtxBuffer) {
            cursorVisible |= vertex.col == IM_COL32(255, 0, 255, 255);
        }
    }
}

void Probe::advance(double seconds) {
    double until = now + seconds;
    for (int count = 0;; count++) {
        check(count < 1000, "rendering failed to settle");
        double next = requested ? now + 1.0 / 60 : DBL_MAX;
        for (int i = 0; i < timerCount; i++) {
            if (timers[i].at < next) {
                next = timers[i].at;
            }
        }
        if (next > until) {
            now = until;
            return;
        }
        now = next > now ? next : now + 0.000001;
        for (int i = 0; i < timerCount; i++) {
            if (timers[i].at <= now) {
                timers[i].at = DBL_MAX;
            }
        }
        frame();
    }
}

bool Probe::quiet() const {
    if (requested) {
        return false;
    }
    for (int i = 0; i < timerCount; i++) {
        if (timers[i].at != DBL_MAX) {
            return false;
        }
    }
    return true;
}

void Probe::settle() {
    advance(6);
    check(quiet(), "idle UI still requests frames or timers");
}

int main() {
    {
        Probe p;
        p.settle();
        check(p.frames < 8, "static UI renders continuously");
        ImGui::GetIO().AddMousePosEvent(p.button.x, p.button.y);
        p.settle();
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        p.settle();
        check(p.clicks == 1, "trickled press/release lost the click");
        p.scroll = p.setScroll = p.requested = true;
        p.settle();
        check(p.scrollY == 150, "deferred scroll did not reach the next frame");
    }
    {
        Probe p;
        p.text = p.focus = true;
        p.advance(0.15);
        check(p.cursorVisible, "focused text has no cursor");
        int frames = p.frames;
        p.advance(0.3);
        check(p.frames == frames, "caret causes continuous rendering");
        ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
        p.advance(0.35);
        check(p.presses >= 2, "caret timer postponed key repeat");
        ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
        p.advance(0.1);
        frames = p.frames;
        p.advance(2.4);
        check(p.frames - frames <= 6, "key release did not cancel repeat");
        ImGui::GetIO().ConfigInputTextCursorBlink = false;
        p.requested = true;
        p.settle();
        ImGui::GetIO().ConfigInputTextCursorBlink = true;
        ImGui::GetIO().AddInputCharacter('x');
        p.advance(0.05);
        check(p.cursorVisible, "editing after idle did not reset the caret");
        p.advance(1.1);
        check(!p.cursorVisible, "caret did not blink off at its deadline");
        p.advance(0.4);
        check(p.cursorVisible, "caret did not blink on at its deadline");
        ImGui::GetIO().AddMousePosEvent(p.button.x, p.button.y);
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        p.settle();
        check(p.clicks == 1, "click did not deactivate text");
    }
    {
        Probe p;
        p.tooltip = true;
        p.settle();
        ImGui::GetIO().AddMousePosEvent(p.button.x, p.button.y);
        p.advance(0.1);
        check(!p.tooltipVisible, "tooltip appeared before its delay");
        p.advance(0.6);
        check(p.tooltipVisible, "tooltip did not wake without more input");
        p.settle();
        ImGui::GetIO().AddMousePosEvent(390, 390);
        p.settle();
        check(!p.tooltipVisible, "tooltip remained after pointer left");
        ImGui::GetIO().AddMousePosEvent(p.button.x, p.button.y);
        p.advance(0.1);
        check(!p.tooltipVisible, "idle time did not expire the tooltip grace period");
        p.advance(0.6);
        check(p.tooltipVisible, "tooltip did not return after its delay");
        p.openModal = p.requested = true;
        p.settle();
        check(GImGui->DimBgRatio == 1, "modal fade did not finish");
        p.closeModal = p.requested = true;
        p.settle();
        check(GImGui->DimBgRatio == 0, "modal close did not finish");
    }
    puts("OK: idle, trickled input, scroll, independent timers, repeat cancellation, caret, tooltip and modal");
}
