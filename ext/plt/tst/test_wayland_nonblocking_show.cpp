#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool nonblockingShow(int fd) {
        if (command(fd, Command::DeferInitialConfigure).count != 1) {
            fprintf(stderr, "nonblocking show: could not defer configure\n");
            return false;
        }

        EventSink events;
        Client client(fd, 800, 1, &events, nullptr, false, &events);
        pump(*client.platform);
        if (events.frameCount != 0) {
            fprintf(stderr, "nonblocking show: configure was not deferred\n");
            return false;
        }
        if (command(fd, Command::ReleaseInitialConfigure).count != 1) {
            fprintf(stderr, "nonblocking show: window was not committed\n");
            return false;
        }
        for (u32 attempt = 0;
             attempt != 10 && events.frameCount == 0;
             ++attempt) {
            pump(*client.platform);
        }
        if (events.frameCount == 0) {
            fprintf(stderr, "nonblocking show: configure was not delivered\n");
            return false;
        }
        return true;
    }
}
