#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool integerScaleFallback(int fd) {
        if (command(fd, Command::IntegerScaleOnly).count != 1) {
            fprintf(stderr, "integer scale fallback: could not remove fractional-scale\n");
            return false;
        }
        EventSink events;
        Client client(fd, 800, 1, nullptr, nullptr, true, &events);
        if (command(fd, Command::SurfaceEnter).count != 1) {
            fprintf(stderr, "integer scale fallback: surface/output was unavailable\n");
            return false;
        }
        pump(*client.platform);
        if (events.lastInfo.contentScale != 2.0f || events.lastInfo.width != 1600) {
            fprintf(
                stderr,
                "integer scale fallback: scale=%g width=%u, expected 2/1600\n",
                static_cast<double>(events.lastInfo.contentScale),
                events.lastInfo.width
            );
            return false;
        }
        // The compositor's own integer preference takes the same path
        // when no fractional-scale object exists.
        if (command(fd, Command::SurfacePreferredScale).count == 1) {
            pump(*client.platform);
            if (events.lastInfo.contentScale != 2.0f) {
                fprintf(stderr, "integer scale fallback: preferred_buffer_scale was dropped\n");
                return false;
            }
        }
        return true;
    }

    bool legacyGlobals(int fd) {
        if (command(fd, Command::LegacyGlobals).count != 1) {
            fprintf(stderr, "legacy protocol globals: could not downgrade globals\n");
            return false;
        }
        InputRecorder input;
        {
            Client client(fd, 800, 1, nullptr, &input);
            command(fd, Command::KeyboardEnter);
            command(fd, Command::PointerEnter);
            pump(*client.platform);
        }
        return input.focusCount == 1 && input.pointerEnterCount == 1;
    }
}
