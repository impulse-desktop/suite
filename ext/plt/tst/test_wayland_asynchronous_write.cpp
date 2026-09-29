#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool asynchronousWrite(int fd) {
        static constexpr size_t contentSize = 2 * 1024 * 1024;
        stl::Buffer content = repeated(contentSize, 'w');
        Client client(fd);
        writeClipboard(*client.window->secondary(), stl::StringView(content));
        command(fd, Command::PointerEnter);
        pump(*client.platform);
        if (command(fd, Command::RequestSourceData).count != 1) {
            fprintf(stderr, "async write: selection source was not ready\n");
            return false;
        }

        // The server deliberately does not drain the pipe. A synchronous
        // source callback blocks here after the pipe buffer fills.
        pump(*client.platform);
        if (command(fd, Command::ReleaseWrite).count != 1) {
            fprintf(stderr, "async write: source send did not return\n");
            return false;
        }
        Reply state;
        for (unsigned attempt = 0; attempt != 100; ++attempt) {
            pump(*client.platform);
            state = command(fd, Command::QueryWrite);
            if (state.first != 0) {
                break;
            }
        }
        if (state.count != contentSize || state.first == 0) {
            fprintf(
                stderr,
                "async write: bytes=%u complete=%d, expected %zu/1\n",
                state.count,
                state.first,
                contentSize
            );
            return false;
        }
        return true;
    }
}
