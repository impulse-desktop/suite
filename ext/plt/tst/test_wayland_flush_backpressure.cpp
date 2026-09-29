#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool flushBackpressure(int fd) {
        Platform* platform = nullptr;
        StopOnClose events(platform);
        Client client(fd, 800, 1, &events);
        platform = client.platform;
        stl::Buffer title = repeated(4000, 't');
        for (unsigned index = 0; index != 2048; ++index) {
            static_cast<u8*>(title.mutData())[0] =
                static_cast<u8>('a' + index % 26);
            client.window->requestTitle(stl::StringView(title));
        }
        command(fd, Command::AwaitTitles);

        client.platform->run();
        if (!events.closed) {
            fprintf(stderr, "flush backpressure: close was not delivered\n");
            return false;
        }
        return true;
    }
}
