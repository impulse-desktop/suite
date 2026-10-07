#include "test.h"

#include <stdio.h>

namespace plt::test {
    // The window with the keyboard hears of every selection: the one the
    // compositor announces on the keyboard's enter, and each new offer
    // after that.
    bool selectionEvent(int fd) {
        EventSink sink;
        Client client(fd, 800, 1, &sink);
        command(fd, Command::KeyboardEnter);
        pump(*client.platform);
        const u32 onEnter = sink.selectionCount;
        if (command(fd, Command::OfferSelection).count != 1) {
            fprintf(stderr, "selection event: data device was not ready\n");
            return false;
        }
        pump(*client.platform);
        if (sink.selectionCount != onEnter + 1) {
            fprintf(
                stderr,
                "selection event: %u events after the offer, expected %u\n",
                sink.selectionCount,
                onEnter + 1
            );
            return false;
        }
        return true;
    }
}
