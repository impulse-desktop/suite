#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool nonblockingShow(int fd) {
        if (command(fd, Command::DeferInitialConfigure).count != 1) {
            fprintf(stderr, "nonblocking show: could not defer configure\n");
            return false;
        }

        command(fd, Command::DeferInitialScale);
        EventSink events;
        Client client(fd, 800, 1, &events, nullptr, false, &events);
        pump(*client.platform);
        if (events.frameCount != 0) {
            fprintf(stderr, "nonblocking show: configure was not deferred\n");
            return false;
        }
        client.window->requestResize(900, 700);
        command(fd, Command::PreferredScale);
        if (command(fd, Command::ReleaseInitialConfigure).count != 1) {
            fprintf(stderr, "nonblocking show: window was not committed\n");
            return false;
        }
        for (u32 attempt = 0; attempt != 10 && events.frameCount == 0; ++attempt) {
            pump(*client.platform);
        }
        if (events.frameCount == 0) {
            fprintf(stderr, "nonblocking show: configure was not delivered\n");
            return false;
        }
        if (events.lastInfo.width != 900 || events.lastInfo.height != 700 || events.lastInfo.contentScale != 1.25f) {
            fprintf(stderr, "nonblocking show: resize before the initial scale was not preserved\n");
            return false;
        }
        return true;
    }
}
