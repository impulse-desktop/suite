#include "app.h"
#include "fiber.h"

#include <std/thr/runable.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Dashboard final: public App, public Runable {
        void paint(Canvas& canvas) override;
        void run() override;
        int value = 0;
    };
}

void Dashboard::paint(Canvas& c) {
    c.clear(0x204060);
    c.text(20, 40, StringView(u8"Live pipe dashboard"));
    c.rectangle(20, 100, value * 3, 40, 0x40a060);
}

void Dashboard::run() {
    const int fd = ::open(setting("PLT_DATA_PIPE", ""), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror("data pipe");
        exit(1);
    }
    for (;;) {
        platform->scheduler()->awaitReadable(fd, 0);
        char bytes[128];
        const ssize_t count = read(fd, bytes, sizeof(bytes) - 1);
        if (count > 0) {
            bytes[count] = 0;
            value = atoi(bytes);
            if (value < 0 || value > 100) {
                exit(1);
            }
            printf("VALUE %d\n", value);
            window->requestFrame();
        }
    }
}

int main() {
    Dashboard app;
    app.open("plt-dashboard");
    app.platform->scheduler()->create(*app.owner, app, 128 * 1024);
    app.App::run();
}
