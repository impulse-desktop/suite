#include "app.h"

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Viewer final: public App {
        void paint(Canvas& canvas) override;
        void key(const KeyInput& input) override;
    };
}

void Viewer::paint(Canvas& c) {
    c.clear(window->info().fullscreen ? 0x40a060 : 0x204060);
    c.text(20, 40, StringView(u8"F: fullscreen / R: resize / T: title"));
    c.rectangle(c.width - 40, c.height - 40, 20, 20, 0xe0b040);
}

void Viewer::key(const KeyInput& input) {
    App::key(input);
    if (input.action != InputAction::Press) {
        return;
    }
    if (input.baseCodepoint == 'f') {
        window->requestFullscreen(!window->info().fullscreen);
    } else if (input.baseCodepoint == 'r') {
        window->requestResize(640, 360);
    } else if (input.baseCodepoint == 't') {
        window->requestTitle(StringView(u8"Updated document"));
    }
}

int main() {
    Viewer app;
    app.open("plt-state");
    app.run();
}
