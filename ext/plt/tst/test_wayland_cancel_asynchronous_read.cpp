#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool cancelAsynchronousRead(int fd) {
        Client client(fd);
        command(fd, Command::OfferSelection);
        pump(*client.platform);

        // The consumer abandons the transfer after the first chunk by
        // deleting the stream; the source sees its pipe close.
        StreamRead read;
        abortOnFiber(*client.platform, *client.window->secondary(), read);
        if (command(fd, Command::ReleaseRead).count != 1) {
            fprintf(stderr, "cancel read: no transfer fd was available\n");
            return false;
        }
        for (unsigned attempt = 0; attempt != 10 && !read.complete; ++attempt) {
            pump(*client.platform);
        }
        if (!read.complete || read.chunks != 1) {
            fprintf(stderr, "cancel read: the aborting fiber did not finish\n");
            return false;
        }
        // The clipboard stays usable after an abandoned transfer.
        if (command(fd, Command::OfferSelection).count != 1) {
            fprintf(stderr, "cancel read: replacement offer was refused\n");
            return false;
        }
        return true;
    }
}
