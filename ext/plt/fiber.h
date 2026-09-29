#pragma once

#include <std/sys/types.h>

namespace stl {
    class ObjPool;
    struct Runable;
}

namespace plt {
    struct FiberMutex;
    struct Poller;

    // Enough for a leaf fiber that keeps only chunk buffers of a few
    // kilobytes on its stack; give parser- or renderer-deep fibers more.
    inline constexpr size_t lightFiberStack = 32 * 1024;

    // A running fiber. park() blocks the fiber until wake() and may be
    // called only from the fiber itself; wake() may be called from anywhere
    // on the platform thread. Being single-threaded there is no publication
    // race to defend against: a wake that arrives while the fiber is not
    // parked is remembered and the next park returns immediately.
    struct Fiber {
        virtual void park() = 0;
        // Parks until wake() or the deadline; false when the wait timed
        // out. May be called only from the fiber itself.
        virtual bool parkFor(u64 timeoutUs) = 0;
        virtual void wake() = 0;
        // Frees a non-running fiber without ever resuming it: the stack is the
        // caller's again the moment this returns, which is what lets an
        // arena drop a whole object graph - stacks included - while its
        // fibers sit parked. A fiber blocked in park(), parkFor(), an await
        // or yield may be released, but a running fiber may never release
        // itself. The handle is dead to the caller afterwards;
        // poller references armed at this moment (a parkFor deadline, an
        // awaited descriptor) collect it themselves when they fire. A
        // fiber parked inside a FiberMutex queue may be released only when
        // that mutex is never unlocked again: its wait node lives on the
        // stack being freed.
        virtual void release() = 0;
    };

    // A single-threaded cooperative fiber scheduler married to a Poller.
    // Fibers run on the platform thread: a blocked fiber resumes inside the
    // poller callback that made it runnable and switches back before the
    // loop continues, so fibers, callbacks and the event loop interleave
    // freely. Everything a fiber touches lives on its own stack, which lets
    // deeply nested code block on I/O without stopping the loop.
    struct Scheduler {
        // Runs entry immediately on the caller-provided stack until it
        // first blocks or returns. Entry and the stack stay the caller's
        // and must outlive the fiber; the control block is the engine's
        // own, freed when the fiber finishes or is released, so from
        // either point on the stack may be reused for the next spawn.
        virtual void spawn(stl::Runable& entry, void* stack, size_t size) = 0;

        // Starts a fiber whose handle and stack belong to owner. The
        // returned handle stays valid until owner dies, even if entry has
        // already returned; wake() is then a no-op. Destroying owner
        // releases the fiber from any blocked state without resuming it.
        // The scheduler and entry must outlive owner, and owner may not be
        // destroyed by this fiber itself while it is running.
        virtual Fiber* create(stl::ObjPool& owner, stl::Runable& entry, size_t stackSize = lightFiberStack) = 0;

        // The calls below block the calling fiber only and must not be used
        // outside one. false means the wait timed out; a timeout of 0 waits
        // without a deadline.
        virtual bool awaitReadable(int fd, u64 timeoutUs) = 0;
        virtual bool awaitWritable(int fd, u64 timeoutUs) = 0;
        virtual void yield() = 0;
        // The running fiber, nullptr outside any.
        virtual Fiber* current() = 0;

        // A fiber mutex bound to this scheduler, owned by the pool.
        FiberMutex* createMutex(stl::ObjPool& owner);

        static Scheduler* create(stl::ObjPool& owner, Poller& poller);
    };
}
