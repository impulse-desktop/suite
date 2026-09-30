"""`imview FILE` opens the file among its directory's images, selected;
a file whose name says nothing about its format is listed all the same,
first, and decoded by its bytes; several files make the list themselves,
in the given order; q leaves the viewer."""

from session import KEY_LEFT, KEY_Q, Session, near, write_png

with Session("view_file", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    write_png(pics / "a.png", 64, 48, (255, 0, 0))
    write_png(pics / "b.png", 80, 60, (0, 255, 0))
    write_png(pics / "c.png", 32, 32, (0, 0, 255))
    write_png(pics / "notes", 40, 40, (255, 255, 0))
    r = None

    def shows(rgb, count, label):
        canvas = (250, 0, r["width"] - 250, r["height"] - 30)
        s.wait(lambda: near(s.capture(label, region=canvas), rgb) >= count, f"the canvas showing {label}")

    s.launch(str(pics / "b.png"))
    s.focus()
    r = s.window()["rect"]
    s.said("listed 3")
    s.said("selected b.png")
    s.said("showing b.png 80x60")
    shows((0, 255, 0), 4500, "b")
    s.tap(KEY_LEFT)
    s.said("selected a.png")
    s.said("showing a.png 64x48")
    s.close(KEY_Q)

    s.launch(str(pics / "notes"))
    s.focus()
    r = s.window()["rect"]
    s.said("listed 4")
    s.said("selected notes")
    s.said("showing notes 40x40")
    shows((255, 255, 0), 1500, "notes")
    s.close()

    s.launch(str(pics / "c.png"), str(pics / "a.png"))
    s.focus()
    r = s.window()["rect"]
    s.said("listed 2")
    s.said("selected c.png")
    s.said("showing c.png 32x32")
    shows((0, 0, 255), 900, "c")
    s.close()
    print("OK: a file opens among its directory, an unnamed format by its bytes, several files as given")
