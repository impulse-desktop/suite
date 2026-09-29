#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool keyboardEnterKeys(int fd) {
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        command(fd, Command::KeyboardEnterWithKeys);
        pump(*client.platform);
        if (input.focusCount != 1 || input.pressCount != 0 || input.textCount != 0) {
            fprintf(stderr, "keyboard enter pressed keys: enter keys were delivered as input\n");
            return false;
        }

        command(fd, Command::KeyboardRelease);
        pump(*client.platform);
        if (input.releaseCount != 0) {
            fprintf(stderr, "keyboard enter pressed keys: orphan release was delivered\n");
            return false;
        }

        command(fd, Command::KeyboardPress);
        pump(*client.platform);
        if (input.pressCount != 1 || input.textCount == 0) {
            fprintf(stderr, "keyboard enter pressed keys: fresh press was not delivered\n");
            return false;
        }

        command(fd, Command::KeyboardRelease);
        pump(*client.platform);
        if (input.releaseCount != 1) {
            fprintf(stderr, "keyboard enter pressed keys: fresh release was not delivered\n");
            return false;
        }
        return true;
    }
}
