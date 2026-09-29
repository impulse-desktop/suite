#include "app.h"

#include <std/lib/buffer.h>

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Editor final: public App {
        void paint(Canvas& canvas) override;
        void text(const TextInput& input) override;
        void key(const KeyInput& input) override;
        Buffer content;
    };
}

void Editor::paint(Canvas& c) {
    c.clear(0x204060);
    c.text(20, 40, StringView(u8"Notes"));
    c.text(20, 90, StringView(content));
    c.rectangle(20, c.height - 50, content.length() * 12, 24, 0x40a060);
}

void Editor::text(const TextInput& input) {
    if (input.codepoint >= 32 && input.codepoint < 127) {
        const u8 ch = (u8)input.codepoint;
        content.append(&ch, 1);
        printf("TEXT %s\n", content.cStr());
        window->requestFrame();
    }
}

void Editor::key(const KeyInput& input) {
    App::key(input);
    if (input.key == InputKey::Backspace && input.action != InputAction::Release && !content.empty()) {
        content.seekNegative(1);
        printf("TEXT %s\n", content.cStr());
        window->requestFrame();
    }
}

int main() {
    Editor app;
    app.open("plt-editor");
    app.run();
}
