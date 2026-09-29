#pragma once

#include <std/str/view.h>

namespace stl {
    class Input;
    class Output;
}

namespace plt {
    // Streams one payload of a drag-and-drop transfer. chunk is valid only
    // for the duration of the call. Returning false stops the transfer and
    // completes it with success=false; done() is called exactly once unless
    // the transfer is cancelled.
    struct ClipboardRead {
        virtual bool data(stl::StringView chunk) = 0;
        virtual void done(bool success) = 0;
    };

    // Fiber-only clipboard streams. Both calls return an object owned by
    // the caller; plain delete releases it. Deleting the Input before end
    // of stream cancels the transfer; deleting the Output before finish()
    // abandons the write without touching the selection.
    struct Clipboard {
        // Streams the current selection; an absent selection reads as
        // empty. A read that would block parks the calling fiber while the
        // event loop keeps running.
        virtual stl::Input* read() = 0;
        // Accumulates a replacement selection; finish() publishes it.
        virtual stl::Output* write() = 0;
    };
}
