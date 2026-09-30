"""`imview DIR` lists the directory's images by name, shows the first and
a thumbnail of each in the gallery, and the keys walk the list: Right,
j and Space forward, Left, k and Backspace back, Home and End to the
ends; a click on a gallery row selects it. The canvas shows the image at
its own size."""

from session import KEY_BACKSPACE, KEY_END, KEY_HOME, KEY_J, KEY_K, KEY_LEFT, KEY_RIGHT, KEY_SPACE, Session, near, write_png

with Session("view_open", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    write_png(pics / "a.png", 64, 48, (255, 0, 0))
    write_png(pics / "b.png", 80, 60, (0, 255, 0))
    write_png(pics / "c.png", 32, 32, (0, 0, 255))
    s.launch(str(pics))
    s.focus()
    s.said("listed 3")
    s.said("selected a.png")
    s.said("showing a.png 64x48")
    for name in ("a.png", "b.png", "c.png"):
        s.said(f"thumbnail {name}")
    # the canvas is right of the 240 px gallery, above the status line
    r = s.window()["rect"]
    canvas = (250, 0, r["width"] - 250, r["height"] - 30)

    def shows(rgb, count, label):
        s.wait(lambda: near(s.capture(label, region=canvas), rgb) >= count, f"the canvas showing {label}")

    shows((255, 0, 0), 2900, "a")
    s.tap(KEY_RIGHT)
    s.said("selected b.png")
    s.said("showing b.png 80x60")
    shows((0, 255, 0), 4500, "b")
    s.tap(KEY_END)
    s.said("selected c.png")
    s.said("showing c.png 32x32")
    shows((0, 0, 255), 900, "c")
    s.tap(KEY_HOME)
    s.said("selected a.png", 2)
    s.tap(KEY_J)
    s.said("selected b.png", 2)
    s.tap(KEY_K)
    s.said("selected a.png", 3)
    s.tap(KEY_SPACE)
    s.said("selected b.png", 3)
    s.tap(KEY_BACKSPACE)
    s.said("selected a.png", 4)
    s.tap(KEY_LEFT)  # already first: stays
    # the gallery's rows are 104 px tall; the third row is c.png
    s.click(120, 260)
    s.said("selected c.png", 2)
    shows((0, 0, 255), 900, "c-clicked")
    s.close()
    print("OK: the viewer lists a directory, shows its images and walks them by key and click")
