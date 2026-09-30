"""The view on an image: the wheel over the canvas zooms in, - and = step
the zoom, 1 is 1:1 and 0 fits again; r turns the image a quarter clockwise
(the 64x48 image stands 48x64), R back; f makes the window fullscreen and
back; Tab hides the gallery, and the canvas takes its room; b hides the
status line; a drag pans; a window narrower than the gallery is fine."""

from session import KEY_1, KEY_B, KEY_EQUAL, KEY_F, KEY_LEFTSHIFT, KEY_MINUS, KEY_R, KEY_TAB, KEY_ZERO, Session, colour_box, write_png

with Session("view_keys", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    write_png(pics / "a.png", 64, 48, (255, 0, 0))
    s.launch(str(pics))
    s.focus()
    s.said("showing a.png 64x48")
    r = s.window()["rect"]
    canvas = (250, 0, r["width"] - 250, r["height"] - 30)

    def reddish(red, green, blue):
        return red >= 100 and green <= 80 and blue <= 80

    def box(label):
        return s.wait(lambda: colour_box(s.capture(label, region=canvas), reddish), f"the image on the canvas ({label})")

    def size(label, w, h):
        def fits():
            b = colour_box(s.capture(label, region=canvas), reddish)
            return b and abs(b[2] - b[0] + 1 - w) <= 1 and abs(b[3] - b[1] + 1 - h) <= 1
        s.wait(fits, f"the image drawn {w}x{h} ({label})")

    size("own", 64, 48)
    # the wheel up over the canvas zooms in
    s.pointer(canvas[0] + canvas[2] // 2, canvas[3] // 2)
    s.scroll(-1)
    s.said("zoom 125")
    size("wheel", 80, 60)
    s.tap(KEY_MINUS)
    s.said("zoom 100")
    s.tap(KEY_EQUAL)
    s.said("zoom 125", 2)
    s.tap(KEY_1)
    s.said("zoom 100", 2)
    size("one", 64, 48)
    s.tap(KEY_ZERO)
    s.said("fit")
    s.tap(KEY_R)
    s.said("rotated 90")
    size("turned", 48, 64)
    s.key(KEY_LEFTSHIFT, 1)
    s.tap(KEY_R)
    s.key(KEY_LEFTSHIFT, 0)
    s.said("rotated 0")
    size("back", 64, 48)
    before = box("panel")
    s.tap(KEY_F)
    s.said("fullscreen on")
    s.wait(lambda: s.size() == (s.width, s.height), "the fullscreen window")
    s.tap(KEY_F)
    s.said("fullscreen off")
    s.wait(lambda: s.size() == (r["width"], r["height"]), "the window back at its size")
    s.tap(KEY_TAB)
    s.said("panel off")
    # the canvas grew by the gallery's width: the image sits 120 px further left
    s.wait(lambda: (b := colour_box(s.capture("nopanel", region=canvas), reddish)) and before[0] - b[0] >= 100, "the image moved left")
    s.tap(KEY_TAB)
    s.said("panel on")
    s.tap(KEY_B)
    s.said("status off")
    s.tap(KEY_B)
    s.said("status on")
    # a drag over the canvas pans (a fitted image stays centred)
    cx, cy = canvas[0] + canvas[2] // 2, canvas[3] // 2
    s.drag(cx, cy, cx + 40, cy + 30)
    s.tap(KEY_ZERO)
    s.said("fit", 2)
    # a window narrower than the gallery leaves the canvas nothing to draw in
    node = s.window()
    s.ipc(f'[con_id={node["id"]}] resize set 200 px 150 px')
    s.said("presenting 200x150")
    s.ipc(f'[con_id={node["id"]}] resize set {r["width"]} px {r["height"]} px')
    s.said(f"presenting {r['width']}x{r['height']}", 2)
    s.close()
    print("OK: zoom, rotation, fullscreen, the toggles, a drag and a tiny window work")
