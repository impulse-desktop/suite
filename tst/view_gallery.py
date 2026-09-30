"""A long list: every thumbnail is decoded at the start, the wheel over
the list scrolls it, the selection moving to the end scrolls it there;
a click on a row selects it."""

from session import KEY_END, KEY_HOME, Session, write_png

with Session("view_gallery", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    for i in range(40):
        write_png(pics / f"f{i:02}.png", 8, 8, (i * 6, 255 - i * 6, 128))
    s.launch(str(pics))
    s.focus()
    s.said("listed 40")
    s.said("showing f00.png 8x8")
    # every thumbnail at the start, in the list's order
    for i in range(40):
        s.said(f"thumbnail f{i:02}.png")
    s.tap(KEY_END)
    s.said("selected f39.png")
    s.said("showing f39.png 8x8")
    # the wheel over the list scrolls it (8x8 images make 192 px square
    # rows, 4 px apart)
    s.pointer(120, 300)
    s.scroll(-10)
    s.tap(KEY_HOME)
    s.said("selected f00.png")
    # the third row spans 396..588 px
    s.click(100, 470)
    s.said("selected f02.png")
    s.said("showing f02.png 8x8")
    s.close()
    print("OK: thumbnails follow the rows in view, and a row click selects")
