"""The editor from the keyboard: zoom in, out and reset with the = - 0
shortcuts, the wheel over the canvas, keys and buttons it has no use for
leaving it alone, then Enter saves the crop as the configured PNG and the
tool exits; a second capture closed without a save writes nothing."""

import time

from session import BTN_MIDDLE, BTN_RIGHT, KEY_A, KEY_ENTER, KEY_EQUAL, KEY_MINUS, KEY_ZERO, Session, png_size

with Session("editor_keys") as s:
    w, h = 640, 480
    # a pattern, so a change of zoom moves pixels
    pixels = bytes(b for y in range(h) for x in range(w) for b in ((x * 255) // w, (y * 255) // h, ((x ^ y) & 32) * 7, 255))
    shot = s.capture_file("pattern.shot", w, h, pixels=pixels)
    shots = s.artifacts / "shots"

    s.launch(str(shot), IM_SHOT_DIR=str(shots), IM_SHOT_NAME="crop", IM_SHOT_FORMAT="png")
    s.focus()
    vw, vh = s.size()
    before = s.settled("before")
    s.tap(KEY_EQUAL)
    s.tap(KEY_EQUAL)
    s.changed(before, "zoomed")
    s.tap(KEY_MINUS)
    s.tap(KEY_ZERO)
    s.same(before, "unzoomed")

    # the wheel zooms the canvas the pointer is over: the scroll travels out
    # as a wl_pointer axis and comes back as an ImGui wheel
    s.pointer(vw * 3 // 4, vh // 2)
    reset = s.settled("reset")
    s.scroll(-1)
    s.scroll(-1)
    s.changed(reset, "wheeled")
    s.tap(KEY_ZERO)
    s.same(reset, "unwheeled")

    # keys the editor has no use for leave it alone: the punctuation the
    # input path names one by one, two letters, one held until it repeats,
    # and the mouse buttons that are not the primary
    idle = s.settled("idle")
    for code in (40, 51, 52, 53, 39, 26, 43, 27, 41, 57, 30, 44):
        s.tap(code)
    s.tap(KEY_A, hold=1.0)
    for code in (BTN_RIGHT, BTN_MIDDLE, 275, 276, 277):
        s.button(code)
        s.button(code, False)
    assert s.windows(), "a stray key closed the editor"
    s.same(idle, "swept", tolerance=500)

    s.close(KEY_ENTER)
    crop = shots / "crop.png"
    assert crop.is_file() and crop.stat().st_size, "Enter did not save the crop"
    assert png_size(crop) == (w, h), f"the crop is {png_size(crop)}, not the whole {w}x{h} capture"

    s.launch(str(shot), IM_SHOT_DIR=str(shots), IM_SHOT_NAME="discard", IM_SHOT_FORMAT="png")
    s.focus()
    time.sleep(0.5)
    s.close()
    assert not (shots / "discard.png").exists(), "closing saved a file"
    print("OK: the editor zooms from the keyboard, Enter saves the PNG crop, closing discards")
