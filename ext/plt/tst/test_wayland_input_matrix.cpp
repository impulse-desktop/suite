#include "test.h"

#include <std/sys/fd.h>
#include <std/sys/mem_fd.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

namespace plt::test {
    bool inputMatrix(int fd) {
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        command(fd, Command::KeyboardEnter);
        command(fd, Command::KeyboardMatrix);
        command(fd, Command::PointerButtons);
        pump(*client.platform);

        static constexpr InputKey expected[]{
            InputKey::F12,
            InputKey::KeypadHome,
            InputKey::VolumeUp,
            InputKey::LeftShift,
            InputKey::Up,
            InputKey::Unknown,
        };
        if (input.pressedKeyCount != sizeof(expected) / sizeof(expected[0])) {
            fprintf(stderr, "input matrix: got %u key presses, expected %zu\n", input.pressedKeyCount, sizeof(expected) / sizeof(expected[0]));
            return false;
        }
        for (size_t index = 0; index != sizeof(expected) / sizeof(expected[0]); ++index) {
            if (input.pressedKeys[index] != expected[index]) {
                fprintf(stderr, "input matrix: key %zu mapped to %u instead of %u\n", index, static_cast<u32>(input.pressedKeys[index]), static_cast<u32>(expected[index]));
                return false;
            }
        }
        if (input.buttonPressCount != 7 || input.buttonReleaseCount != 7 || input.buttonMask != 0xfe) {
            fprintf(stderr, "input matrix: pointer buttons press=%u release=%u mask=%x, expected 7/7/fe\n", input.buttonPressCount, input.buttonReleaseCount, input.buttonMask);
            return false;
        }
        return true;
    }

    bool composeInput(int fd) {
        static constexpr char composeTable[] =
            "<Multi_key> <apostrophe> <j> : \"j\xcc\x81\"\n";
        stl::ScopedFD compose{stl::memFD("plt-compose")};
        compose.write(composeTable, sizeof(composeTable) - 1);
        lseek(compose.get(), 0, SEEK_SET);
        char composePath[32];
        snprintf(composePath, sizeof(composePath), "/proc/self/fd/%d", compose.get());
        setenv("XCOMPOSEFILE", composePath, 1);
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        compose.close();
        command(fd, Command::KeyboardEnter);
        command(fd, Command::KeyboardCompose);
        pump(*client.platform);
        if (input.textCodepointCount != 2 || input.textCodepoints[0] != 'j' || input.textCodepoints[1] != 0x301) {
            fprintf(
                stderr,
                "compose input: got %u codepoints (%x, %x), keys (%u, %u, %u), expected j + combining acute\n",
                input.textCodepointCount,
                input.textCodepoints[0],
                input.textCodepoints[1],
                static_cast<u32>(input.pressedKeys[0]),
                static_cast<u32>(input.pressedKeys[1]),
                static_cast<u32>(input.pressedKeys[2])
            );
            return false;
        }
        return true;
    }
}
