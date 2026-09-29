#pragma once

// A condition that holds every time, kept at its call site as a value
// rather than a branch: ImGui::Begin and BeginChild on a window that
// cannot collapse say it is shown each frame, and drawing into one that
// is not would be legal anyway, only wasted. The one branch that never
// runs lives here, not once per window.
void checkTrue(bool value);
