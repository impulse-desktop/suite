"""The editor from the keypad: keypad + zooms in, keypad - out, keypad 0
resets, the wheel turned toward the user zooms out, and keypad Enter
saves. Every step is judged against the settled 50% view and the editor's
own account of the zoom it stepped to."""

from session import Session

KEY_NUMLOCK, KEY_KPMINUS, KEY_KPPLUS, KEY_KP0, KEY_KPENTER = 69, 74, 78, 82, 96

with Session("keypad") as s:
    w, h = 1280, 800
    pixels = bytes(b for y in range(h) for x in range(w) for b in ((x * 255) // w, (y * 255) // h, ((x ^ y) & 64) * 3, 255))
    shot = s.capture_file("pattern.shot", w, h, pixels=pixels)
    shots = s.artifacts / "shots"
    s.launch(str(shot), IM_SHOT_DIR=str(shots), IM_SHOT_NAME="keypad", IM_SHOT_FORMAT="png")
    s.focus()
    vw, vh = s.size()
    base = s.settled("base")
    s.tap(KEY_NUMLOCK)  # keypad 0 is Insert without it
    s.tap(KEY_KPPLUS)
    s.said("zoomed zoom 60")
    s.changed(base, "in")
    s.tap(KEY_KP0)
    s.said("reset zoom 50")
    s.same(base, "reset")
    s.tap(KEY_KPMINUS)
    s.said("zoomed zoom 40")
    s.changed(base, "out")
    s.tap(KEY_KP0)
    s.said("reset zoom 50", 2)
    s.same(base, "reset2")
    # the wheel zooms the canvas the pointer is over
    s.pointer(vw * 3 // 4, vh // 2)
    s.pointer(vw * 3 // 4 + 1, vh // 2)
    s.scroll(1)
    s.changed(base, "wheel")
    s.close(KEY_KPENTER)
    assert (shots / "keypad.png").is_file(), "keypad Enter did not save"
    print("OK: the editor zooms and saves from the keypad and zooms out with the wheel")
