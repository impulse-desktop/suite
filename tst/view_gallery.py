"""A long list: visible thumbnails load on demand, the wheel over
the list scrolls it and the rows it brings in load theirs; a click on a
row selects it."""

from session import Session, write_png

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
    s.pointer(120, 300)
    s.scroll(10)
    s.wait(lambda: any(f"im view: thumbnail f{i:02}.png" in s.client_log() for i in range(5, 40)), "a thumbnail further down the list")
    s.scroll(-40)
    for i in range(1, 3):
        assert s.client_log().count(f"im view: loading thumbnail f{i:02}.png") == 1, "cached thumbnail decoded again"
    assert s.client_log().count("im view: thumbnail f00.png") == 1, "cached thumbnail uploaded again"
    # the rows are 200 px squares one against the next: the third spans
    # 425..625 px, the list 25 px down for the toolbar and the spacing
    s.click(100, 470)
    s.said("selected f02.png")
    s.said("showing f02.png 8x8")
    s.close()
    print("OK: thumbnails follow the rows in view, and a row click selects")
