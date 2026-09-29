#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool invalidKeymap(int fd) {
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        if (command(fd, Command::InvalidKeymap).count != 1) {
            fprintf(stderr, "invalid keymap: keyboard was not ready\n");
            return false;
        }
        pump(*client.platform);
        command(fd, Command::KeyboardEnter);
        command(fd, Command::KeyboardPress);
        pump(*client.platform);
        command(fd, Command::KeyboardRelease);
        pump(*client.platform);

        if (input.pressCount != 1 || input.releaseCount != 1
            || input.textCount == 0
            || input.pressedKey.layoutCodepoint != 'a') {
            fprintf(stderr, "invalid keymap: valid state was not preserved\n");
            return false;
        }
        return true;
    }
}
