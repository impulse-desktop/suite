#pragma once

#include <std/str/view.h>

// `im screenshot <path>`: a small wayland+vulkan imgui client that loads
// the raw ARGB image at <path> (a self-describing memfd the compositor
// handed over, or `fd:N` for one already open), lets the user crop it, and
// saves the result under the pictures dir. Returns the process exit code.
int mainScreenshot(stl::StringView path);
