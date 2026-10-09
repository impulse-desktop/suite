"""SVG, drawn by lunasvg: fitted, a 40x30 drawing fills the canvas, past
its own size; the raster is drawn at the zoom's level, scales in steps of
sqrt 2, and drawn again only when the level changes: 1:1 is level 0,
40x30, 125% level 1, 57x43, 156% level 2, 80x60; a byte order mark before
the markup is fine; a drawing of no size cannot be shown."""

from session import KEY_1, KEY_EQUAL, KEY_RIGHT, Session, colour_box

DRAWING = '<svg xmlns="http://www.w3.org/2000/svg" width="40" height="30"><rect width="40" height="30" fill="#00ff00"/></svg>\n'

with Session("view_svg", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    (pics / "a.svg").write_text(DRAWING)
    (pics / "b.svg").write_bytes(b"\xef\xbb\xbf" + DRAWING.encode())
    (pics / "c.svg").write_text('<svg xmlns="http://www.w3.org/2000/svg"/>\n')
    s.launch(str(pics / "a.svg"), str(pics / "b.svg"), str(pics / "c.svg"))
    s.focus()
    s.said("listed 3")
    s.said("showing a.svg 40x30")
    s.said("drawn a.svg level ")
    s.said("thumbnail a.svg")
    r = s.window()["rect"]
    canvas = (220, 0, r["width"] - 420, r["height"])

    def greenish(red, green, blue):
        return green >= 160 and red <= 80 and blue <= 80

    def size(label, w, h):
        def fits():
            b = colour_box(s.capture(label, region=canvas), greenish)
            return b and abs(b[2] - b[0] + 1 - w) <= 1 and abs(b[3] - b[1] + 1 - h) <= 1
        s.wait(fits, f"the drawing shown {w}x{h} ({label})")

    def filled():
        b = colour_box(s.capture("fitted", region=canvas), greenish)
        return b and b[2] - b[0] + 1 >= 400
    s.wait(filled, "the drawing fitted past its own size")

    s.tap(KEY_1)
    s.said("zoom 100")
    s.said("drawn a.svg level 0 40x30")
    size("one", 40, 30)
    s.tap(KEY_EQUAL)
    s.said("zoom 125")
    s.said("drawn a.svg level 1 57x43")
    size("125", 50, 38)
    s.tap(KEY_EQUAL)
    s.said("zoom 156")
    s.said("drawn a.svg level 2 80x60")
    size("156", 63, 47)

    s.tap(KEY_RIGHT)
    s.said("showing b.svg 40x30")
    s.said("drawn b.svg level 2 80x60")
    size("bom", 63, 47)

    s.tap(KEY_RIGHT)
    s.said("cannot show c.svg: the SVG has no size")
    log = s.client_log()
    assert log.count("im view: drawn a.svg level 2 80x60") == 1, "the drawing was drawn again at the same level"
    s.close()
    print("OK: SVG is drawn at the zoom's level and fitted past its size")
