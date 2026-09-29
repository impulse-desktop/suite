#include "test.h"

#include <stdio.h>

namespace plt::test {
    namespace {
        struct RetryingSink final: WindowEvents, FrameCallback {
            void close() override {
            }

            bool frame(const WindowInfo&) override {
                ++frameCount;
                if (window != nullptr) {
                    window->requestFrame();
                }
                return false;
            }

            Window* window = nullptr;
            u32 frameCount = 0;
        };
    }

    bool frameRetry(int fd) {
        RetryingSink sink;
        Client client(fd, 800, 1, &sink, nullptr, true, &sink);
        sink.window = client.window;
        sink.frameCount = 0;
        client.window->requestFrame();
        pump(*client.platform);
        // pump() runs the loop for 1ms. A renderer that keeps failing and
        // re-requesting must be retried with a backoff, not spun through
        // the poller at timeout(0) — hundreds of attempts within the pump.
        if (sink.frameCount < 2 || sink.frameCount > 8) {
            fprintf(stderr, "frame retry: %u frame attempts in one pump\n", sink.frameCount);
            return false;
        }
        return true;
    }
}
