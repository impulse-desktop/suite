#include "ui_demo.h"

#include "ui.h"

#include <imgui.h>

using namespace stl;

int mainUiDemo(ObjPool&, Ui& ui, int, char**) {
    UiEvent event;

    ui.open({320_d, 120_d});

    while (ui.next(event)) {
        if (event.kind == UiEvent::Kind::Close) {
            return 0;
        }

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 button(ui.px(96_d), ImGui::GetFrameHeight());

        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("##ui", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorPos(ImVec2((vp->Size.x - button.x) / 2.f, (vp->Size.y - button.y) / 2.f));

        bool ok = ImGui::Button("OK", button);

        ImGui::End();

        if (ok) {
            ui.trace(StringView(u8"ok"));

            return 0;
        }
    }

    return 0;
}
