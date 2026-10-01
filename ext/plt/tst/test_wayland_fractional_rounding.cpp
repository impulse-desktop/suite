#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool initialFractionalSize(int fd) {
        command(fd, Command::InitialFractionalScale);
        EventSink events;
        Client client(fd, 1000, 1, nullptr, nullptr, true, &events);
        const Reply geometry = command(fd, Command::QueryWindowGeometry);
        if (events.lastInfo.width != 1000 || events.lastInfo.height != 600 || events.lastInfo.contentScale != 1.25f || geometry.first != 800 || geometry.second != 480) {
            fprintf(stderr, "initial fractional size: pixels=%ux%u logical=%dx%d scale=%g\n", events.lastInfo.width, events.lastInfo.height, geometry.first, geometry.second, (double)events.lastInfo.contentScale);
            return false;
        }
        return true;
    }

    bool lateInitialScale(int fd) {
        command(fd, Command::DeferInitialScale);
        EventSink events;
        Client client(fd, 1000, 1, nullptr, nullptr, true, &events);
        client.window->requestResize(900, 700);
        command(fd, Command::PreferredScale);
        pump(*client.platform);
        const Reply geometry = command(fd, Command::QueryWindowGeometry);
        if (events.lastInfo.width != 900 || events.lastInfo.height != 700 || events.lastInfo.contentScale != 1.25f || geometry.first != 720 || geometry.second != 560) {
            fprintf(stderr, "late initial scale: pixels=%ux%u logical=%dx%d scale=%g\n", events.lastInfo.width, events.lastInfo.height, geometry.first, geometry.second, (double)events.lastInfo.contentScale);
            return false;
        }
        return true;
    }

    bool fractionalRounding(int fd) {
        EventSink events;
        Client client(fd, 802, 1, nullptr, nullptr, true, &events);
        command(fd, Command::PreferredScale);
        pump(*client.platform);
        const u32 width = events.lastInfo.width;
        if (width != 1003) {
            fprintf(stderr, "fractional rounding: width=%u, expected 1003\n", width);
            return false;
        }
        return true;
    }
}
