#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool outputRemoval(int fd) {
        Client client(fd);
        if (client.window->info().screenPixelWidth != 1920) {
            fprintf(stderr, "output removal: initial output mode missing\n");
            return false;
        }

        command(fd, Command::RemoveOutput);
        pump(*client.platform);
        if (client.window->info().screenPixelWidth != 0) {
            fprintf(stderr, "output removal: stale output mode survived removal\n");
            return false;
        }

        command(fd, Command::RestoreOutput);
        pump(*client.platform);
        if (client.window->info().screenPixelWidth != 1920) {
            fprintf(stderr, "output removal: replacement output was not bound\n");
            return false;
        }
        return true;
    }
}
