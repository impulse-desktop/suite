#include "edit.h"

#include "ui.h"
#include "error.h"

#include <std/sys/fd.h>
#include <std/ios/sys.h>
#include <std/sys/throw.h>
#include <std/ios/out_fd.h>
#include <std/str/builder.h>
#include <std/thr/runable.h>
#include <std/ios/fs_utils.h>
#include <std/mem/obj_pool.h>

#include <errno.h>
#include <fcntl.h>
#include <imgui.h>
#include <string.h>
#include <sys/stat.h>
#include <TextEditor.h>

using namespace stl;

namespace {
    constexpr Design windowWidth = 900_d;
    constexpr Design windowHeight = 650_d;
    constexpr size_t tabSize = 4;

    StringView nameOf(StringView path) {
        size_t slash = path.length();

        while (slash > 0 && path[slash - 1] != '/') {
            slash--;
        }

        return StringView(path.begin() + slash, path.end());
    }

    bool hasSuffix(StringView name, StringView suffix) {
        if (name.length() < suffix.length()) {
            return false;
        }

        for (size_t i = 0; i < suffix.length(); i++) {
            if (((u8)name[name.length() - suffix.length() + i] | 0x20) != (u8)suffix[i]) {
                return false;
            }
        }

        return true;
    }

    // the editor's language for a file, by its name's extension
    const TextEditor::Language* languageFor(StringView name) {
        struct Rule {
            StringView suffix;
            const TextEditor::Language* (*language)();
        };

        static const Rule rules[] = {
            {StringView(u8".c"), TextEditor::Language::C},
            {StringView(u8".h"), TextEditor::Language::C},
            {StringView(u8".cpp"), TextEditor::Language::Cpp},
            {StringView(u8".cc"), TextEditor::Language::Cpp},
            {StringView(u8".cxx"), TextEditor::Language::Cpp},
            {StringView(u8".hpp"), TextEditor::Language::Cpp},
            {StringView(u8".hh"), TextEditor::Language::Cpp},
            {StringView(u8".mm"), TextEditor::Language::Cpp},
            {StringView(u8".cs"), TextEditor::Language::Cs},
            {StringView(u8".lua"), TextEditor::Language::Lua},
            {StringView(u8".py"), TextEditor::Language::Python},
            {StringView(u8".glsl"), TextEditor::Language::Glsl},
            {StringView(u8".vert"), TextEditor::Language::Glsl},
            {StringView(u8".frag"), TextEditor::Language::Glsl},
            {StringView(u8".comp"), TextEditor::Language::Glsl},
            {StringView(u8".hlsl"), TextEditor::Language::Hlsl},
            {StringView(u8".json"), TextEditor::Language::Json},
            {StringView(u8".md"), TextEditor::Language::Markdown},
            {StringView(u8".sql"), TextEditor::Language::Sql},
        };

        for (const Rule& rule : rules) {
            if (hasSuffix(name, rule.suffix)) {
                return rule.language();
            }
        }

        return nullptr;
    }

    // the editor speaks std::string; the suite's strings are views
    StringView viewOf(const std::string& text) {
        return StringView((const u8*)text.data(), text.size());
    }

    // one file in the editor: read on start, written on Ctrl+S; the undo
    // index at the last write says whether there is anything unsaved
    struct EditApp {
        Ui* ui = nullptr;
        Buffer path;
        StringView name;
        TextEditor editor;
        size_t savedIndex = 0;
        bool modified = false;
        bool asking = false;
        bool quit = false;
        bool fullscreen = false;

        void open(StringView given);
        void write(StringView to);
        void save();
        void keep();
        void askQuit();
        void keys();
        void draw();
    };
}

void EditApp::open(StringView given) {
    struct stat st;

    path = Buffer(given);
    name = nameOf(StringView(path));
    editor.SetTabSize(tabSize);
    editor.SetShowLineNumbersEnabled(true);
    editor.SetShowMatchingBrackets(true);
    editor.SetLanguage(languageFor(name));

    if (stat(path.cStr(), &st) != 0) {
        if (errno != ENOENT) {
            fail(StringView(StringBuilder() << StringView(u8"cannot open ") << name << StringView(u8": ") << StringView(strerror(errno))));
        }

        TRACE(ui, StringView(StringBuilder() << StringView(u8"new ") << name));
    } else {
        Buffer bytes;

        readFileContent(path, bytes);
        editor.SetText(std::string_view((const char*)bytes.data(), bytes.length()));
        TRACE(ui, StringView(StringBuilder() << StringView(u8"opened ") << name << StringView(u8" lines=") << (i64)editor.GetLineCount()));
    }

    savedIndex = editor.GetUndoIndex();
    editor.SetFocus();
    TRACE(ui, StringView(StringBuilder() << StringView(u8"language ") << viewOf(editor.GetLanguageName())));
}

void EditApp::write(StringView to) {
    std::string text = editor.GetText();
    Buffer file(to);
    ScopedFD fd(::open(file.cStr(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));

    if (fd.get() < 0) {
        fail(StringView(StringBuilder() << StringView(u8"cannot write ") << to << StringView(u8": ") << StringView(strerror(errno))));
    }

    FDRegular out(fd);

    out.write(text.data(), text.size());
    out.flush();
    TRACE(ui, StringView(StringBuilder() << StringView(u8"saved ") << nameOf(to) << StringView(u8" bytes=") << (i64)text.size()));
}

void EditApp::save() {
    write(StringView(path));
    savedIndex = editor.GetUndoIndex();
    ui->requestFrame();
}

// the window is going, and the Ui does not take that back: the unsaved
// text goes next to the file, under the backup name
void EditApp::keep() {
    if (modified) {
        Buffer copy = Buffer(StringView(path));

        copy.append("~", 1);
        write(StringView(copy));
        sysE << StringView(u8"im edit: unsaved changes kept in ") << StringView(copy) << endL;
    }
}

void EditApp::askQuit() {
    if (!modified) {
        quit = true;

        return;
    }

    asking = true;
    TRACE(ui, StringView(u8"asking"));
    ui->requestFrame();
}

void EditApp::keys() {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        save();
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) {
        askQuit();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
        fullscreen = !fullscreen;
        ui->requestFullscreen(fullscreen);
        TRACE(ui, fullscreen ? StringView(u8"fullscreen on") : StringView(u8"fullscreen off"));
    }
}

// the editor over the window, a status line under it: the file, a mark
// when it has unsaved changes, the cursor and the language
void EditApp::draw() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiStyle& style = ImGui::GetStyle();

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##edit", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    float statusH = ImGui::GetFrameHeightWithSpacing();

    if (editor.Render("##editor", ImVec2(0.f, -statusH))) {
        ui->requestFrame();
    }

    bool now = editor.GetUndoIndex() != savedIndex;

    if (now != modified) {
        modified = now;
        TRACE(ui, modified ? StringView(u8"modified") : StringView(u8"saved state"));
        ui->requestFrame();
    }

    TextEditor::DocPos at = editor.GetMainCursorPosition();
    StringBuilder left;
    StringBuilder right;

    left << name;

    if (modified) {
        left << StringView(u8" \xe2\x80\xa2");
    }

    right << StringView(u8"Ln ") << (i64)(at.line + 1) << StringView(u8", Col ") << (i64)(at.index + 1) << StringView(u8"   ") << viewOf(editor.GetLanguageName());

    StringView leftText(left);
    StringView rightText(right);
    ImVec2 rightSize = ImGui::CalcTextSize((const char*)rightText.begin(), (const char*)rightText.end());

    ImGui::SetCursorPosX(style.FramePadding.x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted((const char*)leftText.begin(), (const char*)leftText.end());
    ImGui::SameLine(vp->Size.x - rightSize.x - style.FramePadding.x);
    ImGui::TextUnformatted((const char*)rightText.begin(), (const char*)rightText.end());

    if (asking) {
        ImGui::OpenPopup("Unsaved changes");
        asking = false;
    }

    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%.*s has unsaved changes.", (int)name.length(), (const char*)name.begin());
        ImGui::Spacing();

        bool doSave = ImGui::Button("Save") || ImGui::IsKeyPressed(ImGuiKey_S);

        ImGui::SameLine();

        bool doDiscard = ImGui::Button("Discard") || ImGui::IsKeyPressed(ImGuiKey_D);

        ImGui::SameLine();

        bool doCancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);

        if (doSave) {
            save();
            quit = true;
            ImGui::CloseCurrentPopup();
        } else if (doDiscard) {
            TRACE(ui, StringView(u8"discarded"));
            modified = false;
            quit = true;
            ImGui::CloseCurrentPopup();
        } else if (doCancel) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::End();
}

int mainEdit(ObjPool& pool, int argc, char** argv) {
    if (argc != 2) {
        sysE << StringView(u8"usage: im edit <file>") << endL;

        return 2;
    }

    EditApp& app = *pool.make<EditApp>();
    Ui& ui = *Ui::create(pool, StringView(u8"edit"), UiOptions{windowWidth, windowHeight});

    app.ui = &ui;
    app.open(StringView(argv[1]));

    auto body = makeRunable([&] {
        UiEvent event;

        while (ui.next(event)) {
            if (event.kind == UiEvent::Kind::Close) {
                app.keep();

                return;
            }

            app.keys();
            app.draw();

            if (app.quit) {
                TRACE(&ui, StringView(u8"quit"));

                return;
            }
        }
    });
    return ui.run(body);
}
