"""Colour metadata with a partial display volume (a white level followed by
a minimum but no peak, or no frame average) is read as no volume at all:
the white level is still the one it names, so the image looks exactly as
with the white level alone, not as if the stray minimum were its white."""

from session import Session

with Session("color_partial") as s:
    s.require("wp_color_manager_v1")
    shot = s.capture_file("good.shot", 64, 48)

    def look(colour, label):
        """The mean colour of the frame's pixels in the editor's window:
        those well away from the panel's greys."""
        s.launch(str(shot), IMWAY_SHOT_COLOR=colour)
        s.focus()
        s.said("surface HDR10 PQ")

        def colourful(capture):
            w, h, px = capture
            found = [px[(y * w + x) * 3:(y * w + x) * 3 + 3] for y in range(h) for x in range(w // 4, w)]
            found = [p for p in found if max(p) - min(p) > 40]
            return found

        frame = s.settled(label)
        found = colourful(frame)
        assert len(found) >= 200, f"{colour}: the frame is not on screen"
        s.close()
        return tuple(sum(p[c] for p in found) // len(found) for c in range(3))

    white = look("1:203", "white")
    for partial in ("1:203:0.1", "1:203:0.1:1000"):
        seen = look(partial, "partial")
        print(f"{partial}: {seen}, with the white level alone {white}")
        assert all(abs(a - b) <= 12 for a, b in zip(white, seen)), f"{partial}: the frame is {seen}, with the white level alone {white}"
    print("OK: a partial display volume leaves the white level as named")
