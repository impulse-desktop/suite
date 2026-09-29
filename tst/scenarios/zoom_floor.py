"""The editor's zoom stops at 10%: from 50%, three steps out with - give
20% and a fourth 10%; four more change nothing, so one step in with = is
back at 20%, and 0 returns to 50%. The zoom steps are read from the
editor's own account of them; the screen is only compared with the
settled 50% view."""

import re

from session import KEY_EQUAL, KEY_MINUS, KEY_ZERO, Session

with Session("zoom_floor") as s:
    w, h = 1280, 800
    pixels = bytes(b for y in range(h) for x in range(w) for b in ((x * 255) // w, (y * 255) // h, ((x ^ y) & 64) * 3, 255))
    shot = s.capture_file("pattern.shot", w, h, pixels=pixels)
    s.launch(str(shot))
    s.focus()
    base = s.settled("z50")

    def zooms():
        return re.findall(r"im screenshot: zoomed zoom (\d+)", s.client_log())

    for _ in range(3):
        s.tap(KEY_MINUS)
    s.said("zoomed zoom 20")
    s.changed(base, "z20")
    s.tap(KEY_MINUS)
    s.said("zoomed zoom 10")
    for _ in range(4):
        s.tap(KEY_MINUS)
    s.tap(KEY_EQUAL)  # the keys are taken in order, so the four above were before it
    s.said("zoomed zoom 20", 2)
    assert zooms() == ["40", "30", "20", "10", "20"], f"zooming out past 10% moved the zoom: {zooms()}"
    s.tap(KEY_ZERO)
    s.said("reset zoom 50")
    s.same(base, "reset")
    s.close()
    print("OK: the editor's zoom stops at 10%")
