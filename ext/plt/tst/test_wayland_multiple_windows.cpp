#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool multipleWindows(int fd) {
        EventSink firstEvents;
        InputRecorder firstInput;
        Client client(fd, 800, 1, &firstEvents, &firstInput, true, &firstEvents);
        const u32 firstFrames = firstEvents.frameCount;

        EventSink secondEvents;
        InputRecorder secondInput;
        Window* const second = client.platform->createWindow(
            *client.owner,
            {
                .appId = stl::StringView(u8"plt.integration.second"),
                .title = stl::StringView(u8"second"),
                .width = 640,
                .height = 480,
                .minimumWidth = 1,
                .minimumHeight = 1,
                .input = &secondInput,
                .events = &secondEvents,
                .frame = &secondEvents,
            }
        );
        second->requestShow();
        for (u32 attempt = 0; attempt != 10
             && secondEvents.frameCount == 0; ++attempt) {
            pump(*client.platform);
        }
        if (secondEvents.frameCount == 0
            || secondEvents.lastInfo.width != 640
            || firstEvents.lastInfo.width != 800
            || firstEvents.frameCount != firstFrames) {
            fprintf(stderr, "multiple windows: initial state leaked\n");
            return false;
        }

        command(fd, Command::ConfigureWindowResize);
        command(fd, Command::PointerEnter);
        pump(*client.platform);
        if (secondEvents.lastInfo.width != 819
            || firstEvents.lastInfo.width != 800
            || secondInput.pointerEnterCount != 1
            || firstInput.pointerEnterCount != 0) {
            fprintf(
                stderr,
                "multiple windows: widths=%u/%u enters=%u/%u\n",
                firstEvents.lastInfo.width,
                secondEvents.lastInfo.width,
                firstInput.pointerEnterCount,
                secondInput.pointerEnterCount
            );
            return false;
        }
        return true;
    }
}
