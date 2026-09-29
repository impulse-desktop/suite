#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool frameApi(int fd) {
        EventSink events;
        Client client(fd, 800, 1, &events, nullptr, true, &events);
        pump(*client.platform);
        Reply frames = command(fd, Command::QueryFrames);
        if (events.frameCount != 1 || frames.count != 0 || frames.first != 0) {
            fprintf(stderr, "frame API: rejected frame armed presentation callback\n");
            return false;
        }

        events.frameCount = 0;
        events.submitFrames = true;
        client.window->requestFrame();
        pump(*client.platform);
        frames = command(fd, Command::QueryFrames);
        if (events.frameCount != 1 || frames.count != 1 || frames.first != 1) {
            fprintf(stderr, "frame API: initial frame was not submitted\n");
            return false;
        }
        command(fd, Command::CompleteFrames);
        pump(*client.platform);

        events.frameCount = 0;
        client.window->requestFrame();
        client.window->requestFrame();
        pump(*client.platform);
        if (events.frameCount != 1) {
            fprintf(stderr, "frame API: invalidations were not coalesced\n");
            return false;
        }
        frames = command(fd, Command::QueryFrames);
        if (frames.count != 2 || frames.first != 1) {
            fprintf(stderr, "frame API: presentation callback was not armed\n");
            return false;
        }

        client.window->requestFrame();
        client.window->requestFrame();
        pump(*client.platform);
        if (events.frameCount != 1) {
            fprintf(stderr, "frame API: frame escaped presentation pacing\n");
            return false;
        }
        command(fd, Command::CompleteFrames);
        pump(*client.platform);
        if (events.frameCount != 2) {
            fprintf(stderr, "frame API: pending invalidation was not delivered\n");
            return false;
        }
        frames = command(fd, Command::QueryFrames);
        if (frames.count != 3 || frames.first != 1) {
            fprintf(stderr, "frame API: next presentation callback was not armed\n");
            return false;
        }
        return true;
    }
}
