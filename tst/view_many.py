"""Three hundred files: visible thumbnails load on demand, and
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
    # one step a frame at most: Down stays held until the end of the list,
    # however slow the renderer is
    s.key(KEY_DOWN, 1)
    s.wait(lambda: "im view: selected f299.png" in s.client_log(), "the walk to f299.png", timeout=60)
    s.key(KEY_DOWN, 0)
    s.said("showing f299.png 8x8")
    s.said("thumbnail f299.png")
    s.close()
    print("OK: a long list walks by key repeat and loads the final thumbnail")
