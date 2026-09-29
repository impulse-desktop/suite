#include "test.h"

#include "xdg-decoration-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>

namespace plt::test {
    bool decorations(int fd) {
        Client client(fd);
        Reply mode = command(fd, Command::QueryDecoration);
        if (mode.count != 1 || mode.first != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
            fprintf(stderr, "decorations: default mode=%d count=%u\n", mode.first, mode.count);
            return false;
        }

        Window* const borderless = client.platform->createWindow(
            *client.owner,
            {
                .appId = stl::StringView(u8"plt.integration.borderless"),
                .title = stl::StringView(u8"borderless"),
                .decorations = false,
            }
        );
        borderless->requestShow();
        pump(*client.platform);

        mode = command(fd, Command::QueryDecoration);
        if (mode.count != 2 || mode.first != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) {
            fprintf(stderr, "decorations: borderless mode=%d count=%u\n", mode.first, mode.count);
            return false;
        }
        return true;
    }

    bool windowApi(int fd) {
        EventSink events;
        Client client(fd, 800, 1, &events, nullptr, true, &events);
        client.window->requestShow();

        const RenderContext render = client.window->renderContext();
        const WindowInfo initial = events.lastInfo;
        if (render.backend != RenderBackend::Wayland
            || render.connection == nullptr || render.window == nullptr
            || initial.width != 800 || initial.height != 600
            || initial.screenPixelWidth != 1920
            || initial.screenPixelHeight != 1080
            || initial.contentScale != 1
            || initial.focused || initial.maximized || initial.fullscreen
            || initial.tiled || initial.iconified
            || events.frameCount == 0
            || events.lastInfo.width != initial.width
            || events.lastInfo.height != initial.height) {
            fprintf(stderr, "window API: invalid initial state or render context\n");
            return false;
        }

        const u32 frames = events.frameCount;
        client.window->requestFrame();
        client.window->requestClose();
        client.window->requestClose();
        pump(*client.platform);
        if (events.frameCount != frames + 1 || events.closeCount != 1) {
            fprintf(stderr, "window API: direct callbacks were not idempotent\n");
            return false;
        }

        client.window->requestTitle(stl::StringView(u8"updated title"));
        client.window->requestMinimumSize(320, 240);
        client.window->requestResizeUnit(10, 20, 3, 7);
        command(fd, Command::PointerEnter);
        pump(*client.platform);
        client.window->requestMove(11, 22);
        client.window->requestMaximized(true);
        client.window->requestMaximized(false);
        client.window->requestFullscreen(true);
        client.window->requestFullscreen(false);
        client.window->requestRestore();
        client.window->requestIconify();
        client.window->requestAttention();
        pump(*client.platform);
        if (command(fd, Command::QueryActivation).count != 1) {
            fprintf(stderr, "window API: requestAttention did not activate\n");
            return false;
        }
        client.window->requestFocus();
        pump(*client.platform);
        if (command(fd, Command::QueryActivation).count != 2) {
            fprintf(stderr, "window API: focus did not activate\n");
            return false;
        }

        const u32 expectedRequests =
            UpdatedTitle | InitialAppId | Move | Maximize | Unmaximize
            | Fullscreen | Unfullscreen | Minimize;
        const Reply requests = command(fd, Command::QueryWindowRequests);
        const Reply minimum = command(fd, Command::QueryMinimum);
        if ((requests.count & expectedRequests) != expectedRequests
            || minimum.first != 320 || minimum.second != 240) {
            fprintf(
                stderr,
                "window API: request mask=%x minimum=%dx%d\n",
                requests.count,
                minimum.first,
                minimum.second
            );
            return false;
        }

        command(fd, Command::ConfigureWindowState);
        pump(*client.platform);
        const WindowInfo state = events.lastInfo;
        if (state.width != 900 || state.height != 700 || !state.focused
            || !state.maximized || !state.fullscreen || !state.tiled) {
            fprintf(stderr, "window API: configured state was not exposed\n");
            return false;
        }

        command(fd, Command::ConfigureWindowResize);
        pump(*client.platform);
        const WindowInfo resized = events.lastInfo;
        if (resized.width != 813 || resized.height != 627
            || resized.focused || resized.maximized
            || resized.fullscreen || resized.tiled) {
            fprintf(
                stderr,
                "window API: snapped size=%ux%u expected 813x627\n",
                resized.width,
                resized.height
            );
            return false;
        }

        client.window->requestResize(640, 480);
        pump(*client.platform);
        const Reply geometry = command(fd, Command::QueryWindowGeometry);
        if (geometry.first != 640 || geometry.second != 480
            || events.lastInfo.width != 640
            || events.lastInfo.height != 480) {
            fprintf(stderr, "window API: explicit resize failed\n");
            return false;
        }

        // The URI launcher spawns xdg-open fire-and-forget; an empty
        // PATH keeps the desktop out of the test while the spawn path
        // still runs.
        stl::Buffer previousPath;
        if (const char* const currentPath = getenv("PATH")) {
            previousPath.append(currentPath, stl::StringView(currentPath).length());
        }
        setenv("PATH", "/nonexistent", 1);
        client.window->requestOpenUri(stl::StringView(u8"https://example.test/"));
        if (!previousPath.empty()) {
            setenv("PATH", previousPath.cStr(), 1);
        }

        command(fd, Command::PingClient);
        pump(*client.platform);

        command(fd, Command::CloseWindow);
        pump(*client.platform);
        if (events.closeCount != 1) {
            fprintf(stderr, "window API: duplicate close callback\n");
            return false;
        }
        return true;
    }
}
