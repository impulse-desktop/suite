#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

namespace stl {
    class ObjPool;
}

namespace plt {
    enum class InputKey : u8 {
        Unknown,
        Printable,
        Space,
        Escape,
        Enter,
        Backspace,
        Tab,
        Insert,
        Delete,
        Home,
        End,
        Up,
        Down,
        Left,
        Right,
        PageUp,
        PageDown,
        Clear,
        F1,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,
        F13,
        F14,
        F15,
        F16,
        F17,
        F18,
        F19,
        F20,
        F21,
        F22,
        F23,
        F24,
        F25,
        F26,
        F27,
        F28,
        F29,
        F30,
        F31,
        F32,
        F33,
        F34,
        F35,
        Keypad0,
        Keypad1,
        Keypad2,
        Keypad3,
        Keypad4,
        Keypad5,
        Keypad6,
        Keypad7,
        Keypad8,
        Keypad9,
        KeypadDecimal,
        KeypadDivide,
        KeypadMultiply,
        KeypadSubtract,
        KeypadAdd,
        KeypadEnter,
        KeypadEqual,
        KeypadSeparator,
        KeypadF1,
        KeypadF2,
        KeypadF3,
        KeypadF4,
        KeypadInsert,
        KeypadDelete,
        KeypadUp,
        KeypadDown,
        KeypadLeft,
        KeypadRight,
        KeypadHome,
        KeypadEnd,
        KeypadPageUp,
        KeypadPageDown,
        KeypadBegin,
        KeypadSpace,
        KeypadTab,
        CapsLock,
        ScrollLock,
        NumLock,
        PrintScreen,
        Pause,
        Menu,
        LeftShift,
        LeftControl,
        LeftAlt,
        LeftSuper,
        RightShift,
        RightControl,
        RightAlt,
        RightSuper,
        MediaPlay,
        MediaPause,
        MediaPlayPause,
        MediaReverse,
        MediaStop,
        MediaFastForward,
        MediaRewind,
        MediaTrackNext,
        MediaTrackPrevious,
        MediaRecord,
        VolumeDown,
        VolumeUp,
        VolumeMute,
        Count
    };

    enum class InputAction : u8 {
        Press,
        Repeat,
        Release
    };

    enum InputModifier : u16 {
        InputShift = 1 << 0,
        InputControl = 1 << 1,
        InputAlt = 1 << 2,
        InputSuper = 1 << 3,
        InputCapsLock = 1 << 4,
        InputNumLock = 1 << 5,
        InputAltGraph = 1 << 6
    };

    struct KeyInput {
        InputKey key = InputKey::Unknown;
        InputAction action = InputAction::Press;
        u16 modifiers = 0;
        // Unicode identities for the unshifted active and base layouts,
        // followed by the key produced with Shift in the active layout.
        u32 layoutCodepoint = 0;
        u32 baseCodepoint = 0;
        u32 shiftedCodepoint = 0;
    };

    struct TextInput {
        u32 codepoint = 0;
        u16 modifiers = 0;
    };

    enum class PointerButton : u8 {
        Primary,
        Secondary,
        Middle,
        Auxiliary1,
        Auxiliary2,
        Auxiliary3,
        Auxiliary4,
        Auxiliary5
    };

    struct PointerMotionInput {
        int pixelX = 0;
        int pixelY = 0;
        u16 modifiers = 0;
    };

    struct PointerButtonInput {
        PointerButton button = PointerButton::Primary;
        bool pressed = false;
        int pixelX = 0;
        int pixelY = 0;
        u16 modifiers = 0;
        double time = 0;
    };

    enum class ScrollPhase : u8 {
        None,
        Begin,
        Update,
        End,
        Cancel
    };

    struct ScrollInput {
        double x = 0;
        double y = 0;
        int pixelX = 0;
        int pixelY = 0;
        u16 modifiers = 0;
        ScrollPhase phase = ScrollPhase::None;
        bool precise = false;
        bool momentum = false;
        double time = 0;
    };

    struct InputSink {
        virtual void key(const KeyInput& input) = 0;
        virtual void text(const TextInput& input) = 0;
        // Input-method composition preview. text is UTF-8 and valid only for
        // the duration of the call; cursorBegin/cursorEnd are byte offsets
        // into text, or -1 when the input method hides the preedit cursor.
        // An empty text clears the preview.
        virtual void preedit(stl::StringView text, i32 cursorBegin, i32 cursorEnd) = 0;
        virtual void pointerMotion(const PointerMotionInput& input) = 0;
        virtual void pointerButton(const PointerButtonInput& input) = 0;
        virtual void scroll(const ScrollInput& input) = 0;
        virtual void focus(bool focused) = 0;
        virtual void pointerPresence(bool present) = 0;
        virtual void flush() = 0;
    };

    struct Scheduler;

    // Rehosts every delivery of target onto one long-lived fiber, in
    // arrival order. Producers enqueue from the event loop and return
    // immediately; a delivery that blocks holds back later input, not the
    // loop. Transient payloads are copied at the boundary.
    InputSink* createFiberInputSink(stl::ObjPool& owner, Scheduler& scheduler, InputSink& target);
}
