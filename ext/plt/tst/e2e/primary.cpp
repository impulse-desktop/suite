#include "app.h"

#include <std/ios/input.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Selection final: public App {
        void paint(Canvas& canvas) override;
        void pointerButton(const PointerButtonInput& input) override;
        Buffer content;
        u32 color = 0x204060;
    };
}

void Selection::paint(Canvas& c) {
    c.clear(color);
    c.text(20, 40, StringView(u8"Left: select / Middle: paste"));
    c.text(20, 90, StringView(content));
}

void Selection::pointerButton(const PointerButtonInput& input) {
    if (!input.pressed) {
        return;
    }
    if (input.button == PointerButton::Primary) {
        Output* const out = window->primary()->write();
        const StringView value(setting("PLT_SELECTION_TEXT", "selected by plt"));
        out->write(value.data(), value.length());
        out->finish();
        delete out;
        color = 0xe0b040;
        printf("SELECTED\n");
    } else if (input.button == PointerButton::Middle) {
        Input* const in = window->primary()->read();
        content.reset();
        in->readAll(content);
        delete in;
        color = 0x40a060;
        printf("PRIMARY %s\n", content.cStr());
    }
    window->requestFrame();
}

int main() {
    Selection app;
    app.open("plt-primary");
    app.run();
}
