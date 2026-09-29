#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool minimumAfterScale(int fd) {
        Client client(fd, 800, 500);
        const Reply before = command(fd, Command::QueryMinimum);
        command(fd, Command::PreferredScale);
        Reply after;
        for (unsigned attempt = 0; attempt != 100; ++attempt) {
            pump(*client.platform);
            after = command(fd, Command::QueryMinimum);
            if (after.count > before.count) {
                break;
            }
        }
        if (before.first != 500 || after.count <= before.count
            || after.first != 400) {
            fprintf(
                stderr,
                "minimum after scale: before=%dx%d count=%u, after=%dx%d count=%u; expected second 400x400\n",
                before.first,
                before.second,
                before.count,
                after.first,
                after.second,
                after.count
            );
            return false;
        }
        return true;
    }
}
