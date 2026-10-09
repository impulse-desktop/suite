"""The editor's panel with the mouse: a click near the far end of the zoom
slider zooms the canvas in, the Reset button puts the view back exactly
as it opened, and Ctrl+click on the slider makes it a text field."""

from session import KEY_LEFTCTRL, Session

with Session("panel_mouse") as s:
    w, h = 1280, 800
    pixels = bytes(b for y in range(h) for x in range(w) for b in ((x * 255) // w, (y * 255) // h, ((x ^ y) & 64) * 3, 255))
    shot = s.capture_file("pattern.shot", w, h, pixels=pixels)
    s.launch(str(shot))
    s.focus()
    # the baseline is taken with the pointer parked on the canvas, where
    # the scenario leaves it after Reset: where a compositor puts the
    # pointer at start is its own business, and one had it over Save,
    # hovered, in the frame the reset was then held to
    s.pointer(400, 300)
    opened = s.settled("opened")
    # the panel's first rows: the zoom slider under its label, 200 px
    # across, then Save and Reset side by side
    s.click(185, 27)
    s.changed(opened, "zoomed")
    s.click(151, 54)
    s.pointer(400, 300)
    s.same(opened, "reset")
    # Ctrl+click turns the slider into a text field, over which ImGui asks
    # for the text cursor
    s.key(KEY_LEFTCTRL, 1)
    s.click(100, 27)
    s.key(KEY_LEFTCTRL, 0)
    # the field takes the slider's place, a few hundred pixels' worth
    s.changed(opened, "editing", threshold=200)
    s.pointer(120, 27)
    s.settled("hovering")
    s.close()
    print("OK: the zoom slider zooms, Reset restores the view, and Ctrl+click edits the zoom")
