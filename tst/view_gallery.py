"""A long list: visible thumbnails load on demand, the wheel over
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
    for i in range(3):
        s.said(f"thumbnail f{i:02}.png")
    assert "thumbnail f20.png" not in s.client_log(), "offscreen thumbnail loaded at startup"
    s.tap(KEY_END)
    s.said("selected f39.png")
    s.said("showing f39.png 8x8")
    s.said("thumbnail f39.png")
    # the wheel over the list scrolls it (8x8 images make 192 px square
    # rows, 4 px apart)
    s.pointer(120, 300)
    s.scroll(-10)
    s.tap(KEY_HOME)
    s.said("selected f00.png")
    s.said("showing f00.png", 2)
    for i in range(1, 3):
        assert s.client_log().count(f"im view: loading thumbnail f{i:02}.png") == 1, "cached thumbnail decoded again"
    assert s.client_log().count("im view: thumbnail f00.png") == 1, "cached thumbnail uploaded again"
    # the rows are 200 px squares 6 px apart: the third spans 418..618 px,
    # the list 6 px down for the padding
    s.click(100, 470)
    s.said("selected f02.png")
    s.said("showing f02.png 8x8")
    s.close()
    print("OK: thumbnails follow the rows in view, and a row click selects")
