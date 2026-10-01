#include "ui_demo.h"

#include "ui.h"
#include "util.h"

#include <imgui.h>

using namespace stl;

// The tool as every tool is now: a window opened, then a loop over its
// events. A frame is one of them: on it the tool draws its ImGui windows,
// and what it drew is shown when it asks for the next event.
int mainUiDemo(ObjPool& pool, int, char**) {
    Ui& ui = *Ui::create(pool, {320_d, 120_d});
    UiEvent event;

    while (ui.next(event)) {
        if (event.kind == UiEvent::Kind::Close) {
            return 0;
        }

        // the whole window one ImGui window, the button in its middle
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 button(px(96_d), ImGui::GetFrameHeight());

        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("##ui", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorPos(ImVec2((vp->Size.x - button.x) / 2.f, (vp->Size.y - button.y) / 2.f));

        bool ok = ImGui::Button("OK", button);

        ImGui::End();

        if (ok) {
            traceText("ok"_sv);

            return 0;
        }
    }

    return 0;
}
