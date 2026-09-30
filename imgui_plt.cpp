#include "imgui_plt.h"

#include "gpu.h"

#include "util.h"

#include <std/ios/sys.h>
#include <std/str/view.h>
#include <std/sys/types.h>
#include <std/mem/obj_pool.h>

#include <time.h>
#include <float.h>
#include <imgui.h>
#include <plt/input.h>
#include <plt/window.h>

using namespace stl;

namespace {
    // indexed by plt::InputKey; keep in that enum's declaration order
    constexpr ImGuiKey namedKeys[] = {
        ImGuiKey_None, // Unknown
        ImGuiKey_None, // Printable: mapped from the base codepoint instead
        ImGuiKey_Space,
        ImGuiKey_Escape,
        ImGuiKey_Enter,
        ImGuiKey_Backspace,
        ImGuiKey_Tab,
        ImGuiKey_Insert,
        ImGuiKey_Delete,
        ImGuiKey_Home,
        ImGuiKey_End,
        ImGuiKey_UpArrow,
        ImGuiKey_DownArrow,
        ImGuiKey_LeftArrow,
        ImGuiKey_RightArrow,
        ImGuiKey_PageUp,
        ImGuiKey_PageDown,
        ImGuiKey_None, // Clear
        ImGuiKey_F1,
        ImGuiKey_F2,
        ImGuiKey_F3,
        ImGuiKey_F4,
        ImGuiKey_F5,
        ImGuiKey_F6,
        ImGuiKey_F7,
        ImGuiKey_F8,
        ImGuiKey_F9,
        ImGuiKey_F10,
        ImGuiKey_F11,
        ImGuiKey_F12,
        ImGuiKey_F13,
        ImGuiKey_F14,
        ImGuiKey_F15,
        ImGuiKey_F16,
        ImGuiKey_F17,
        ImGuiKey_F18,
        ImGuiKey_F19,
        ImGuiKey_F20,
        ImGuiKey_F21,
        ImGuiKey_F22,
        ImGuiKey_F23,
        ImGuiKey_F24,
        ImGuiKey_None, // F25: ImGui stops at F24
        ImGuiKey_None, // F26
        ImGuiKey_None, // F27
        ImGuiKey_None, // F28
        ImGuiKey_None, // F29
        ImGuiKey_None, // F30
        ImGuiKey_None, // F31
        ImGuiKey_None, // F32
        ImGuiKey_None, // F33
        ImGuiKey_None, // F34
        ImGuiKey_None, // F35
        ImGuiKey_Keypad0,
        ImGuiKey_Keypad1,
        ImGuiKey_Keypad2,
        ImGuiKey_Keypad3,
        ImGuiKey_Keypad4,
        ImGuiKey_Keypad5,
        ImGuiKey_Keypad6,
        ImGuiKey_Keypad7,
        ImGuiKey_Keypad8,
        ImGuiKey_Keypad9,
        ImGuiKey_KeypadDecimal,
        ImGuiKey_KeypadDivide,
        ImGuiKey_KeypadMultiply,
        ImGuiKey_KeypadSubtract,
        ImGuiKey_KeypadAdd,
        ImGuiKey_KeypadEnter,
        ImGuiKey_KeypadEqual,
        ImGuiKey_None, // KeypadSeparator
        ImGuiKey_None, // KeypadF1
        ImGuiKey_None, // KeypadF2
        ImGuiKey_None, // KeypadF3
        ImGuiKey_None, // KeypadF4
        // numlock-off keypad navigation folds onto the plain nav keys
        ImGuiKey_Insert,
        ImGuiKey_Delete,
        ImGuiKey_UpArrow,
        ImGuiKey_DownArrow,
        ImGuiKey_LeftArrow,
        ImGuiKey_RightArrow,
        ImGuiKey_Home,
        ImGuiKey_End,
        ImGuiKey_PageUp,
        ImGuiKey_PageDown,
        ImGuiKey_None, // KeypadBegin
        ImGuiKey_Space,
        ImGuiKey_Tab,
        ImGuiKey_CapsLock,
        ImGuiKey_ScrollLock,
        ImGuiKey_NumLock,
        ImGuiKey_PrintScreen,
        ImGuiKey_Pause,
        ImGuiKey_Menu,
        ImGuiKey_LeftShift,
        ImGuiKey_LeftCtrl,
        ImGuiKey_LeftAlt,
        ImGuiKey_LeftSuper,
        ImGuiKey_RightShift,
        ImGuiKey_RightCtrl,
        ImGuiKey_RightAlt,
        ImGuiKey_RightSuper,
        ImGuiKey_None, // MediaPlay
        ImGuiKey_None, // MediaPause
        ImGuiKey_None, // MediaPlayPause
        ImGuiKey_None, // MediaReverse
        ImGuiKey_None, // MediaStop
        ImGuiKey_None, // MediaFastForward
        ImGuiKey_None, // MediaRewind
        ImGuiKey_None, // MediaTrackNext
        ImGuiKey_None, // MediaTrackPrevious
        ImGuiKey_None, // MediaRecord
        ImGuiKey_None, // VolumeDown
        ImGuiKey_None, // VolumeUp
        ImGuiKey_None, // VolumeMute
    };

    static_assert(sizeof(namedKeys) / sizeof(namedKeys[0]) == (size_t)plt::InputKey::Count, "the key table tracks plt::InputKey");

    ImGuiKey printableKey(u32 codepoint);
    int mouseButtonIndex(plt::PointerButton button);
    plt::PointerIcon pointerIcon(ImGuiMouseCursor cursor);

    struct ImGuiPltImpl final: ImGuiPlt, plt::InputSink {
        plt::InputSink* sink() override;
        void newFrame(plt::Window& window) override;

        void key(const plt::KeyInput& input) override;
        void text(const plt::TextInput& input) override;
        void preedit(StringView text, i32 cursorBegin, i32 cursorEnd) override;
        void pointerMotion(const plt::PointerMotionInput& input) override;
        void pointerButton(const plt::PointerButtonInput& input) override;
        void scroll(const plt::ScrollInput& input) override;
        void focus(bool focused) override;
        void pointerPresence(bool present) override;
        void flush() override;

        u64 frameNs = 0;
        // plt windows start with the Text icon; the first newFrame pushes
        // the ImGui choice (normally Default) by differing from it
        plt::PointerIcon icon = plt::PointerIcon::Text;
    };

    // ImGui keys name US-layout positions, so the layout-independent base
    // codepoint is the right source; letters, digits, the punctuation row
    // and the space (a printable key to the platform) are all ImGui cares
    // about
    ImGuiKey printableKey(u32 codepoint) {
        // an ASCII letter of either case folds to lower case by its 0x20
        // bit; no other codepoint lands in a-z that way
        u32 folded = codepoint | 0x20;

        if (folded >= 'a' && folded <= 'z') {
            return (ImGuiKey)(ImGuiKey_A + (int)(folded - 'a'));
        }

        if (codepoint >= '0' && codepoint <= '9') {
            return (ImGuiKey)(ImGuiKey_0 + (int)(codepoint - '0'));
        }

        switch (codepoint) {
            case ' ':
                return ImGuiKey_Space;
            case '\'':
                return ImGuiKey_Apostrophe;
            case ',':
                return ImGuiKey_Comma;
            case '-':
                return ImGuiKey_Minus;
            case '.':
                return ImGuiKey_Period;
            case '/':
                return ImGuiKey_Slash;
            case ';':
                return ImGuiKey_Semicolon;
            case '=':
                return ImGuiKey_Equal;
            case '[':
                return ImGuiKey_LeftBracket;
            case '\\':
                return ImGuiKey_Backslash;
            case ']':
                return ImGuiKey_RightBracket;
            case '`':
                return ImGuiKey_GraveAccent;
            default:
                return ImGuiKey_None;
        }
    }

    // what the screenshot tool, this binding's one user, ever makes ImGui
    // ask for: nothing calls SetMouseCursor, the Hand producers (TextLink,
    // the debug item picker) are never used, and no resize cursor can come
    // up: both top-level windows are NoDecoration (so NoResize), its child
    // windows set no ResizeX/Y, and it has no tables, columns or splitters.
    // Its zoom slider becomes a text field on Ctrl+click.
    plt::PointerIcon pointerIcon(ImGuiMouseCursor cursor) {
        switch (cursor) {
            case ImGuiMouseCursor_TextInput:
                return plt::PointerIcon::Text;
            default:
                return plt::PointerIcon::Default;
        }
    }

    int mouseButtonIndex(plt::PointerButton button) {
        switch (button) {
            case plt::PointerButton::Primary:
                return 0;
            case plt::PointerButton::Secondary:
                return 1;
            case plt::PointerButton::Middle:
                return 2;
            case plt::PointerButton::Auxiliary1:
                return 3;
            case plt::PointerButton::Auxiliary2:
                return 4;
            default:
                return -1;
        }
    }

    // the test build says what the platform handed ImGui, so a scenario
    // that fails on a frame can tell where the pointer was
    void traceInput(StringView what, i32 x, i32 y) {
#ifdef IM_FOR_TESTS
        sysO << "im input: "_sv << what << " "_sv << x << " "_sv << y << endL;
#else
        (void)what;
        (void)x;
        (void)y;
#endif
    }

    plt::InputSink* ImGuiPltImpl::sink() {
        return this;
    }

    void ImGuiPltImpl::newFrame(plt::Window& window) {
        ImGuiIO& io = ImGui::GetIO();
        plt::WindowInfo info = window.info();

        io.BackendPlatformName = "imgui_plt";
        io.DisplaySize = ImVec2((float)info.width, (float)info.height);

        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);

        u64 now = (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;

        io.DeltaTime = frameNs && now > frameNs ? (float)(now - frameNs) / 1e9f : 1.f / 60.f;
        frameNs = now;

        plt::PointerIcon wanted = pointerIcon(ImGui::GetMouseCursor());

        if (wanted != icon) {
            icon = wanted;
            window.requestPointerIcon(icon);
        }
    }

    void ImGuiPltImpl::key(const plt::KeyInput& input) {
        ImGuiIO& io = ImGui::GetIO();

        io.AddKeyEvent(ImGuiMod_Ctrl, (input.modifiers & plt::InputControl) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (input.modifiers & plt::InputShift) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (input.modifiers & plt::InputAlt) != 0);
        io.AddKeyEvent(ImGuiMod_Super, (input.modifiers & plt::InputSuper) != 0);

        // ImGui synthesizes repeats from held keys on its own
        if (input.action == plt::InputAction::Repeat) {
            return;
        }

        ImGuiKey key = input.key == plt::InputKey::Printable ? printableKey(input.baseCodepoint) : namedKeys[(int)input.key];

        if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, input.action == plt::InputAction::Press);
        }
    }

    void ImGuiPltImpl::text(const plt::TextInput& input) {
        ImGui::GetIO().AddInputCharacter(input.codepoint);
    }

    void ImGuiPltImpl::preedit(StringView, i32, i32) {
        // no composition preview: ImGui widgets have no preedit rendering
    }

    void ImGuiPltImpl::pointerMotion(const plt::PointerMotionInput& input) {
        traceInput("pointer"_sv, input.pixelX, input.pixelY);

        if (gTraceFrames) {
            sysE << "im pointer: "_sv << (i64)input.pixelX << " "_sv << (i64)input.pixelY << endL;
        }

        ImGui::GetIO().AddMousePosEvent((float)input.pixelX, (float)input.pixelY);
    }

    void ImGuiPltImpl::pointerButton(const plt::PointerButtonInput& input) {
        int button = mouseButtonIndex(input.button);

        if (button < 0) {
            return;
        }

        ImGuiIO& io = ImGui::GetIO();

        io.AddMousePosEvent((float)input.pixelX, (float)input.pixelY);
        io.AddMouseButtonEvent(button, input.pressed);
    }

    void ImGuiPltImpl::scroll(const plt::ScrollInput& input) {
        // the frame trace: every wheel event as plt hands it over, between
        // the frames' lines
        if (gTraceFrames) {
            sysE << "im wheel: y/100 "_sv << (i64)(input.y * 100.0) << " precise "_sv << (i64)input.precise << " momentum "_sv << (i64)input.momentum << " phase "_sv << (i64)input.phase << endL;
        }

        ImGui::GetIO().AddMouseWheelEvent((float)input.x, (float)input.y);
    }

    void ImGuiPltImpl::focus(bool focused) {
        ImGui::GetIO().AddFocusEvent(focused);
    }

    void ImGuiPltImpl::pointerPresence(bool present) {
        traceInput(present ? "pointer present"_sv : "pointer absent"_sv, 0, 0);

        if (!present) {
            ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
    }

    void ImGuiPltImpl::flush() {
        // plt batches per pointer frame; ImGui consumes its queue in NewFrame
    }
}

ImGuiPlt* ImGuiPlt::create(ObjPool& pool) {
    return pool.make<ImGuiPltImpl>();
}
