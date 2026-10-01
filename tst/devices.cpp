#include "virtual-pointer-client.h"
#include "virtual-keyboard-client.h"

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

extern "C" {
#include "virtual-keyboard-code.h"
#include "virtual-pointer-code.h"
}

namespace {
    struct Devices {
        wl_seat* seat = nullptr;
        zwlr_virtual_pointer_manager_v1* pointers = nullptr;
        zwp_virtual_keyboard_manager_v1* keyboards = nullptr;
    };

    void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
        auto& devices = *static_cast<Devices*>(data);
        printf("GLOBAL %s %u\n", interface, version);
        if (strcmp(interface, wl_seat_interface.name) == 0) {
            devices.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        } else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
            devices.pointers = static_cast<zwlr_virtual_pointer_manager_v1*>(wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1));
        } else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
            devices.keyboards = static_cast<zwp_virtual_keyboard_manager_v1*>(wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
        }
    }

    void removed(void*, wl_registry*, uint32_t) {
    }

    xkb_state* uploadKeymap(zwp_virtual_keyboard_v1* keyboard) {
        xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        xkb_rule_names names{};
        names.layout = "us";
        xkb_keymap* keymap = context ? xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
        char* text = keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : nullptr;
        xkb_state* state = nullptr;
        if (text) {
            const size_t size = strlen(text) + 1;
            const int fd = memfd_create("keymap", MFD_CLOEXEC);
            if (fd >= 0 && ftruncate(fd, (off_t)size) == 0 && pwrite(fd, text, size, 0) == (ssize_t)size) {
                zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
                state = xkb_state_new(keymap);
            }
            if (fd >= 0) {
                close(fd);
            }
            free(text);
        }
        if (keymap) {
            xkb_keymap_unref(keymap);
        }
        if (context) {
            xkb_context_unref(context);
        }
        return state;
    }

    struct Modifiers {
        uint32_t depressed = 0;
        uint32_t latched = 0;
        uint32_t locked = 0;
        uint32_t group = 0;

        bool operator==(const Modifiers& other) const = default;
    };

    Modifiers modifiers(xkb_state* state) {
        return {
            xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED),
            xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED),
            xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED),
            xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE),
        };
    }
}

int main() {
    wl_display* const display = wl_display_connect(nullptr);
    if (display == nullptr) {
        return 1;
    }
    Devices devices;
    wl_registry* const registry = wl_display_get_registry(display);
    const wl_registry_listener listener{global, removed};
    wl_registry_add_listener(registry, &listener, &devices);
    if (wl_display_roundtrip(display) < 0 || devices.seat == nullptr || devices.pointers == nullptr || devices.keyboards == nullptr) {
        fprintf(stderr, "devices: the compositor offers no seat, virtual pointer or virtual keyboard\n");
        return 1;
    }
    auto* const pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(devices.pointers, devices.seat);
    auto* const keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(devices.keyboards, devices.seat);
    xkb_state* const state = uploadKeymap(keyboard);
    if (state == nullptr || wl_display_roundtrip(display) < 0) {
        fprintf(stderr, "devices: no keymap for the virtual keyboard\n");
        return 1;
    }
    Modifiers sent = modifiers(state);
    puts("READY");
    fflush(stdout);
    char line[128];
    unsigned int serial = 0;
    while (fgets(line, sizeof(line), stdin) != nullptr) {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const uint32_t time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
        unsigned int x, y, width, height, code, pressed, depressed, latched, locked;
        int steps;
        if (sscanf(line, "move %u %u %u %u", &x, &y, &width, &height) == 4) {
            zwlr_virtual_pointer_v1_motion_absolute(pointer, time, x, y, width, height);
            zwlr_virtual_pointer_v1_frame(pointer);
        } else if (sscanf(line, "button %u %u", &code, &pressed) == 2) {
            zwlr_virtual_pointer_v1_button(pointer, time, code, pressed);
            zwlr_virtual_pointer_v1_frame(pointer);
        } else if (sscanf(line, "scroll %d", &steps) == 1) {
            zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
            zwlr_virtual_pointer_v1_axis_discrete(pointer, time, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(steps * 10), steps);
            zwlr_virtual_pointer_v1_frame(pointer);
        } else if (sscanf(line, "key %u %u", &code, &pressed) == 2) {
            xkb_state_update_key(state, code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
            const Modifiers now = modifiers(state);
            if (!(now == sent)) {
                zwp_virtual_keyboard_v1_modifiers(keyboard, now.depressed, now.latched, now.locked, now.group);
                sent = now;
            }
            zwp_virtual_keyboard_v1_key(keyboard, time, code, pressed);
        } else if (sscanf(line, "mods %u %u %u", &depressed, &latched, &locked) == 3) {
            zwp_virtual_keyboard_v1_modifiers(keyboard, depressed, latched, locked, 0);
        } else {
            fprintf(stderr, "devices: unknown command: %s", line);
            return 1;
        }
        if (wl_display_roundtrip(display) < 0) {
            return 1;
        }
        printf("DONE %u\n", ++serial);
        fflush(stdout);
    }
    xkb_state_unref(state);
    zwp_virtual_keyboard_v1_destroy(keyboard);
    zwlr_virtual_pointer_v1_destroy(pointer);
    zwp_virtual_keyboard_manager_v1_destroy(devices.keyboards);
    zwlr_virtual_pointer_manager_v1_destroy(devices.pointers);
    wl_seat_destroy(devices.seat);
    wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
}
