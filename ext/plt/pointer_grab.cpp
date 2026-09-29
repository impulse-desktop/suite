#include "pointer_grab.h"

using namespace plt;

void PointerGrab::enter(void* target) noexcept {
    focus = target;
}

void PointerGrab::leave(void* target) noexcept {
    if (focus == target) {
        focus = nullptr;
    }
}

void PointerGrab::remove(void* target) noexcept {
    if (focus == target) {
        focus = nullptr;
    }
    if (grab == target) {
        grab = nullptr;
        pressedButtons = 0;
    }
}

void PointerGrab::reset() noexcept {
    focus = nullptr;
    grab = nullptr;
    pressedButtons = 0;
}

void* PointerGrab::focusTarget() const noexcept {
    return focus;
}

void* PointerGrab::eventTarget() const noexcept {
    return grab != nullptr ? grab : focus;
}

void* PointerGrab::buttonTarget(bool pressed) noexcept {
    if (pressed) {
        if (pressedButtons == 0) {
            grab = focus;
        }
        if (grab != nullptr) {
            ++pressedButtons;
        }
        return eventTarget();
    }
    void* const target = eventTarget();
    if (pressedButtons != 0) {
        --pressedButtons;
        if (pressedButtons == 0) {
            grab = nullptr;
        }
    }
    return target;
}
