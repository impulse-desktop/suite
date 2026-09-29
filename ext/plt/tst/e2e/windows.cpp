#include "app.h"

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Document final: public App {
        Document(Platform* shared, u32 color);
        void paint(Canvas& canvas) override;
        u32 color;
    };
}

Document::Document(Platform* shared, u32 value)
    : App(shared)
    , color(value)
{
}

void Document::paint(Canvas& c)
{
    c.clear(color);
    c.text(20, 40, StringView(id));
    c.rectangle(20, c.height - 50, 100, 24, window->info().focused ? 0xe0b040 : 0x808080);
}

int main() {
    Document first(nullptr, 0xc04040);
    first.open("plt-first", 300, 240);
    Document second(first.platform, 0x40a060);
    second.open("plt-second", 300, 240);
    first.run();
}
