"""The real desktop configuration: a 3840x2160 capture on an output of
that size, at ui scale 2.5, becomes a 1920x1080 viewport at 50%, alongside
a 500px panel and 20px spacing."""

from session import Session

with Session("initial_size_scaled", 3840, 2160) as s:
    shot = s.capture_file("frame.shot", 3840, 2160)
    s.launch(str(shot), IM_SCALE="2.5")
    s.focus()
    s.wait(lambda: s.size() == (2440, 1080), "the scaled size")
    s.close()
    print("OK: a scaled editor opens at 2440x1080 for a 4K capture")
