#include "app.h"

#include <std/ios/input.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct ClipboardEditor final: public App {
        void paint(Canvas& canvas) override;
        void key(const KeyInput& input) override;
        Buffer content;
        u32 color = 0x204060;
    };
}

void ClipboardEditor::paint(Canvas& c) {
    c.clear(color);
    c.text(20, 40, StringView(u8"Ctrl+C copy / Ctrl+V paste"));
    c.text(20, 90, StringView(content));
}

void ClipboardEditor::key(const KeyInput& input) {
    App::key(input);
    if (input.action != InputAction::Press || !(input.modifiers & InputControl)) {
        return;
    }
    if (input.baseCodepoint == 'c') {
        Output* const out = window->secondary()->write();
        const StringView value(setting("PLT_COPY_TEXT", "clipboard from plt"));
        out->write(value.data(), value.length());
        out->finish();
        delete out;
        color = 0xe0b040;
        printf("COPIED\n");
    } else if (input.baseCodepoint == 'v') {
        Input* const in = window->secondary()->read();
        content.reset();
        in->readAll(content);
        delete in;
        color = 0x40a060;
        printf("PASTED %s\n", content.cStr());
    }
    window->requestFrame();
}

int main() {
    ClipboardEditor app;
    app.open("plt-clipboard");
    app.run();
}
