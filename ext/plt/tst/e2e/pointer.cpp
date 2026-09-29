#include "app.h"

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Paint final: public App {
        void paint(Canvas& canvas) override;
        void pointerButton(const PointerButtonInput& input) override;
        void pointerMotion(const PointerMotionInput& input) override;
        i32 x = 80;
        i32 y = 100;
        int clicks = 0;
        bool pressed = false;
    };
}

void Paint::paint(Canvas& c) {
    c.clear(0x204060);
    c.text(20, 40, StringView(u8"Drag to paint"));
    c.rectangle(x - 16, y - 16, 32, 32, 0xe0b040);
    c.rectangle(20, c.height - 50, 100, 24, clicks ? 0x40a060 : 0xc04040);
}

void Paint::pointerButton(const PointerButtonInput& input) {
    pressed = input.pressed;
    x = input.pixelX;
    y = input.pixelY;
    if (pressed) {
        ++clicks;
    }
    printf("POINTER %d %d %d %d\n", x, y, clicks, pressed);
    window->requestPointerIcon(pressed ? PointerIcon::Grabbing : PointerIcon::Crosshair);
    window->requestFrame();
}

void Paint::pointerMotion(const PointerMotionInput& input) {
    if (pressed) {
        x = input.pixelX;
        y = input.pixelY;
        printf("DRAG %d %d\n", x, y);
        window->requestFrame();
    }
}

int main() {
    Paint app;
    app.open("plt-pointer");
    app.window->requestPointerIcon(PointerIcon::Crosshair);
    app.run();
}
