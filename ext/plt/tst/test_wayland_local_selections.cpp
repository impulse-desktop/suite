#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool localSelections(int fd) {
        Client client(fd);
        command(fd, Command::PointerEnter);
        pump(*client.platform);

        writeClipboard(*client.window->primary(), stl::StringView(u8"primary content"));
        writeClipboard(*client.window->secondary(), stl::StringView(u8"clipboard content"));
        pump(*client.platform);
        if (command(fd, Command::QueryPrimarySelection).count != 1
            || command(fd, Command::QuerySelection).count != 1) {
            fprintf(stderr, "local selections: ownership was not published\n");
            return false;
        }

        // Reading a selection this client owns serves a snapshot and
        // completes without blocking the fiber.
        StreamRead primary;
        StreamRead clipboard;
        readOnFiber(*client.platform, *client.window->primary(), primary);
        readOnFiber(*client.platform, *client.window->secondary(), clipboard);
        if (!primary.complete
            || stl::StringView(primary.content) != stl::StringView(u8"primary content")
            || !clipboard.complete
            || stl::StringView(clipboard.content)
                != stl::StringView(u8"clipboard content")) {
            fprintf(stderr, "local selections: contents mismatch\n");
            return false;
        }

        // An abandoned local stream leaves the selection intact.
        StreamRead aborted;
        abortOnFiber(*client.platform, *client.window->primary(), aborted);
        StreamRead again;
        readOnFiber(*client.platform, *client.window->primary(), again);
        if (!aborted.complete || !again.complete
            || stl::StringView(again.content) != stl::StringView(u8"primary content")) {
            fprintf(stderr, "local selections: abandoned stream disturbed the selection\n");
            return false;
        }
        return true;
    }
}
