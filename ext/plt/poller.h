#pragma once

#include <std/lib/node.h>
#include <std/sys/types.h>
#include <std/thr/poll_fd.h>

namespace plt {
    struct PollCallback {
        virtual void ready(stl::PollFD event) = 0;
    };

    struct TimerCallback {
        virtual void ready() = 0;
    };

    // One pending descriptor wait. The node lives with its waiter — on a
    // fiber's await frame or inside an owner object — and any number of
    // waiters may watch one descriptor at once. Waits are one-shot: the
    // poller unlinks a node before running its callback, and the callback
    // re-arms if it wants more events. arm() on a linked node re-links it,
    // and cancel() guarantees the callback does not run afterwards, even
    // from a dispatch round already in progress.
    struct PollWaiter: public stl::IntrusiveNode {
        stl::PollFD fd{};
        PollCallback* callback = nullptr;
        // The readiness of the dispatch in flight, written by the poller.
        u32 readyFlags = 0;
    };

    // Timers are keyed by callback: timeout()/deadline() replace the
    // pending deadline for that callback, and cancel() guarantees the
    // callback does not run afterwards.
    struct Poller {
        virtual void arm(PollWaiter& waiter) = 0;
        virtual void cancel(PollWaiter& waiter) = 0;
        virtual void timeout(u64 microseconds, TimerCallback& callback) = 0;
        virtual void deadline(u64 monotonicMicroseconds, TimerCallback& callback) = 0;
        virtual void cancel(TimerCallback& callback) = 0;
        // Runs the callback after the next poll round, once the ready
        // descriptor waiters of that round have been dispatched. Unlike a
        // zero timer — which fires before the descriptors are polled — a
        // callback that defers in a loop still lets every armed waiter
        // make progress.
        virtual void defer(TimerCallback& callback) = 0;
    };
}
