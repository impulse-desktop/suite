#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool asynchronousPrimary(int fd) {
        Client client(fd);
        if (command(fd, Command::OfferPrimarySelection).count != 1) {
            fprintf(stderr, "async primary: primary device was not ready\n");
            return false;
        }
        pump(*client.platform);

        StreamRead read;
        readOnFiber(*client.platform, *client.window->primary(), read);
        if (command(fd, Command::ReleaseRead).count != 1) {
            fprintf(stderr, "async primary: no read transfer fd\n");
            return false;
        }
        for (u32 attempt = 0; attempt != 10 && !read.complete; ++attempt) {
            pump(*client.platform);
        }
        if (!read.complete
            || stl::StringView(read.content)
                != stl::StringView(u8"hermetic Wayland clipboard")) {
            fprintf(stderr, "async primary: read failed\n");
            return false;
        }

        command(fd, Command::PointerEnter);
        pump(*client.platform);
        writeClipboard(*client.window->primary(), stl::StringView(u8"primary source"));
        pump(*client.platform);
        if (command(fd, Command::RequestPrimarySourceData).count != 1) {
            fprintf(stderr, "async primary: source was not published\n");
            return false;
        }
        pump(*client.platform);
        if (command(fd, Command::ReleaseWrite).count != 1) {
            fprintf(stderr, "async primary: source callback blocked\n");
            return false;
        }
        Reply state;
        for (u32 attempt = 0; attempt != 20; ++attempt) {
            pump(*client.platform);
            state = command(fd, Command::QueryWrite);
            if (state.first != 0) {
                break;
            }
        }
        if (state.count != sizeof("primary source") - 1 || state.first == 0) {
            fprintf(stderr, "async primary: source contents mismatch\n");
            return false;
        }
        return true;
    }
}
