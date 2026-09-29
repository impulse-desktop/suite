#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool asynchronousRead(int fd) {
        Client client(fd);
        if (command(fd, Command::OfferSelection).count != 1) {
            fprintf(stderr, "async read: data device was not ready\n");
            return false;
        }
        pump(*client.platform);

        StreamRead read;
        readOnFiber(*client.platform, *client.window->secondary(), read);
        if (read.complete) {
            fprintf(stderr, "async read: remote stream completed before any data arrived\n");
            return false;
        }
        const Reply released = command(fd, Command::ReleaseRead);
        if (released.count != 1) {
            fprintf(
                stderr,
                "async read: the reading fiber blocked, but no transfer fd was available\n"
            );
            return false;
        }
        for (unsigned attempt = 0; attempt != 10 && !read.complete; ++attempt) {
            pump(*client.platform);
        }
        if (!read.complete
            || stl::StringView(read.content)
                != stl::StringView(u8"hermetic Wayland clipboard")) {
            fprintf(
                stderr,
                "async read: complete=%d bytes=%zu\n",
                read.complete,
                read.content.length()
            );
            return false;
        }
        return true;
    }
}
