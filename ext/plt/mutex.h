#pragma once

namespace plt {
    // A cooperative FIFO mutex for fibers, made by Scheduler::createMutex
    // and bound to that scheduler for good. The mutex itself is only the
    // wait list and a flag: each blocked lock() parks on a waiter node
    // living on the calling fiber's stack. unlock() hands the mutex to the
    // oldest waiter and resumes it inline; it may run outside a fiber.
    struct FiberMutex {
        // Blocks the calling fiber until the mutex is granted. May be
        // called only inside a fiber.
        virtual void lock() = 0;
        // Takes the mutex when it is free; never blocks.
        virtual bool tryLock() = 0;
        virtual void unlock() = 0;
        // True while some fiber owns the mutex.
        virtual bool locked() const = 0;
        // True when the calling fiber holds the mutex; lets a writer called
        // from an owning transaction pass through instead of deadlocking.
        virtual bool heldByCurrent() const = 0;
    };

    // Scoped ownership of a FiberMutex: locks on construction, unlocks on
    // destruction, so a transaction releases the stream on every exit path.
    struct LockGuard {
        explicit LockGuard(FiberMutex& mutex);
        ~LockGuard();

        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;

        FiberMutex& mutex;
    };
}
