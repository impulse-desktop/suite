"""Selections in the editor. A crop dragged from its bottom-right corner up
to its top-left is the same rectangle as one dragged the usual way: at the
editor's 50% zoom a 200x150 drag saves a 400x300 PNG. A middle-button drag
pans the canvas and drops the selection, so the next save is the whole
frame."""

import time

from session import BTN_MIDDLE, KEY_ENTER, Session, png_size

with Session("crop_drag") as s:
    shot = s.capture_file("frame.shot", 1280, 800)
    shots = s.artifacts / "shots"

    def open_editor(name):
        s.launch(str(shot), IM_SHOT_DIR=str(shots), IM_SHOT_NAME=name, IM_SHOT_FORMAT="png")
        s.focus()
        s.settled("opened-" + name)

    def save(name):
        s.close(KEY_ENTER)
        saved = shots / f"{name}.png"
        assert saved.is_file() and saved.stat().st_size, f"Enter did not save {name}"
        return png_size(saved)

    open_editor("reversed")
    s.drag(620, 300, 420, 150)
    w, h = save("reversed")
    print(f"reversed crop: {w}x{h}")
    assert 396 <= w <= 404 and 296 <= h <= 304, f"the reversed drag saved {w}x{h}, not 400x300"

    open_editor("panned")
    s.drag(420, 150, 620, 300)
    # the middle button stays down, the pointer moving, until the editor
    # says it took the drag: a slow editor can take a whole press, move and
    # release into fewer frames than a drag needs to be seen
    mx, my = 500, 250
    s.pointer(mx, my)
    time.sleep(0.1)
    s.button(BTN_MIDDLE)
    for i in range(1, 51):
        s.pointer(mx - (i % 10) * 6, my - (i % 10) * 5)
        time.sleep(0.2)
        if "im screenshot: panned" in s.client_log():
            break
    s.button(BTN_MIDDLE, False)
    s.said("panned")
    w, h = save("panned")
    print(f"after the pan: {w}x{h}")
    assert (w, h) == (1280, 800), f"the pan kept a selection: saved {w}x{h}"
    print("OK: a reversed drag crops the same rectangle, a middle drag pans and drops it")
