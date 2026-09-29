#pragma once

#include "drop.h"
#include "input.h"
#include "platform.h"
#include "poller.h"
#include "window.h"

#include <linux/input-event-codes.h>

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/view.h>

namespace plt::test {
    enum class Command : u32 {
        DeferInitialConfigure,
        ReleaseInitialConfigure,
        QueryInitialConfigure,
        PointerEnter,
        PreferredScale,
        QuerySelection,
        QueryMinimum,
        OfferSelection,
        OfferPlainSelection,
        OfferUnsupportedSelection,
        ReleaseRead,
        RequestSourceData,
        RequestBrokenSourceData,
        CancelSources,
        ReleaseWrite,
        QueryWrite,
        AwaitTitles,
        ConfigureWindowState,
        ConfigureWindowResize,
        CloseWindow,
        QueryWindowRequests,
        QueryDecoration,
        QueryWindowGeometry,
        QueryFrames,
        CompleteFrames,
        PointerSequence,
        QueryCursor,
        KeyboardEnter,
        KeyboardPress,
        KeyboardRelease,
        KeyboardControl,
        KeyboardControlShift,
        KeyboardControlCapsLock,
        KeyboardRussianControl,
        KeyboardLeave,
        KeyboardMatrix,
        KeyboardCompose,
        InvalidKeymap,
        QueryActivation,
        QueryPrimarySelection,
        OfferPrimarySelection,
        RequestPrimarySourceData,
        PointerValue120,
        PointerFingerPhases,
        KeyboardEnterWithKeys,
        RemoveOutput,
        RestoreOutput,
        TextInputEnter,
        TextInputPreedit,
        TextInputCommitString,
        TextInputCommitInvalid,
        RemoveSeat,
        DragEnter,
        DragEnterUtf8String,
        DragEnterUriList,
        DragMotion,
        DragDrop,
        DragLeave,
        DragData,
        DragUriData,
        QueryDragAccept,
        QueryDragFinish,
        CursorShapeV1,
        PointerButtons,
        IntegerScaleOnly,
        SurfaceEnter,
        LegacyGlobals,
        QuerySelectionSerial,
        QueryTextInput,
        QueryTextInputRect,
        KeyboardKeysymSweep,
        KeyboardNumpadSweep,
        KeyboardNumLock,
        PingClient,
        PointerLegacyDiscrete,
        SurfacePreferredScale,
        TextInputLeave,
        Quit,
    };

    // One press+release per row, in table order, under the stock
    // us(evdev) keymap: the client asserts the exact InputKey sequence,
    // so the whole keysym switch is walked. Rows with checked=false
    // still deliver (and cover) their translation, but skip the
    // assertion where the evdev keymap does not pin the keysym down.
    struct KeysymSweepKey {
        u32 keycode;
        InputKey key;
        bool checked;
    };

    inline constexpr KeysymSweepKey keysymSweepKeys[] = {
        {KEY_ESC, InputKey::Escape, true},
        {KEY_ENTER, InputKey::Enter, true},
        {KEY_BACKSPACE, InputKey::Backspace, true},
        {KEY_TAB, InputKey::Tab, true},
        {KEY_INSERT, InputKey::Insert, true},
        {KEY_DELETE, InputKey::Delete, true},
        {KEY_HOME, InputKey::Home, true},
        {KEY_END, InputKey::End, true},
        {KEY_UP, InputKey::Up, true},
        {KEY_DOWN, InputKey::Down, true},
        {KEY_LEFT, InputKey::Left, true},
        {KEY_RIGHT, InputKey::Right, true},
        {KEY_PAGEUP, InputKey::PageUp, true},
        {KEY_PAGEDOWN, InputKey::PageDown, true},
        {KEY_F1, InputKey::F1, true},
        {KEY_F2, InputKey::F2, true},
        {KEY_F3, InputKey::F3, true},
        {KEY_F4, InputKey::F4, true},
        {KEY_F5, InputKey::F5, true},
        {KEY_F6, InputKey::F6, true},
        {KEY_F7, InputKey::F7, true},
        {KEY_F8, InputKey::F8, true},
        {KEY_F9, InputKey::F9, true},
        {KEY_F10, InputKey::F10, true},
        {KEY_F11, InputKey::F11, true},
        {KEY_F12, InputKey::F12, true},
        // Without NumLock the pad digits carry their navigation keysyms.
        {KEY_KP7, InputKey::KeypadHome, true},
        {KEY_KP8, InputKey::KeypadUp, true},
        {KEY_KP9, InputKey::KeypadPageUp, true},
        {KEY_KP4, InputKey::KeypadLeft, true},
        {KEY_KP5, InputKey::KeypadBegin, true},
        {KEY_KP6, InputKey::KeypadRight, true},
        {KEY_KP1, InputKey::KeypadEnd, true},
        {KEY_KP2, InputKey::KeypadDown, true},
        {KEY_KP3, InputKey::KeypadPageDown, true},
        {KEY_KP0, InputKey::KeypadInsert, true},
        {KEY_KPDOT, InputKey::KeypadDelete, true},
        {KEY_KPSLASH, InputKey::KeypadDivide, true},
        {KEY_KPASTERISK, InputKey::KeypadMultiply, true},
        {KEY_KPMINUS, InputKey::KeypadSubtract, true},
        {KEY_KPPLUS, InputKey::KeypadAdd, true},
        {KEY_KPENTER, InputKey::KeypadEnter, true},
        {KEY_KPEQUAL, InputKey::KeypadEqual, false},
        {KEY_KPCOMMA, InputKey::KeypadSeparator, false},
        {KEY_CAPSLOCK, InputKey::CapsLock, true},
        {KEY_SCROLLLOCK, InputKey::ScrollLock, true},
        {KEY_NUMLOCK, InputKey::NumLock, true},
        {KEY_SYSRQ, InputKey::PrintScreen, true},
        {KEY_PAUSE, InputKey::Pause, true},
        // Neither key resolves to the Menu keysym under the bare 'us'
        // keymap the mock serves; they still walk the translation.
        {KEY_MENU, InputKey::Menu, false},
        {KEY_COMPOSE, InputKey::Unknown, false},
        {KEY_LEFTSHIFT, InputKey::LeftShift, true},
        {KEY_RIGHTSHIFT, InputKey::RightShift, true},
        {KEY_LEFTCTRL, InputKey::LeftControl, true},
        {KEY_RIGHTCTRL, InputKey::RightControl, true},
        {KEY_LEFTALT, InputKey::LeftAlt, true},
        {KEY_RIGHTALT, InputKey::RightAlt, true},
        {KEY_LEFTMETA, InputKey::LeftSuper, true},
        {KEY_RIGHTMETA, InputKey::RightSuper, true},
        {KEY_MUTE, InputKey::VolumeMute, true},
        {KEY_VOLUMEDOWN, InputKey::VolumeDown, true},
        {KEY_VOLUMEUP, InputKey::VolumeUp, true},
        {KEY_NEXTSONG, InputKey::MediaTrackNext, true},
        {KEY_PREVIOUSSONG, InputKey::MediaTrackPrevious, true},
        {KEY_PLAYPAUSE, InputKey::MediaPlay, false},
        {KEY_PLAYCD, InputKey::MediaPlay, false},
        {KEY_PAUSECD, InputKey::MediaPause, false},
        {KEY_STOPCD, InputKey::MediaStop, false},
        {KEY_RECORD, InputKey::MediaRecord, false},
        {KEY_REWIND, InputKey::MediaRewind, false},
        {KEY_FASTFORWARD, InputKey::MediaFastForward, false},
    };

    // The same pad keys once NumLock is locked: digits and the decimal.
    inline constexpr KeysymSweepKey keysymNumpadKeys[] = {
        {KEY_KP0, InputKey::Keypad0, true},
        {KEY_KP1, InputKey::Keypad1, true},
        {KEY_KP2, InputKey::Keypad2, true},
        {KEY_KP3, InputKey::Keypad3, true},
        {KEY_KP4, InputKey::Keypad4, true},
        {KEY_KP5, InputKey::Keypad5, true},
        {KEY_KP6, InputKey::Keypad6, true},
        {KEY_KP7, InputKey::Keypad7, true},
        {KEY_KP8, InputKey::Keypad8, true},
        {KEY_KP9, InputKey::Keypad9, true},
        {KEY_KPDOT, InputKey::KeypadDecimal, true},
    };

    struct Reply {
        u32 count = 0;
        i32 first = 0;
        i32 second = 0;
    };

    enum WindowRequest : u32 {
        UpdatedTitle = 1 << 0,
        InitialAppId = 1 << 1,
        Move = 1 << 2,
        Maximize = 1 << 3,
        Unmaximize = 1 << 4,
        Fullscreen = 1 << 5,
        Unfullscreen = 1 << 6,
        Minimize = 1 << 7,
    };

    Reply command(int fd, Command value);
    void pump(Platform& platform);
    stl::Buffer repeated(size_t size, u8 value);

    struct Client {
        explicit Client(int controlFd, u32 width = 800, u32 minimum = 1, WindowEvents* events = nullptr, InputSink* input = nullptr, bool waitForConfigure = true, FrameCallback* frame = nullptr, DropTarget* drop = nullptr);

        int controlFd;
        stl::ObjPool::Ref owner;
        Platform* platform = nullptr;
        Window* window = nullptr;
    };

    struct StreamRead {
        stl::Buffer content;
        u32 chunks = 0;
        bool complete = false;
    };

    // Reads clipboard.read() to end of payload on a fresh fiber; the fiber
    // owns the stream, so complete flips only after the delete.
    void readOnFiber(Platform& platform, Clipboard& clipboard, StreamRead& read);
    // Reads at most one chunk on a fiber, then deletes the stream: the
    // consumer-side abort of a transfer.
    void abortOnFiber(Platform& platform, Clipboard& clipboard, StreamRead& read);
    void writeClipboard(Clipboard& clipboard, stl::StringView content);

    struct EventSink final: WindowEvents, FrameCallback {
        void close() override {
            ++closeCount;
        }

        bool frame(const WindowInfo& info) override {
            ++frameCount;
            lastInfo = info;
            return submitFrames;
        }

        WindowInfo lastInfo;
        u32 closeCount = 0;
        u32 frameCount = 0;
        bool submitFrames = false;
    };

    struct InputRecorder final: InputSink {
        void key(const KeyInput& input) override {
            lastKey = input;
            if (input.action == InputAction::Press) {
                pressedKey = input;
                if (pressedKeyCount != sizeof(pressedKeys) / sizeof(pressedKeys[0])) {
                    pressedKeys[pressedKeyCount++] = input.key;
                }
                ++pressCount;
            } else if (input.action == InputAction::Repeat) {
                ++repeatCount;
            } else {
                ++releaseCount;
            }
        }

        void text(const TextInput& input) override {
            lastText = input;
            if (textCodepointCount != sizeof(textCodepoints) / sizeof(textCodepoints[0])) {
                textCodepoints[textCodepointCount++] = input.codepoint;
            }
            ++textCount;
        }

        void preedit(stl::StringView text, i32 cursorBegin, i32 cursorEnd) override {
            lastPreedit.reset();
            lastPreedit.append(text.data(), text.length());
            lastPreeditCursorBegin = cursorBegin;
            lastPreeditCursorEnd = cursorEnd;
            ++preeditCount;
        }

        void pointerMotion(const PointerMotionInput& input) override {
            lastMotion = input;
            ++motionCount;
        }

        void pointerButton(const PointerButtonInput& input) override {
            lastButton = input;
            if (input.pressed) {
                buttonMask |= 1u << static_cast<u8>(input.button);
                ++buttonPressCount;
            } else {
                ++buttonReleaseCount;
            }
        }

        void scroll(const ScrollInput& input) override {
            lastScroll = input;
            if (scrollCount < sizeof(scrolls) / sizeof(scrolls[0])) {
                scrolls[scrollCount] = input;
            }
            ++scrollCount;
        }

        void focus(bool focused) override {
            if (focused) {
                ++focusCount;
            } else {
                ++blurCount;
            }
        }

        void pointerPresence(bool present) override {
            if (present) {
                ++pointerEnterCount;
            } else {
                ++pointerLeaveCount;
            }
        }

        void flush() override {
            ++flushCount;
        }

        KeyInput pressedKey;
        InputKey pressedKeys[16]{};
        u32 pressedKeyCount = 0;
        KeyInput lastKey;
        TextInput lastText;
        u32 textCodepoints[16]{};
        u32 textCodepointCount = 0;
        stl::Buffer lastPreedit;
        i32 lastPreeditCursorBegin = -1;
        i32 lastPreeditCursorEnd = -1;
        u32 preeditCount = 0;
        PointerMotionInput lastMotion;
        PointerButtonInput lastButton;
        ScrollInput lastScroll;
        ScrollInput scrolls[8]{};
        u32 pressCount = 0;
        u32 repeatCount = 0;
        u32 releaseCount = 0;
        u32 textCount = 0;
        u32 motionCount = 0;
        u32 buttonPressCount = 0;
        u32 buttonReleaseCount = 0;
        u32 buttonMask = 0;
        u32 scrollCount = 0;
        u32 focusCount = 0;
        u32 blurCount = 0;
        u32 pointerEnterCount = 0;
        u32 pointerLeaveCount = 0;
        u32 flushCount = 0;
    };

    struct StopOnClose final: WindowEvents {
        explicit StopOnClose(Platform*& platform_)
            : platform(platform_)
        {
        }

        void close() override {
            closed = true;
            platform->stop();
        }

        Platform*& platform;
        bool closed = false;
    };

    bool nonblockingShow(int fd);
    bool windowApi(int fd);
    bool decorations(int fd);
    bool frameApi(int fd);
    bool frameRetry(int fd);
    bool pointerInput(int fd);
    bool keyboardInput(int fd);
    bool localSelections(int fd);
    bool missingSelections(int fd);
    bool rejectedSelection(int fd);
    bool pollerApi(int fd);
    bool deferredClipboard(int fd);
    bool fractionalRounding(int fd);
    bool minimumAfterScale(int fd);
    bool asynchronousRead(int fd);
    bool asynchronousPrimary(int fd);
    bool cancelAsynchronousRead(int fd);
    bool cancelReadyClipboardRead(int fd);
    bool asynchronousWrite(int fd);
    bool brokenClipboardConsumer(int fd);
    bool flushBackpressure(int fd);
    bool queuedWaylandEvent(int fd);
    bool foreignQueuedWaylandEvent(int fd);
    bool plainMimeSelection(int fd);
    bool unsupportedMimeSelection(int fd);
    bool sourceCancellation(int fd);
    bool invalidKeymap(int fd);
    bool multipleWindows(int fd);
    bool scrollValue120(int fd);
    bool scrollFingerPhases(int fd);
    bool keyboardEnterKeys(int fd);
    bool outputRemoval(int fd);
    bool textInput(int fd);
    bool cursorShapes(int fd);
    bool cursorShapesV1(int fd);
    bool inputMatrix(int fd);
    bool keysymMatrix(int fd);
    bool composeInput(int fd);
    bool integerScaleFallback(int fd);
    bool legacyGlobals(int fd);
    bool fiberClipboard(int fd);
    bool textDrop(int fd);
    bool utf8StringDrop(int fd);
    bool uriListDrop(int fd);
    bool rawDropApi(int fd);
    bool rejectedDrag(int fd);
    bool cancelledDrag(int fd);
}
