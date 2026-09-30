"""A long list: thumbnails are decoded for the rows in view only, more as
the list scrolls, by the wheel over the gallery or by the selection
moving to the end; a click on a row selects it."""

import time

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
    # 8x8 images make 192 px square rows, 4 px apart: a 700 px window shows
    # three and part of the fourth, and two more are decoded ahead
    for i in range(6):
        s.said(f"thumbnail f{i:02}.png")
    time.sleep(1.0)
    assert "thumbnail f20.png" not in s.client_log(), "a thumbnail far below the view was decoded"
    s.tap(KEY_END)
    s.said("selected f39.png")
    s.said("showing f39.png 8x8")
    s.said("thumbnail f39.png")
    # the wheel over the gallery scrolls it up into rows not yet seen
    s.pointer(120, 300)
    s.scroll(-10)
    s.wait(lambda: any(f"thumbnail f{i:02}.png" in s.client_log() for i in range(20, 33)), "thumbnails of the rows scrolled into view")
    s.tap(KEY_HOME)
    s.said("selected f00.png")
    # the third row spans 396..588 px
    s.click(100, 470)
    s.said("selected f02.png")
    s.said("showing f02.png 8x8")
    s.close()
    print("OK: thumbnails follow the rows in view, and a row click selects")
