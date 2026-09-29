#include "app.h"

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Scroller final: public App {
        void paint(Canvas& canvas) override;
        void scroll(const ScrollInput& input) override;
        double position = 0;
    };
}

void Scroller::paint(Canvas& c) {
    c.clear(position == 0 ? 0x204060 : 0x40a060);
    c.text(20, 40, StringView(u8"Scrollable document"));
    char label[80];
    snprintf(label, sizeof(label), "Scroll offset: %.1f", position);
    c.text(20, 90, StringView(label));
}

void Scroller::scroll(const ScrollInput& input) {
    position += input.y;
    printf("SCROLL %.3f\n", position);
    window->requestFrame();
}

int main() {
    Scroller app;
    app.open("plt-scroller");
    app.run();
}
