#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool plainMimeSelection(int fd) {
        Client client(fd);
        if (command(fd, Command::OfferPlainSelection).count != 1) {
            fprintf(stderr, "plain MIME: data device was not ready\n");
            return false;
        }
        pump(*client.platform);

        StreamRead read;
        readOnFiber(*client.platform, *client.window->secondary(), read);
        if (command(fd, Command::ReleaseRead).count != 1) {
            fprintf(stderr, "plain MIME: fallback was not requested\n");
            return false;
        }
        pump(*client.platform);
        if (!read.complete
            || stl::StringView(read.content)
                != stl::StringView(u8"hermetic Wayland clipboard")) {
            fprintf(stderr, "plain MIME: fallback read failed\n");
            return false;
        }
        return true;
    }
}
