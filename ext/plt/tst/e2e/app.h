#pragma once

#include "input.h"
#include "window.h"
#include "platform.h"

#include <std/mem/obj_pool.h>
#include <std/ptr/intrusive.h>

struct _cairo;

namespace plt::e2e {
    class Surface;

    struct Canvas {
        _cairo* context;
        u32 width;
        u32 height;

        void clear(u32 color);
        void rectangle(double x, double y, double width, double height, u32 color);
        void text(double x, double y, stl::StringView text, u32 color = 0xffffff);
    };

    // A small client of the public plt API. The renderer alone knows Wayland.
    class App: public InputSink, public FrameCallback, public WindowEvents {
    public:
        explicit App(Platform* shared = nullptr);
        virtual ~App();

        void open(const char* title, u32 width = 400, u32 height = 280);
        void run();
        virtual void paint(Canvas& canvas) = 0;
        bool frame(const WindowInfo& info) override;
        void close() override;
        void key(const KeyInput& input) override;
        void text(const TextInput& input) override;
        void preedit(stl::StringView text, i32 begin, i32 end) override;
        void pointerMotion(const PointerMotionInput& input) override;
        void pointerButton(const PointerButtonInput& input) override;
        void scroll(const ScrollInput& input) override;
        void focus(bool focused) override;
        void pointerPresence(bool present) override;
        void flush() override;

        stl::ObjPool::Ref owner;
        Platform* platform;
        Window* window = nullptr;
        Surface* surface = nullptr;
        const char* id = nullptr;
        u64 frames = 0;
    };

    const char* setting(const char* name, const char* fallback);
}
