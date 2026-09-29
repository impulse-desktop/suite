#pragma once

#include "platform.h"
#include "window.h"

namespace plt {
    enum class HeadlessPixelFormat : u8 {
        RGB8
    };

    struct HeadlessRenderTarget {
        u8* pixels = nullptr;
        size_t length = 0;
        u32 width = 0;
        u32 height = 0;
        u32 stride = 0;
        HeadlessPixelFormat format = HeadlessPixelFormat::RGB8;
    };

    struct HeadlessFrame {
        const u8* pixels = nullptr;
        size_t length = 0;
        u32 width = 0;
        u32 height = 0;
        u32 stride = 0;
        HeadlessPixelFormat format = HeadlessPixelFormat::RGB8;
        u64 generation = 0;
    };

    struct WindowHeadless: Window {
        // Dispatches one requested frame through WindowOptions::frame. The
        // callback is deliberately never made inline from requestFrame().
        virtual bool dispatchFrame() = 0;
        virtual bool framePending() const = 0;
        virtual void configure(const WindowInfo& info) = 0;
        virtual void failNextPresentation() = 0;
        virtual HeadlessFrame presentedFrame() const = 0;
        virtual void setClipboards(Clipboard& primary, Clipboard& secondary) = 0;

        // Requests recorded for tests instead of reaching a real desktop.
        virtual PointerIcon pointerIcon() const = 0;
        virtual stl::StringView openedUri() const = 0;
        virtual u64 openUriCount() const = 0;
        virtual stl::StringView title() const = 0;
    };

    Platform* createHeadlessPlatform(stl::ObjPool& owner);
}
