#include "test.h"

#include "fiber.h"

#include <std/ios/input.h>
#include <std/ptr/scoped.h>
#include <std/thr/runable.h>

#include <stdio.h>

namespace plt::test {
    bool fiberClipboard(int fd) {
        Client client(fd);
        Scheduler* const scheduler = client.platform->scheduler();
        command(fd, Command::PointerEnter);
        pump(*client.platform);

        // A remote offer blocks the fiber on the transfer pipe while the
        // loop keeps running; nested calls read the clipboard mid-stack.
        command(fd, Command::OfferSelection);
        pump(*client.platform);
        stl::Buffer remote;
        bool remoteComplete = false;
        auto remoteBody = stl::makeRunable([&] {
            const stl::ScopedPtr<stl::Input> stream{client.window->secondary()->read()};
            for (;;) {
                u8 chunk[4096];
                const size_t count = stream->read(chunk, sizeof(chunk));
                if (count == 0) {
                    break;
                }
                remote.append(chunk, count);
            }
            remoteComplete = true;
        });
        alignas(16) static u8 remoteStack[lightFiberStack];
        scheduler->spawn(remoteBody, remoteStack, sizeof(remoteStack));
        if (remoteComplete) {
            fprintf(stderr, "fiber clipboard: remote read completed before any data arrived\n");
            return false;
        }
        if (command(fd, Command::ReleaseRead).count != 1) {
            fprintf(stderr, "fiber clipboard: no transfer fd was available\n");
            return false;
        }
        pump(*client.platform);
        if (!remoteComplete || stl::StringView(remote) != stl::StringView(u8"hermetic Wayland clipboard")) {
            fprintf(stderr, "fiber clipboard: remote selection was not delivered\n");
            return false;
        }

        // Reading a selection this client owns completes without blocking.
        writeClipboard(*client.window->secondary(), stl::StringView(u8"local fiber clipboard"));
        pump(*client.platform);
        StreamRead local;
        readOnFiber(*client.platform, *client.window->secondary(), local);
        if (!local.complete || stl::StringView(local.content) != stl::StringView(u8"local fiber clipboard")) {
            fprintf(stderr, "fiber clipboard: local selection was not read inline\n");
            return false;
        }
        return true;
    }
}
