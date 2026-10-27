"""The gallery's zoom popup lies over the thumbnails, not under them."""

from session import Session, near, write_png

with Session("view_combo", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    write_png(pics / "red.png", 64, 48, (255, 0, 0))
    s.launch(str(pics))
    s.focus()
    s.said("showing red.png 64x48")
    s.said("thumbnail red.png")
    # The toolbar's combo ends at the gallery's right edge. Its popup and
    # the first thumbnail then occupy the same pixels.
    s.click(160, 10)
    popup = (80, 25, 110, 120)
    s.wait(lambda: near(s.capture("zoom-popup", region=popup), (255, 0, 0)) == 0, "the zoom popup over the thumbnail")
    # Fit, 25%, 50%, 75%, then 100%: choose the last to prove the popup was open.
    s.click(140, 97)
    s.said("zoom 100")
    s.close()
    print("OK: the gallery's zoom popup draws over its thumbnails")
