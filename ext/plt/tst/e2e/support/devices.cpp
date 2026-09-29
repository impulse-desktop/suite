#include "virtual-pointer-client.h"

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <wayland-client.h>

extern "C" {
#include "virtual-pointer-code.h"
}

namespace {
    struct Devices {
        wl_seat* seat = nullptr;
        zwlr_virtual_pointer_manager_v1* manager = nullptr;
    };

    void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
        auto& devices = *static_cast<Devices*>(data);
        if (strcmp(interface, wl_seat_interface.name) == 0) {
            devices.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        } else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
            devices.manager = static_cast<zwlr_virtual_pointer_manager_v1*>(wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1));
        }
    }

    void removed(void*, wl_registry*, uint32_t) {
    }
}

// Keep a real virtual pointer attached and inject the driver's input events.
int main() {
    wl_display* const display = wl_display_connect(nullptr);
    if (display == nullptr) {
        return 1;
    }
    Devices devices;
    wl_registry* const registry = wl_display_get_registry(display);
    const wl_registry_listener listener{global, removed};
    wl_registry_add_listener(registry, &listener, &devices);
    if (wl_display_roundtrip(display) < 0 || devices.seat == nullptr || devices.manager == nullptr) {
        return 1;
    }
    auto* const pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(devices.manager, devices.seat);
    if (wl_display_roundtrip(display) < 0) {
        return 1;
    }
    puts("READY");
    fflush(stdout);
    char line[128];
    unsigned int serial = 0;
    while (fgets(line, sizeof(line), stdin) != nullptr) {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const uint32_t time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
        unsigned int x, y, width, height, button, pressed;
        int steps;
        if (sscanf(line, "move %u %u %u %u", &x, &y, &width, &height) == 4) {
            zwlr_virtual_pointer_v1_motion_absolute(pointer, time, x, y, width, height);
        } else if (sscanf(line, "button %u %u", &button, &pressed) == 2) {
            zwlr_virtual_pointer_v1_button(pointer, time, button, pressed);
        } else if (sscanf(line, "scroll %d", &steps) == 1) {
            zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
            zwlr_virtual_pointer_v1_axis_discrete(pointer, time, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(steps * 10), steps);
        } else {
            return 1;
        }
        zwlr_virtual_pointer_v1_frame(pointer);
        if (wl_display_roundtrip(display) < 0) {
            return 1;
        }
        printf("DONE %u\n", ++serial);
        fflush(stdout);
    }
    zwlr_virtual_pointer_v1_destroy(pointer);
    zwlr_virtual_pointer_manager_v1_destroy(devices.manager);
    wl_seat_destroy(devices.seat);
    wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
}
