#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool rejectedSelection(int fd) {
        Client client(fd);
        command(fd, Command::OfferSelection);
        pump(*client.platform);
        // The consumer takes one chunk and walks away; deleting the stream
        // mid-transfer closes the pipe under the source.
        StreamRead read;
        abortOnFiber(*client.platform, *client.window->secondary(), read);
        if (command(fd, Command::ReleaseRead).count != 1) {
            fprintf(stderr, "rejected selection: no transfer fd\n");
            return false;
        }
        for (unsigned attempt = 0; attempt != 10 && !read.complete; ++attempt) {
            pump(*client.platform);
        }
        if (!read.complete || read.chunks != 1 || read.content.empty()) {
            fprintf(stderr, "rejected selection: first chunk was not delivered\n");
            return false;
        }
        return true;
    }
}
