#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool missingSelections(int fd) {
        Client client(fd);
        StreamRead primary;
        StreamRead clipboard;
        readOnFiber(*client.platform, *client.window->primary(), primary);
        readOnFiber(*client.platform, *client.window->secondary(), clipboard);
        // An absent selection is an immediately empty stream: the fibers
        // finish without ever blocking.
        if (!primary.complete || !primary.content.empty()
            || !clipboard.complete || !clipboard.content.empty()) {
            fprintf(stderr, "missing selections: streams were not empty\n");
            return false;
        }
        return true;
    }
}
