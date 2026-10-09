"""`imview DIR` lists the directory's images by name, shows the first and
a thumbnail of each in the gallery; a click on a gallery row selects it.
The canvas shows the image at its own size."""

from session import Session, near, write_png

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
    # the canvas sits right of the list, a fifth of the width (200 px); a
    # thumbnail bulging towards the pointer reaches 215 px, so the region
    # starts past that, and stops where the info panel would be
    r = s.window()["rect"]
    canvas = (220, 0, r["width"] - 420, r["height"])

    def shows(rgb, count, label):
        s.wait(lambda: near(s.capture(label, region=canvas), rgb) >= count, f"the canvas showing {label}")

    shows((255, 0, 0), 2900, "a")
    # the list's rows are as wide as the list (200 px) and as tall as the
    # image's proportion, one against the next: a.png and b.png 150 px,
    # c.png 200 px; the list 25 px down for the toolbar and the spacing
    s.click(100, 250)
    s.said("selected b.png")
    s.said("showing b.png 80x60")
    shows((0, 255, 0), 4500, "b")
    s.click(100, 400)
    s.said("selected c.png")
    s.said("showing c.png 32x32")
    shows((0, 0, 255), 900, "c")
    s.click(100, 100)
    s.said("selected a.png", 2)
    shows((255, 0, 0), 2900, "a-again")
    s.close()
    print("OK: the viewer lists a directory, shows its images and selects them by click")
