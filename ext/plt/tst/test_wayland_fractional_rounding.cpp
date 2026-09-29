#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool fractionalRounding(int fd) {
        EventSink events;
        Client client(fd, 802, 1, nullptr, nullptr, true, &events);
        command(fd, Command::PreferredScale);
        pump(*client.platform);
        const u32 width = events.lastInfo.width;
        if (width != 1003) {
            fprintf(
                stderr,
                "fractional rounding: width=%u, expected 1003\n",
                width
            );
            return false;
        }
        return true;
    }
}
