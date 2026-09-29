#include "test.h"

#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <wayland-client-core.h>

namespace {
    struct StopAfterReady final: plt::TimerCallback {
        explicit StopAfterReady(plt::Platform& platform_);

        void ready() override;

        plt::Platform& platform;
    };
}

StopAfterReady::StopAfterReady(plt::Platform& platform_)
    : platform(platform_)
{
}

void StopAfterReady::ready() {
    platform.stop();
}

namespace plt::test {
    bool queuedWaylandEvent(int fd) {
        Platform* platform = nullptr;
        StopOnClose events(platform);
        Client client(fd, 800, 1, &events);
        platform = client.platform;
        if (command(fd, Command::CloseWindow).count != 1) {
            fprintf(stderr, "queued event: close was not sent\n");
            return false;
        }

        auto* const display = static_cast<wl_display*>(client.window->renderContext().connection);
        if (wl_display_prepare_read(display) != 0) {
            fprintf(stderr, "queued event: client queue was not empty\n");
            return false;
        }
        pollfd source{
            .fd = wl_display_get_fd(display),
            .events = POLLIN,
            .revents = 0,
        };
        int result;
        do {
            result = poll(&source, 1, 1000);
        } while (result < 0 && errno == EINTR);
        if (result <= 0 || !(source.revents & POLLIN) || wl_display_read_events(display) < 0) {
            wl_display_cancel_read(display);
            fprintf(stderr, "queued event: could not queue close event\n");
            return false;
        }

        client.platform->run();
        if (!events.closed) {
            fprintf(stderr, "queued event: close callback stalled\n");
            return false;
        }
        return true;
    }

    bool foreignQueuedWaylandEvent(int fd) {
        Client client(fd);
        const RenderContext render = client.window->renderContext();
        auto* const display = static_cast<wl_display*>(render.connection);
        auto* const queue = wl_display_create_queue(display);
        if (queue == nullptr) {
            fprintf(stderr, "foreign queue: could not create event queue\n");
            return false;
        }
        auto* const surface = static_cast<wl_proxy*>(render.window);
        wl_proxy_set_queue(surface, queue);
        if (command(fd, Command::SurfaceEnter).count != 1) {
            wl_proxy_set_queue(surface, nullptr);
            wl_event_queue_destroy(queue);
            fprintf(stderr, "foreign queue: could not send surface event\n");
            return false;
        }

        pollfd source{
            .fd = wl_display_get_fd(display),
            .events = POLLIN,
            .revents = 0,
        };
        int result;
        do {
            result = poll(&source, 1, 1000);
        } while (result < 0 && errno == EINTR);
        if (result <= 0 || !(source.revents & POLLIN)) {
            wl_proxy_set_queue(surface, nullptr);
            wl_event_queue_destroy(queue);
            fprintf(stderr, "foreign queue: surface event did not arrive\n");
            return false;
        }

        StopAfterReady stop(*client.platform);
        client.platform->poller()->defer(stop);
        client.platform->run();
        const int dispatched = wl_display_dispatch_queue_pending(display, queue);
        wl_proxy_set_queue(surface, nullptr);
        wl_event_queue_destroy(queue);
        if (dispatched <= 0) {
            fprintf(stderr, "foreign queue: surface event was not queued\n");
            return false;
        }
        return true;
    }
}
