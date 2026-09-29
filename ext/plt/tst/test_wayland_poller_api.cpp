#include "test.h"

#include <std/sys/crt.h>
#include <std/thr/poll_fd.h>

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

using namespace stl;

namespace plt::test {
    namespace {
        struct FdCallback final: PollCallback {
            explicit FdCallback(Platform& platform_): platform(platform_) {
                waiter.callback = this;
            }

            void arm(int fd, u32 flags) {
                waiter.fd = {.fd = fd, .flags = flags};
                platform.poller()->arm(waiter);
            }

            void ready(PollFD event_) override {
                event = event_;
                called = true;
                u8 byte;
                readOk = read(event.fd, &byte, 1) == 1;
                if (stopOnReady) {
                    platform.stop();
                }
            }

            Platform& platform;
            PollWaiter waiter;
            PollFD event{};
            bool called = false;
            bool readOk = false;
            bool stopOnReady = true;
        };

        struct DeadlineCallback final: TimerCallback {
            explicit DeadlineCallback(Platform& platform_): platform(platform_) {}

            void ready() override {
                called = true;
                platform.stop();
            }

            Platform& platform;
            bool called = false;
        };

        struct CrossCancelFD final: PollCallback {
            CrossCancelFD(
                Platform& platform_,
                u32& callCount_
            )
                : platform(platform_)
                , callCount(callCount_)
            {
                waiter.callback = this;
            }

            void arm(int fd) {
                waiter.fd = {.fd = fd, .flags = PollFlag::In};
                platform.poller()->arm(waiter);
            }

            void ready(PollFD) override {
                ++callCount;
                platform.poller()->cancel(*cancelled);
                platform.stop();
            }

            Platform& platform;
            PollWaiter waiter;
            PollWaiter* cancelled = nullptr;
            u32& callCount;
        };

        struct CancelTimerCallback final: TimerCallback {
            CancelTimerCallback(
                Platform& platform_,
                TimerCallback& cancelled_
            )
                : platform(platform_)
                , cancelled(cancelled_)
            {
            }

            void ready() override {
                called = true;
                platform.poller()->cancel(cancelled);
                platform.stop();
            }

            Platform& platform;
            TimerCallback& cancelled;
            bool called = false;
        };
    }

    bool pollerApi(int fd) {
        Client client(fd);
        int pipes[2];
        if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0) {
            perror("pipe2");
            return false;
        }

        FdCallback ready(*client.platform);
        ready.arm(pipes[0], PollFlag::In);
        u8 byte = 1;
        if (write(pipes[1], &byte, 1) != 1) {
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }
        client.platform->run();
        if (!ready.called || !ready.readOk
            || !(ready.event.flags & PollFlag::In)) {
            fprintf(stderr, "poller API: fd callback failed\n");
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }

        FdCallback cancelledWait(*client.platform);
        cancelledWait.arm(pipes[0], PollFlag::In);
        client.platform->poller()->cancel(cancelledWait.waiter);
        if (write(pipes[1], &byte, 1) != 1) {
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }
        pump(*client.platform);
        if (read(pipes[0], &byte, 1) != 1) {
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }
        if (cancelledWait.called) {
            fprintf(stderr, "poller API: waiter cancel failed\n");
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }

        // Several waiters may watch one descriptor; a single readiness
        // detaches and serves every one of them.
        FdCallback firstOfTwo(*client.platform);
        FdCallback secondOfTwo(*client.platform);
        firstOfTwo.stopOnReady = false;
        firstOfTwo.arm(pipes[0], PollFlag::In);
        secondOfTwo.arm(pipes[0], PollFlag::In);
        if (write(pipes[1], &byte, 1) != 1) {
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }
        client.platform->run();
        if (!firstOfTwo.called || !secondOfTwo.called) {
            fprintf(stderr, "poller API: shared descriptor waiters failed\n");
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }

        DeadlineCallback cancelled(*client.platform);
        client.platform->poller()->timeout(0, cancelled);
        client.platform->poller()->cancel(cancelled);
        pump(*client.platform);
        if (cancelled.called) {
            fprintf(stderr, "poller API: timer cancel failed\n");
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }

        DeadlineCallback cancelledByReady(*client.platform);
        CancelTimerCallback cancelFromReady(
            *client.platform,
            cancelledByReady
        );
        client.platform->poller()->timeout(0, cancelFromReady);
        client.platform->poller()->timeout(0, cancelledByReady);
        client.platform->run();
        if (!cancelFromReady.called || cancelledByReady.called) {
            fprintf(
                stderr,
                "poller API: ready timer cancellation failed\n"
            );
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }

        int crossPipes[2][2];
        if (pipe2(crossPipes[0], O_CLOEXEC | O_NONBLOCK) != 0
            || pipe2(crossPipes[1], O_CLOEXEC | O_NONBLOCK) != 0) {
            perror("pipe2");
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }
        u32 crossCallCount = 0;
        CrossCancelFD first(*client.platform, crossCallCount);
        CrossCancelFD second(*client.platform, crossCallCount);
        first.cancelled = &second.waiter;
        second.cancelled = &first.waiter;
        first.arm(crossPipes[0][0]);
        second.arm(crossPipes[1][0]);
        if (write(crossPipes[0][1], &byte, 1) != 1
            || write(crossPipes[1][1], &byte, 1) != 1) {
            for (auto& crossPipe : crossPipes) {
                close(crossPipe[0]);
                close(crossPipe[1]);
            }
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }
        client.platform->run();
        for (auto& crossPipe : crossPipes) {
            close(crossPipe[0]);
            close(crossPipe[1]);
        }
        if (crossCallCount != 1) {
            fprintf(
                stderr,
                "poller API: ready fd cancellation invoked %u callbacks\n",
                crossCallCount
            );
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }

        DeadlineCallback deadline(*client.platform);
        client.platform->poller()->deadline(monotonicNowUs(), deadline);
        client.platform->run();
        close(pipes[0]);
        close(pipes[1]);
        if (!deadline.called) {
            fprintf(stderr, "poller API: deadline failed\n");
            return false;
        }
        return true;
    }
}
