"""The editor's 90%-of-the-output clamp applies to the height too: on a
600-pixel-tall output at ui scale 3 the chrome alone is taller than 540,
so the editor opens exactly 540 tall."""

from session import Session

with Session("initial_size_short", 1280, 600) as s:
    shot = s.capture_file("frame.shot", 1280, 600)
    s.launch(str(shot), IMGUI_SCALE="3")
    s.focus()
    s.wait(lambda: s.size()[1] == 600 * 9 // 10, "the clamped height")
    s.close()
    print("OK: the editor's height is clamped to 90% of the output")
