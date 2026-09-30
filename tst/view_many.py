"""Three hundred files: every thumbnail is decoded at the start, and
Down held walks the whole list by the client's own key repeat."""

from session import KEY_DOWN, Session, write_png

with Session("view_many", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    for i in range(300):
        write_png(pics / f"f{i:03}.png", 8, 8, (i % 256, 255 - i % 256, 64))
    s.launch(str(pics))
    s.focus()
    s.said("listed 300")
    s.said("showing f000.png 8x8")
    # 25 repeats a second after 600 ms, one per frame at most: the end of
    # the list within the hold with room for a slow renderer (Alpine got
    # to f286 in 16 s)
    s.tap(KEY_DOWN, hold=20.0)
    s.said("selected f299.png")
    s.said("showing f299.png 8x8")
    s.said("thumbnail f299.png")
    s.close()
    print("OK: a long list walks by key repeat, and old thumbnails are dropped")
