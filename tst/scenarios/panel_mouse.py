"""The editor's panel with the mouse: a click near the far end of the zoom
slider zooms the canvas in, and the Reset button puts the view back
exactly as it opened."""

from session import Session

with Session("panel_mouse") as s:
    w, h = 1280, 800
    pixels = bytes(b for y in range(h) for x in range(w) for b in ((x * 255) // w, (y * 255) // h, ((x ^ y) & 64) * 3, 255))
    shot = s.capture_file("pattern.shot", w, h, pixels=pixels)
    s.launch(str(shot))
    s.focus()
    opened = s.settled("opened")
    # the panel's first rows: the zoom slider under its label, 200 px
    # across, then Save and Reset side by side
    s.click(185, 27)
    s.changed(opened, "zoomed")
    s.click(151, 54)
    s.pointer(400, 300)
    s.same(opened, "reset")
    s.close()
    print("OK: the zoom slider zooms and Reset restores the view")
