#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool cancelReadyClipboardRead(int fd) {
        Client client(fd);
        writeClipboard(*client.window->secondary(), stl::StringView(u8"local clipboard"));
        command(fd, Command::PointerEnter);
        pump(*client.platform);

        // Two concurrent local streams serve independent snapshots; one
        // aborting mid-way does not disturb the other.
        StreamRead aborted;
        StreamRead full;
        abortOnFiber(*client.platform, *client.window->secondary(), aborted);
        readOnFiber(*client.platform, *client.window->secondary(), full);
        if (!aborted.complete || !full.complete
            || stl::StringView(full.content) != stl::StringView(u8"local clipboard")) {
            fprintf(
                stderr,
                "cancel ready read: concurrent local streams interfered\n"
            );
            return false;
        }
        return true;
    }
}
