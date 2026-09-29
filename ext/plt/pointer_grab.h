#pragma once

#include <std/sys/types.h>

namespace plt {
    struct PointerGrab {
        void enter(void* target) noexcept;
        void leave(void* target) noexcept;
        void remove(void* target) noexcept;
        void reset() noexcept;
        void* focusTarget() const noexcept;
        void* eventTarget() const noexcept;
        void* buttonTarget(bool pressed) noexcept;

        void* focus = nullptr;
        void* grab = nullptr;
        u32 pressedButtons = 0;
    };
}
