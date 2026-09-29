#include "input.h"

#include "fiber.h"

#include <std/lib/list.h>
#include <std/str/view.h>
#include <std/lib/buffer.h>
#include <std/thr/runable.h>
#include <std/mem/obj_list.h>
#include <std/mem/obj_pool.h>

using namespace plt;
using namespace stl;

namespace {
    constexpr size_t fiberSinkStack = 64 * 1024;

    struct FiberSinkImpl;

    struct SinkEvent final: public IntrusiveNode {
        enum class Type : u8 {
            Key,
            Text,
            Preedit,
            PointerMotion,
            PointerButton,
            Scroll,
            Focus,
            PointerPresence,
            Flush
        };

        Type type = Type::Flush;
        KeyInput key{};
        TextInput text{};
        PointerMotionInput motion{};
        PointerButtonInput button{};
        ScrollInput scroll{};
        bool flag = false;
        i32 cursorBegin = -1;
        i32 cursorEnd = -1;
        Buffer payload;
    };

    struct SinkPump final: public Runable {
        explicit SinkPump(FiberSinkImpl* sink);

        void run() override;

        FiberSinkImpl* sink;
    };

    struct FiberSinkImpl final: public InputSink {
        FiberSinkImpl(ObjPool& owner, Scheduler& scheduler, InputSink& target);
        ~FiberSinkImpl() noexcept;

        void key(const KeyInput& input) override;
        void text(const TextInput& input) override;
        void preedit(StringView text, i32 cursorBegin, i32 cursorEnd) override;
        void pointerMotion(const PointerMotionInput& input) override;
        void pointerButton(const PointerButtonInput& input) override;
        void scroll(const ScrollInput& input) override;
        void focus(bool focused) override;
        void pointerPresence(bool present) override;
        void flush() override;

        void push(SinkEvent& event);
        void deliver(const SinkEvent& event);

        Scheduler& scheduler;
        InputSink& target;
        SinkPump pump;
        ObjList<SinkEvent> events;
        IntrusiveList queue;
        Fiber* fiber = nullptr;
    };
}

SinkPump::SinkPump(FiberSinkImpl* sink_)
    : sink(sink_)
{
}

void SinkPump::run() {
    FiberSinkImpl& impl = *sink;
    for (;;) {
        while (impl.queue.empty()) {
            impl.scheduler.current()->park();
        }
        // Keep the event owned by the queue while delivery can block:
        // releasing the fiber does not unwind its stack.
        SinkEvent* const event = static_cast<SinkEvent*>(impl.queue.mutFront());
        impl.deliver(*event);
        impl.queue.popFront();
        impl.events.release(event);
    }
}

FiberSinkImpl::FiberSinkImpl(ObjPool& owner, Scheduler& scheduler_, InputSink& target_)
    : scheduler(scheduler_)
    , target(target_)
    , pump(this)
    , events(&owner)
{
}

FiberSinkImpl::~FiberSinkImpl() noexcept {
    while (!queue.empty()) {
        events.release(static_cast<SinkEvent*>(queue.popFront()));
    }
}

void FiberSinkImpl::push(SinkEvent& event) {
    queue.pushBack(&event);
    if (fiber != nullptr) {
        fiber->wake();
    }
}

void FiberSinkImpl::deliver(const SinkEvent& event) {
    const StringView payload(event.payload);
    switch (event.type) {
        case SinkEvent::Type::Key: {
            target.key(event.key);
            break;
        }
        case SinkEvent::Type::Text: {
            target.text(event.text);
            break;
        }
        case SinkEvent::Type::Preedit: {
            target.preedit(payload, event.cursorBegin, event.cursorEnd);
            break;
        }
        case SinkEvent::Type::PointerMotion: {
            target.pointerMotion(event.motion);
            break;
        }
        case SinkEvent::Type::PointerButton: {
            target.pointerButton(event.button);
            break;
        }
        case SinkEvent::Type::Scroll: {
            target.scroll(event.scroll);
            break;
        }
        case SinkEvent::Type::Focus: {
            target.focus(event.flag);
            break;
        }
        case SinkEvent::Type::PointerPresence: {
            target.pointerPresence(event.flag);
            break;
        }
        case SinkEvent::Type::Flush: {
            target.flush();
            break;
        }
    }
}

void FiberSinkImpl::key(const KeyInput& input) {
    SinkEvent& event = *events.make();
    event.type = SinkEvent::Type::Key;
    event.key = input;
    push(event);
}

void FiberSinkImpl::text(const TextInput& input) {
    SinkEvent& event = *events.make();
    event.type = SinkEvent::Type::Text;
    event.text = input;
    push(event);
}

void FiberSinkImpl::preedit(StringView text, i32 cursorBegin, i32 cursorEnd) {
    SinkEvent& event = *events.make();
    event.type = SinkEvent::Type::Preedit;
    event.payload.append(text.data(), text.length());
    event.cursorBegin = cursorBegin;
    event.cursorEnd = cursorEnd;
    push(event);
}

void FiberSinkImpl::pointerMotion(const PointerMotionInput& input) {
    SinkEvent& event = *events.make();
    event.type = SinkEvent::Type::PointerMotion;
    event.motion = input;
    push(event);
}

void FiberSinkImpl::pointerButton(const PointerButtonInput& input) {
    SinkEvent& event = *events.make();
    event.type = SinkEvent::Type::PointerButton;
    event.button = input;
    push(event);
}

void FiberSinkImpl::scroll(const ScrollInput& input) {
    SinkEvent& event = *events.make();
    event.type = SinkEvent::Type::Scroll;
    event.scroll = input;
    push(event);
}

void FiberSinkImpl::focus(bool focused) {
    SinkEvent& event = *events.make();
    event.type = SinkEvent::Type::Focus;
    event.flag = focused;
    push(event);
}

void FiberSinkImpl::pointerPresence(bool present) {
    SinkEvent& event = *events.make();
    event.type = SinkEvent::Type::PointerPresence;
    event.flag = present;
    push(event);
}

void FiberSinkImpl::flush() {
    SinkEvent& event = *events.make();
    event.type = SinkEvent::Type::Flush;
    push(event);
}

InputSink* plt::createFiberInputSink(ObjPool& owner, Scheduler& scheduler, InputSink& target) {
    FiberSinkImpl* const sink = owner.make<FiberSinkImpl>(owner, scheduler, target);
    // Deliveries run through client handlers down to the PTY write, so the
    // pump is not a light fiber. The owner also owns the parked fiber: its
    // destructor releases the scheduler state before the sink disappears.
    sink->fiber = scheduler.create(owner, sink->pump, fiberSinkStack);
    return sink;
}
