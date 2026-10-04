"""The HDR editor composes its frames into a PQ swapchain sized to the
window. Opened at ui scale 3 it is clamped to 90% of the output right after
mapping, which rebuilds the swapchain in the same mode: the resized window
still shows the captured frame, not a black or stale one."""

from session import Session

with Session("resize_hdr") as s:
    s.require("wp_color_manager_v1")
    w, h = 1280, 800
    # PQ codes that vary across the frame: many colours once drawn
    pixels = bytes(b for y in range(h) for x in range(w) for b in ((x * 255) // w, (y * 255) // h, ((x ^ y) & 64) * 3, 255))
    shot = s.capture_file("frame.shot", w, h, pixels=pixels)
    s.launch(str(shot), IM_SHOT_COLOR="1:203", IM_SCALE="3")
    s.focus()
    s.said("surface HDR10 PQ")
    s.wait(lambda: s.size()[0] == 1280 * 9 // 10, "the clamped size")

    def drawn():
        # the right half of the window is the canvas: the frame's colours
        vw, vh, px = s.capture("resized")
        colours = {px[(y * vw + x) * 3:(y * vw + x) * 3 + 3] for y in range(vh // 4, vh * 3 // 4, 7) for x in range(vw * 3 // 4, vw - 4, 7)}
        return len(colours) > 8

    s.wait(drawn, "the resized HDR editor showing the frame")
    s.close()
    print("OK: the HDR editor's frames follow a resize")
