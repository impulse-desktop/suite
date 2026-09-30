"""IMGUI_SCALE=2, as a hidpi compositor hands it down: the window asks
for twice the size and is held to 90% of the output, the gallery's rows
are twice as tall."""

from session import Session, write_png

with Session("view_scale", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    write_png(pics / "a.png", 64, 48, (255, 0, 0))
    write_png(pics / "b.png", 64, 48, (0, 255, 0))
    write_png(pics / "c.png", 64, 48, (0, 0, 255))
    s.launch(str(pics), IMGUI_SCALE="2")
    s.focus()
    s.wait(lambda: s.size() == (s.width * 9 // 10, s.height * 9 // 10), "the window held to 90% of the output")
    s.said("showing a.png 64x48")
    # rows are 208 px tall now: the second row's middle is c.png's neighbour b.png
    s.click(120, 312)
    s.said("selected b.png")
    s.close()
    print("OK: the ui scale sizes the window and the gallery")
