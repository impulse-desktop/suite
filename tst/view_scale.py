"""The default ui scale is 2: the window asks
for twice the size and is held to 90% of the output, the list's gaps are
twice as wide."""

from session import Session, write_png

with Session("view_scale", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    write_png(pics / "a.png", 64, 48, (255, 0, 0))
    write_png(pics / "b.png", 64, 48, (0, 255, 0))
    write_png(pics / "c.png", 64, 48, (0, 0, 255))
    s.launch(str(pics), unset=("IM_SCALE",))
    s.focus()
    w, h = s.width * 9 // 10, s.height * 9 // 10
    # the tool's own present at the clamped size, not the compositor's
    # rect: a click into a window still being re-presented lands in a
    # pointer leave (see the sway race in the fixture's history)
    s.said(f"presenting {w}x{h}")
    assert s.size() == (w, h), f"the window was not held to 90% of the output ({s.size()})"
    s.said("showing a.png 64x48")
    s.settled("scaled")
    # the list is 230 px wide, its rows 230x173 one against the next: the
    # second row, b.png, spans 198..371 px, the list 25 px down for the
    # toolbar and the spacing
    s.click(120, 312)
    s.said("selected b.png")
    s.close()
    print("OK: the ui scale sizes the window and the gallery")
