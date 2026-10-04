#include "error.h"

#include <std/ios/sys.h>
#include <std/str/view.h>
#include <std/str/builder.h>

#include <math.h>
#include <imgui.h>
#include <string.h>

using namespace stl;

namespace {
    constexpr u32 canvasTexture = 1000;
    constexpr int canvasSide = 256;

    ImVec4 video;
    bool hasVideo = false;

    struct Writer {
        void word(u32 value) {
            sysO.write(&value, sizeof(value));
        }

        void real(float value) {
            sysO.write(&value, sizeof(value));
        }

        void bytes(const void* data, size_t size) {
            sysO.write(data, size);
        }
    };

    void serveTextures() {
        static u32 next = 1;

        for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
            if (texture->Status == ImTextureStatus_WantCreate) {
                texture->SetTexID((ImTextureID)next++);
                texture->SetStatus(ImTextureStatus_OK);
            } else if (texture->Status == ImTextureStatus_WantUpdates) {
                texture->SetStatus(ImTextureStatus_OK);
            } else if (texture->Status == ImTextureStatus_WantDestroy) {
                texture->SetTexID(ImTextureID_Invalid);
                texture->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    void demo() {
        ImGui::SetNextWindowPos(ImVec2(20, 20));
        ImGui::SetNextWindowSize(ImVec2(900, 1000));
        ImGui::ShowMetricsWindow();
        ImGui::SetNextWindowPos(ImVec2(940, 20));
        ImGui::SetNextWindowSize(ImVec2(940, 1000));
        ImGui::Begin("Widgets");
        ImGui::TextWrapped("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.");

        static float value = 0.4f;
        static bool check = true;
        static int choice = 1;
        float samples[64];

        for (int i = 0; i < 64; i++) {
            samples[i] = sinf((float)i * 0.3f) * cosf((float)i * 0.07f);
        }

        ImGui::SliderFloat("slider", &value, 0.f, 1.f);
        ImGui::Checkbox("check", &check);
        ImGui::RadioButton("one", &choice, 0);
        ImGui::SameLine();
        ImGui::RadioButton("two", &choice, 1);
        ImGui::ProgressBar(value);
        ImGui::PlotLines("lines", samples, 64, 0, nullptr, -1.f, 1.f, ImVec2(0, 120));
        ImGui::PlotHistogram("histogram", samples, 64, 0, nullptr, -1.f, 1.f, ImVec2(0, 120));
        ImGui::ColorButton("color", ImVec4(0.9f, 0.3f, 0.1f, 0.6f));

        if (ImGui::BeginTable("table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            for (int row = 0; row < 12; row++) {
                ImGui::TableNextRow();

                for (int column = 0; column < 3; column++) {
                    ImGui::TableSetColumnIndex(column);
                    ImGui::Text("cell %d,%d", row, column);
                }
            }

            ImGui::EndTable();
        }

        ImDrawList* list = ImGui::GetWindowDrawList();
        ImVec2 at = ImGui::GetCursorScreenPos();

        list->AddCircleFilled(ImVec2(at.x + 60, at.y + 60), 50, IM_COL32(80, 160, 240, 200));
        list->AddCircle(ImVec2(at.x + 180, at.y + 60), 50, IM_COL32(240, 200, 80, 255), 0, 3.f);
        list->AddRectFilled(ImVec2(at.x + 260, at.y + 10), ImVec2(at.x + 400, at.y + 110), IM_COL32(200, 60, 120, 128), 16.f);
        list->AddLine(ImVec2(at.x + 420, at.y + 10), ImVec2(at.x + 600, at.y + 110), IM_COL32(255, 255, 255, 255), 2.f);
        list->AddBezierCubic(ImVec2(at.x + 620, at.y + 110), ImVec2(at.x + 660, at.y), ImVec2(at.x + 740, at.y + 120), ImVec2(at.x + 780, at.y + 10), IM_COL32(120, 255, 120, 255), 4.f);
        ImGui::End();
    }

    void play(float width, float height) {
        float pad = 8.f;
        float bar = ImGui::GetFrameHeight() + 2.f * pad;
        ImU32 black = IM_COL32(0, 0, 0, 255);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(width, height));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::Begin("##play", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar(2);

        ImDrawList* list = ImGui::GetWindowDrawList();
        float videoHeight = height - bar;
        float w = width;
        float h = w * 9.f / 16.f;

        if (h > videoHeight) {
            h = videoHeight;
            w = h * 16.f / 9.f;
        }

        ImVec2 p0(floorf((width - w) / 2.f), floorf((videoHeight - h) / 2.f));
        ImVec2 p1(p0.x + floorf(w), p0.y + floorf(h));

        video = ImVec4(p0.x, p0.y, p1.x, p1.y);
        hasVideo = true;

        list->AddRectFilled(ImVec2(0, videoHeight), ImVec2(width, height), ImGui::GetColorU32(ImGuiCol_WindowBg));
        list->AddRectFilled(ImVec2(0, 0), ImVec2(width, p0.y), black);
        list->AddRectFilled(ImVec2(0, p1.y), ImVec2(width, videoHeight), black);
        list->AddRectFilled(ImVec2(0, p0.y), ImVec2(p0.x, p1.y), black);
        list->AddRectFilled(ImVec2(p1.x, p0.y), ImVec2(width, p1.y), black);
        ImGui::SetCursorScreenPos(ImVec2(pad, videoHeight + pad));
        ImGui::Button("Pause##toggle", ImVec2(160, 0));
        ImGui::SameLine();
        ImGui::Button("Stop", ImVec2(160, 0));
        ImGui::SameLine();

        static float position = 1234.f;
        const char* time = "00:20:34 / 01:45:12";

        ImGui::SetNextItemWidth(width - ImGui::GetCursorPosX() - ImGui::CalcTextSize(time).x - 3.f * pad);
        ImGui::SliderFloat("##position", &position, 0.f, 6312.f, "", ImGuiSliderFlags_NoInput);
        ImGui::SameLine();
        ImGui::TextUnformatted(time);
        ImGui::End();
    }

    void menu(float width, float height) {
        play(width, height);
        ImGui::SetNextWindowPos(ImVec2(video.x + (video.z - video.x) * 0.55f, video.y + (video.w - video.y) * 0.3f));
        ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

        const char* items[] = {"Play", "Pause", "Next frame", "Previous frame", "Audio track", "Subtitles", "Screenshot", "Copy time", "Fullscreen", "Properties"};

        for (const char* item : items) {
            ImGui::MenuItem(item);
        }

        ImGui::Separator();
        ImGui::MenuItem("Close");
        ImGui::End();
    }

    void view(float width, float height) {
        float side = floorf(width / 5.f);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(side, height));
        ImGui::Begin("##list", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        for (int i = 0; i < 60; i++) {
            StringBuilder name;

            name << StringView(u8"IMG_") << (u64)(2000 + i * 7) << StringView(u8"_holiday_") << (u64)i << StringView(u8".jxl");
            ImGui::Selectable(name.cStr(), i == 5);
        }

        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(side, 0));
        ImGui::SetNextWindowSize(ImVec2(width - 2.f * side, height));
        ImGui::Begin("##canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::GetWindowDrawList()->AddImage(ImTextureRef((ImTextureID)canvasTexture), ImVec2(side + 40.f, 40.f), ImVec2(width - side - 40.f, height - 40.f));
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(width - side, 0));
        ImGui::SetNextWindowSize(ImVec2(side, height));
        ImGui::Begin("##properties", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        const char* labels[] = {"file", "size", "format", "depth", "color", "transfer", "camera", "lens", "exposure", "iso", "taken"};
        const char* values[] = {"IMG_2035_holiday_05.jxl", "6000 x 4000", "JPEG XL", "16 bit", "Display P3", "sRGB", "X-T5", "33mm f/1.4", "1/250", "160", "2024-07-14 18:22"};

        if (ImGui::BeginTable("##props", 2)) {
            for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", labels[i]);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(values[i]);
            }

            ImGui::EndTable();
        }

        ImGui::End();
    }

    void dump(Writer& out, const ImDrawData& data, bool canvas) {
        ImVector<ImTextureData*>& textures = ImGui::GetPlatformIO().Textures;
        u32 count = canvas ? 1 : 0;

        for (ImTextureData* texture : textures) {
            count += texture->Status == ImTextureStatus_OK ? 1 : 0;
        }

        out.word(0x46434d49u);
        out.word((u32)data.DisplaySize.x);
        out.word((u32)data.DisplaySize.y);
        out.word(count);

        for (ImTextureData* texture : textures) {
            if (texture->Status != ImTextureStatus_OK) {
                continue;
            }

            if (texture->Format != ImTextureFormat_RGBA32) {
                raiseError(StringView(u8"compositor_dump: the atlas is not RGBA32"));
            }

            out.word((u32)texture->TexID);
            out.word((u32)texture->Width);
            out.word((u32)texture->Height);
            out.bytes(texture->GetPixels(), (size_t)texture->Width * texture->Height * 4);
        }

        if (canvas) {
            static u8 pixels[canvasSide * canvasSide * 4];

            for (int y = 0; y < canvasSide; y++) {
                for (int x = 0; x < canvasSide; x++) {
                    u8* p = pixels + (y * canvasSide + x) * 4;

                    p[0] = (u8)x;
                    p[1] = (u8)y;
                    p[2] = (u8)(((x / 16) ^ (y / 16)) & 1 ? 220 : 40);
                    p[3] = 255;
                }
            }

            out.word(canvasTexture);
            out.word(canvasSide);
            out.word(canvasSide);
            out.bytes(pixels, sizeof(pixels));
        }

        out.word((u32)data.CmdListsCount);

        for (const ImDrawList* list : data.CmdLists) {
            out.word((u32)list->VtxBuffer.Size);

            for (const ImDrawVert& vertex : list->VtxBuffer) {
                out.real(vertex.pos.x);
                out.real(vertex.pos.y);
                out.real(vertex.uv.x);
                out.real(vertex.uv.y);
                out.word(vertex.col);
            }

            out.word((u32)list->IdxBuffer.Size);

            for (ImDrawIdx index : list->IdxBuffer) {
                out.word(index);
            }

            u32 commands = 0;

            for (const ImDrawCmd& command : list->CmdBuffer) {
                commands += command.UserCallback || !command.ElemCount ? 0 : 1;
            }

            out.word(commands);

            for (const ImDrawCmd& command : list->CmdBuffer) {
                if (command.UserCallback || !command.ElemCount) {
                    continue;
                }

                out.real(command.ClipRect.x);
                out.real(command.ClipRect.y);
                out.real(command.ClipRect.z);
                out.real(command.ClipRect.w);
                out.word((u32)command.GetTexID());
                out.word(command.VtxOffset);
                out.word(command.IdxOffset);
                out.word(command.ElemCount);
            }
        }

        out.word(hasVideo ? 1 : 0);

        if (hasVideo) {
            out.real(video.x);
            out.real(video.y);
            out.real(video.z);
            out.real(video.w);
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 4) {
        sysE << StringView(u8"usage: compositor_dump demo|play|menu|view WIDTH HEIGHT") << endL;

        return 2;
    }

    auto number = [](const char* text) {
        u32 value = 0;

        for (; *text >= '0' && *text <= '9'; text++) {
            value = value * 10 + (u32)(*text - '0');
        }

        return (float)value;
    };

    StringView scene(argv[1]);
    float width = number(argv[2]);
    float height = number(argv[3]);
    float scale = 2.f;

    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle style;

    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.DisplaySize = ImVec2(width, height);
    io.DeltaTime = 1.f / 60.f;
    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;
    ImGui::GetStyle() = style;

    Writer out;

    for (int frame = 0; frame < 4; frame++) {
        ImGui::NewFrame();

        if (scene == StringView(u8"demo")) {
            demo();
        } else if (scene == StringView(u8"play")) {
            play(width, height);
        } else if (scene == StringView(u8"view")) {
            view(width, height);
        } else if (scene == StringView(u8"menu")) {
            menu(width, height);
        } else {
            raiseError(StringView(u8"compositor_dump: unknown scene"));
        }

        ImGui::Render();
        serveTextures();
    }

    dump(out, *ImGui::GetDrawData(), scene == StringView(u8"view"));
    sysO.flush();
    ImGui::DestroyContext();

    return 0;
}
