#include "app.h"

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Gallery final: public App {
        void paint(Canvas& canvas) override;
    };
}

void Gallery::paint(Canvas& c) {
    c.clear(0x204060);
    c.rectangle(c.width / 2, 0, c.width / 2, c.height / 2, 0xc04040);
    c.rectangle(0, c.height / 2, c.width / 2, c.height / 2, 0x40a060);
    c.rectangle(c.width / 2, c.height / 2, c.width / 2, c.height / 2, 0xe0b040);
    c.text(20, 40, StringView(u8"plt image viewer"));
}

int main() {
    Gallery app;
    app.open("plt-gallery");
    app.run();
}
