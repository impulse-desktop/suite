#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool deferredClipboard(int fd) {
        Client client(fd);
        writeClipboard(*client.window->secondary(), stl::StringView(u8"clipboard"));
        const Reply before = command(fd, Command::PointerEnter);
        pump(*client.platform);
        const Reply after = command(fd, Command::QuerySelection);
        if (before.count != 0 || after.count != 1) {
            fprintf(
                stderr,
                "deferred clipboard: selection count before=%u after=%u, expected 0/1\n",
                before.count,
                after.count
            );
            return false;
        }
        return true;
    }
}
