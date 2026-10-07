#include "test.h"

#include <stdio.h>

namespace plt::test {
    // The window with the keyboard hears of every selection: not of one
    // set while the keyboard was elsewhere, but of that one as the
    // keyboard enters, which a compositor announces with the enter, and
    // of each change after that.
    bool selectionEvent(int fd) {
        EventSink sink;
        Client client(fd, 800, 1, &sink);
        if (command(fd, Command::OfferSelection).count != 1) {
            fprintf(stderr, "selection event: data device was not ready\n");
            return false;
        }
        pump(*client.platform);
        if (sink.selectionCount != 0) {
            fprintf(stderr, "selection event: %u events without the keyboard, expected 0\n", sink.selectionCount);
            return false;
        }
        command(fd, Command::KeyboardEnter);
        pump(*client.platform);
        if (sink.selectionCount != 1) {
            fprintf(stderr, "selection event: %u events after the keyboard's enter, expected 1\n", sink.selectionCount);
            return false;
        }
        command(fd, Command::OfferSelection);
        pump(*client.platform);
        if (sink.selectionCount != 2) {
            fprintf(stderr, "selection event: %u events after a change, expected 2\n", sink.selectionCount);
            return false;
        }
        return true;
    }
}
