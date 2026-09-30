"""The editor's selection at its edges. A drag that leaves the image past
its bottom-right corner selects up to the image's edge, and the saved PNG
runs from the drag's start to the corner. A drag straight across, no
taller than a pixel, selects nothing, and the whole frame is saved."""

from session import KEY_ENTER, Session, png_size

with Session("crop_edges") as s:
    shot = s.capture_file("frame.shot", 1280, 800)
    shots = s.artifacts / "shots"
    # the canvas starts past the 200px panel and 8px of spacing, at 50%
    canvas_x = 208

    def capture(name, dx0, dy0, dx1, dy1):
        s.launch(str(shot), IM_SHOT_DIR=str(shots), IM_SHOT_NAME=name, IM_SHOT_FORMAT="png")
        s.focus()
        s.settled("opened-" + name)
        s.drag(canvas_x + dx0, dy0, canvas_x + dx1, dy1)
        s.close(KEY_ENTER)
        saved = shots / f"{name}.png"
        assert saved.is_file() and saved.stat().st_size, f"{name}: Enter did not save"
        return png_size(saved)

    # from (100,50) on the canvas, image (200,100), out past the corner
    w, h = capture("corner", 100, 50, 700, 450)
    assert 1079 <= w <= 1081 and 699 <= h <= 701, f"the drag past the corner saved {w}x{h}, want 1080x700"
    # straight across: no height, no selection
    w, h = capture("across", 100, 200, 400, 200)
    assert (w, h) == (1280, 800), f"the flat drag saved {w}x{h}, want the whole 1280x800 frame"
    print("OK: a drag past the image stops at its edge, a flat drag selects the whole frame")
