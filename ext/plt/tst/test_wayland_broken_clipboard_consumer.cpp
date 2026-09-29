#include "test.h"

namespace plt::test {
    bool brokenClipboardConsumer(int fd) {
        Client client(fd);
        writeClipboard(*client.window->secondary(), stl::StringView(u8"broken consumer"));
        command(fd, Command::PointerEnter);
        pump(*client.platform);
        if (command(fd, Command::RequestBrokenSourceData).count != 1) {
            return false;
        }

        pump(*client.platform);
        command(fd, Command::QuerySelection);
        return true;
    }
}
