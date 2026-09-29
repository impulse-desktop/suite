#include "test.h"

#include "cursor-shape-v1-server-protocol.h"

#include <stdio.h>

namespace plt::test {
    namespace {
        bool expectShape(Client& client, int fd, PointerIcon icon, u32 shape) {
            client.window->requestPointerIcon(icon);
            pump(*client.platform);
            const Reply cursor = command(fd, Command::QueryCursor);
            if (cursor.first != static_cast<i32>(shape) || cursor.second != 1) {
                fprintf(
                    stderr,
                    "cursor shapes: icon %u produced shape %d instead of %u\n",
                    static_cast<u32>(icon),
                    cursor.first,
                    shape
                );
                return false;
            }
            return true;
        }
    }

    bool cursorShapes(int fd) {
        Client client(fd);
        command(fd, Command::PointerEnter);
        pump(*client.platform);

        struct Shape {
            PointerIcon icon;
            u32 shape;
        };
        static constexpr Shape shapes[]{
            {PointerIcon::Default, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
            {PointerIcon::ContextMenu, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU},
            {PointerIcon::Help, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP},
            {PointerIcon::Pointer, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
            {PointerIcon::Progress, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS},
            {PointerIcon::Wait, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT},
            {PointerIcon::Cell, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL},
            {PointerIcon::Crosshair, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR},
            {PointerIcon::Text, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT},
            {PointerIcon::VerticalText, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT},
            {PointerIcon::Alias, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS},
            {PointerIcon::Copy, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
            {PointerIcon::Move, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
            {PointerIcon::NoDrop, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP},
            {PointerIcon::NotAllowed, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED},
            {PointerIcon::Grab, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB},
            {PointerIcon::Grabbing, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING},
            {PointerIcon::ResizeEast, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE},
            {PointerIcon::ResizeNorth, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE},
            {PointerIcon::ResizeNorthEast, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE},
            {PointerIcon::ResizeNorthWest, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE},
            {PointerIcon::ResizeSouth, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE},
            {PointerIcon::ResizeSouthEast, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE},
            {PointerIcon::ResizeSouthWest, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE},
            {PointerIcon::ResizeWest, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE},
            {PointerIcon::ResizeEastWest, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE},
            {PointerIcon::ResizeNorthSouth, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE},
            {PointerIcon::ResizeNorthEastSouthWest, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE},
            {PointerIcon::ResizeNorthWestSouthEast, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE},
            {PointerIcon::ResizeColumn, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE},
            {PointerIcon::ResizeRow, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE},
            {PointerIcon::AllScroll, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL},
            {PointerIcon::ZoomIn, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN},
            {PointerIcon::ZoomOut, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT},
            {PointerIcon::DndAsk, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK},
            {PointerIcon::ResizeAll, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE},
            {PointerIcon::DisappearingItem, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP},
        };
        for (const Shape& shape : shapes) {
            if (!expectShape(client, fd, shape.icon, shape.shape)) {
                return false;
            }
        }
        return true;
    }

    bool cursorShapesV1(int fd) {
        if (command(fd, Command::CursorShapeV1).count != 1) {
            fprintf(stderr, "cursor shapes v1: could not downgrade the global\n");
            return false;
        }
        Client client(fd);
        command(fd, Command::PointerEnter);
        pump(*client.platform);

        // Bound at v1, the v2-only shapes must degrade to v1 equivalents
        // while v1 shapes still pass through unchanged.
        if (!expectShape(client, fd, PointerIcon::ZoomIn, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN)
            || !expectShape(client, fd, PointerIcon::DndAsk, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY)
            || !expectShape(client, fd, PointerIcon::ResizeAll, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE)) {
            return false;
        }
        return true;
    }
}
