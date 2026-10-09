"""The SDR white the capture was taken at comes on the command line, and
the editor shows it at the frame's white: the same PQ codes taken at a
brighter white are darker on screen."""

from session import Session

with Session("shot_white") as s:
    # red and blue at about 203 nits, the default white
    shot = s.capture_file("mid.shot", 64, 48, pixel=(148, 0, 148, 255))

    def look(label, *args):
        """The mean colour of the frame's pixels in the editor's window:
        those well away from the panel's greys."""
        s.launch(str(shot), *args, IM_SHOT_COLOR="1")
        s.focus()
        w, h, px = s.settled(label)
        found = [px[(y * w + x) * 3:(y * w + x) * 3 + 3] for y in range(h) for x in range(w // 4, w)]
        found = [p for p in found if max(p) - min(p) > 40]
        assert len(found) >= 200, f"{label}: the frame is not on screen"
        s.close()
        return tuple(sum(p[c] for p in found) // len(found) for c in range(3))

    taken = look("default")
    brighter = look("brighter", "--white", "812")
    print(f"taken at the default white {taken}, at 812 nits {brighter}")
    assert sum(brighter) + 60 < sum(taken), f"taken at 812 nits the frame is {brighter}, at the default white {taken}"
    print("OK: the capture's white comes from the command line")
