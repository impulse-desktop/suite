"""Images larger than a thumbnail: each is shrunk to one, the shown one is
fitted to the canvas; a file the list does not take (readme.txt) is not
listed; leaving while decodes are still queued is fine."""

from session import Session, near, write_png

with Session("view_big", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    for i in range(12):
        write_png(pics / f"big{i:02}.png", 1600, 1200, (0, 0, 255))
    (pics / "readme.txt").write_text("not an image\n")
    s.launch(str(pics))
    s.focus()
    s.said("listed 12")
    s.said("showing big00.png 1600x1200")
    s.said("thumbnail big00.png")
    r = s.window()["rect"]
    canvas = (210, 0, r["width"] - 420, r["height"])
    # fitted: the canvas between the panels is 600x700, the image lands
    # at 600x450 (the region starts 10 px in: 590 columns of it)
    s.wait(lambda: near(s.capture("fitted", region=canvas), (0, 0, 255)) >= 250000, "the big image fitted to the canvas")
    s.close()
    print("OK: big images are shrunk to thumbnails and fitted to the canvas")
