#include "test.h"

#include "cursor-shape-v1-server-protocol.h"

#include <stdio.h>

namespace plt::test {
    bool pointerInput(int fd) {
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        command(fd, Command::PointerEnter);
        pump(*client.platform);
        client.window->requestPointerIcon(plt::PointerIcon::Pointer);
        pump(*client.platform);
        const Reply cursor = command(fd, Command::QueryCursor);
        if (cursor.count < 2 || cursor.first != WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER) {
            fprintf(stderr, "pointer input: cursor shape was not updated\n");
            return false;
        }
        if (cursor.second != 1) {
            fprintf(stderr, "pointer input: set_shape used a non-enter serial\n");
            return false;
        }

        command(fd, Command::PointerSequence);
        pump(*client.platform);
        client.window->requestPointerIcon(plt::PointerIcon::Text);
        if (input.motionCount != 3 || input.lastMotion.pixelX != 30 || input.lastMotion.pixelY != 40 || input.buttonPressCount != 1 || input.buttonReleaseCount != 1 || input.lastButton.button != plt::PointerButton::Primary || input.lastButton.pressed || input.scrollCount != 1 || input.lastScroll.x != -2 || input.lastScroll.y != 3 || input.lastScroll.pixelX != 30 || input.lastScroll.pixelY != 40 || input.pointerEnterCount != 2 || input.pointerLeaveCount != 1 || input.flushCount != 2) {
            fprintf(stderr, "pointer input: event translation mismatch\n");
            return false;
        }
        return true;
    }
}
