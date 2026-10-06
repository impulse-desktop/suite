#include "ui_demo.h"

#include "ui.h"

#include <std/thr/runable.h>

#include <imgui.h>

using namespace stl;

int mainUiDemo(ObjPool& pool, int, char**) {
    Ui& ui = *Ui::create(pool, StringView(u8"ui"), {320_d, 120_d});
    auto body = makeRunable([&] {
        UiEvent event;

        while (ui.next(event)) {
            if (event.kind == UiEvent::Kind::Close) {
                return;
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
                TRACE(&ui, StringView(u8"ok"));

                return;
            }
        }
    });
    return ui.run(body);
}
