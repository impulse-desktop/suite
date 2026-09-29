#include "app.h"
#include "poller.h"

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Animation final: public App, public TimerCallback {
        void paint(Canvas& canvas) override;
        void ready() override;
        int tick = 0;
    };
}

void Animation::paint(Canvas& c) {
    c.clear(tick == 30 ? 0x40a060 : 0x204060);
    c.text(20, 40, StringView(u8"Timer-driven animation"));
    c.rectangle(20, 100, tick * 10, 40, 0xe0b040);
}

void Animation::ready() {
    ++tick;
    window->requestFrame();
    printf("TICK %d\n", tick);
    if (tick < 30) {
        platform->poller()->timeout(20'000, *this);
    }
}

int main() {
    Animation app;
    app.open("plt-animation");
    app.platform->poller()->timeout(100'000, app);
    app.run();
}
