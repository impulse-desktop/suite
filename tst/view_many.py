"""Three hundred files: the list holds them all, and the visible
thumbnails load on demand."""

from session import Session, write_png

with Session("view_many", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    for i in range(300):
        write_png(pics / f"f{i:03}.png", 8, 8, (i % 256, 255 - i % 256, 64))
    s.launch(str(pics))
    s.focus()
    s.said("listed 300")
    s.said("showing f000.png 8x8")
    s.said("thumbnail f000.png")
    assert "thumbnail f299.png" not in s.client_log(), "offscreen thumbnail loaded at startup"
    s.close()
    print("OK: a long list loads and its visible thumbnails come on demand")
