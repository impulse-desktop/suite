#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool unsupportedMimeSelection(int fd) {
        Client client(fd);
        if (command(fd, Command::OfferUnsupportedSelection).count != 1) {
            fprintf(stderr, "unsupported MIME: data device was not ready\n");
            return false;
        }
        pump(*client.platform);

        // No offered mime is usable, so the stream is immediately empty and
        // no transfer ever starts.
        StreamRead read;
        readOnFiber(*client.platform, *client.window->secondary(), read);
        if (!read.complete || !read.content.empty()) {
            fprintf(stderr, "unsupported MIME: stream was not empty\n");
            return false;
        }
        return true;
    }
}
