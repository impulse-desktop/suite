"""The editor opens at the image's 50% size plus its chrome, but never
wider or taller than 90% of the output: at ui scale 3 the panel alone
would push the window past the 1280-pixel output, so it opens clamped."""

from session import Session

with Session("initial_size_clamped") as s:
    shot = s.capture_file("frame.shot", 1280, 800)
    s.launch(str(shot), IMGUI_SCALE="3")
    s.focus()
    s.wait(lambda: s.size()[0] == 1280 * 9 // 10 and s.size()[1] <= 800 * 9 // 10, "the clamped size")
    s.close()
    print("OK: the editor opens clamped to 90% of the output")
