#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool scrollValue120(int fd) {
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        command(fd, Command::PointerValue120);
        pump(*client.platform);
        if (input.scrollCount != 1
            || input.lastScroll.x != 0.0
            || input.lastScroll.y != 2.0) {
            fprintf(
                stderr,
                "value120 scroll: expected 2 wheel lines, got %u events x=%f y=%f\n",
                input.scrollCount,
                input.lastScroll.x,
                input.lastScroll.y
            );
            return false;
        }

        // The pre-value120 spelling of the same wheel: axis_discrete
        // steps scale by 120 into the same event.
        command(fd, Command::PointerLegacyDiscrete);
        pump(*client.platform);
        if (input.scrollCount != 2 || input.lastScroll.y != 2.0) {
            fprintf(
                stderr,
                "value120 scroll: legacy discrete gave %u events y=%f\n",
                input.scrollCount,
                input.lastScroll.y
            );
            return false;
        }
        return true;
    }

    bool scrollFingerPhases(int fd) {
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        command(fd, Command::PointerFingerPhases);
        pump(*client.platform);
        if (input.scrollCount != 3
            || input.scrolls[0].phase != ScrollPhase::Begin
            || input.scrolls[1].phase != ScrollPhase::Update
            || input.scrolls[2].phase != ScrollPhase::End
            || !input.scrolls[0].precise
            || !input.scrolls[1].precise
            || !input.scrolls[2].precise
            || input.scrolls[0].time != 1.0
            || input.scrolls[1].time != 1.01
            || input.scrolls[2].time != 1.02) {
            fprintf(
                stderr,
                "finger scroll: expected precise begin/update/end, got %u events phases=%u/%u/%u\n",
                input.scrollCount,
                static_cast<unsigned>(input.scrolls[0].phase),
                static_cast<unsigned>(input.scrolls[1].phase),
                static_cast<unsigned>(input.scrolls[2].phase)
            );
            return false;
        }
        return true;
    }
}
