# TextEditor

Johan Goossens's syntax-highlighting text editor for Dear ImGui, the
widget behind `im edit`: a document of lines of codepoints with overlays
for colouring, bracket matching, word wrap, folding and a minimap; many
cursors, transactional undo, find and replace. Taken as is from
[goossens/ImGuiColorTextEdit](https://github.com/goossens/ImGuiColorTextEdit)
at commit 133614b0d5e1008527a26f46a93fdb1d751ca115 (2026-09-26), MIT
license in LICENSE; `TextEditor.cpp` and `TextEditor.h` only, the
TextDiff widget and the examples are left out. One local change, marked
`[suite]` in the sources: the caret's blink asks for its next frame
through the frame timer of the suite's ImGui, whose frames come on
request, as that ImGui's own InputText does.

Its public API moves between releases, so a newer copy is taken whole,
and `edit.cpp` adjusted to it. It wants `imgui_internal.h` and C++17,
and keeps codepoints in `ImWchar`, which the build makes 32-bit
(`IMGUI_USE_WCHAR32`) so nothing beyond the Basic Multilingual Plane is
lost in a file.
