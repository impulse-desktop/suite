#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool sourceCancellation(int fd) {
        Client client(fd);
        command(fd, Command::PointerEnter);
        pump(*client.platform);

        writeClipboard(*client.window->secondary(), stl::StringView(u8"clipboard one"));
        writeClipboard(*client.window->primary(), stl::StringView(u8"primary one"));
        pump(*client.platform);
        if (command(fd, Command::QuerySelection).count != 1
            || command(fd, Command::QueryPrimarySelection).count != 1) {
            fprintf(stderr, "source cancellation: initial sources missing\n");
            return false;
        }
        if (command(fd, Command::CancelSources).count != 3) {
            fprintf(stderr, "source cancellation: sources were not ready\n");
            return false;
        }
        pump(*client.platform);

        writeClipboard(*client.window->secondary(), stl::StringView(u8"clipboard two"));
        writeClipboard(*client.window->primary(), stl::StringView(u8"primary two"));
        pump(*client.platform);
        if (command(fd, Command::QuerySelection).count != 2
            || command(fd, Command::QueryPrimarySelection).count != 2) {
            fprintf(stderr, "source cancellation: sources were not replaced\n");
            return false;
        }
        return true;
    }
}
