#include "mutex.h"

#include "fiber.h"

#include <std/lib/node.h>
#include <std/lib/list.h>
#include <std/mem/obj_pool.h>

using namespace plt;
using namespace stl;

namespace {
    struct MutexWaiter final: public IntrusiveNode {
        Fiber* fiber = nullptr;
        bool granted = false;
    };

    struct FiberMutexImpl final: public FiberMutex {
        explicit FiberMutexImpl(Scheduler& scheduler);

        void lock() override;
        bool tryLock() override;
        void unlock() override;
        bool locked() const override;
        bool heldByCurrent() const override;

        Scheduler* const scheduler;
        IntrusiveList waiters;
        Fiber* owner = nullptr;
        bool held = false;
    };
}

FiberMutexImpl::FiberMutexImpl(Scheduler& scheduler_)
    : scheduler(&scheduler_)
{
}

void FiberMutexImpl::lock() {
    if (tryLock()) {
        owner = scheduler->current();
        return;
    }
    MutexWaiter waiter;
    waiter.fiber = scheduler->current();
    waiters.pushBack(&waiter);
    // A remembered wake from an unrelated park/wake pair may end the park
    // early; only the grant made by unlock() releases the loop.
    while (!waiter.granted) {
        waiter.fiber->park();
    }
}

bool FiberMutexImpl::tryLock() {
    if (held) {
        return false;
    }
    held = true;
    return true;
}

void FiberMutexImpl::unlock() {
    if (waiters.empty()) {
        held = false;
        owner = nullptr;
        return;
    }
    // Ownership passes directly to the oldest waiter: held stays true, so
    // writers arriving between this unlock and the waiter resuming still
    // queue behind it.
    MutexWaiter* const waiter = static_cast<MutexWaiter*>(waiters.popFront());
    waiter->granted = true;
    owner = waiter->fiber;
    waiter->fiber->wake();
}

bool FiberMutexImpl::locked() const {
    return held;
}

bool FiberMutexImpl::heldByCurrent() const {
    return held && owner != nullptr && owner == scheduler->current();
}

FiberMutex* Scheduler::createMutex(ObjPool& owner) {
    return owner.make<FiberMutexImpl>(*this);
}

LockGuard::LockGuard(FiberMutex& mutex_)
    : mutex(mutex_)
{
    mutex.lock();
}

LockGuard::~LockGuard() {
    mutex.unlock();
}
