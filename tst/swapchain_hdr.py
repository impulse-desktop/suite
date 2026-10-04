"""The HDR editor's swapchain going stale under it: a rebuild after an
acquire or a present reports the swapchain out of date (IM_CHAOS) makes the
PQ swapchain again, with the compositor's frame buffers; the editor is
drawn and closes cleanly."""

from session import Session

with Session("swapchain_hdr") as s:
    s.require("wp_color_manager_v1")
    shot = s.capture_file("good.shot", 64, 48)

    def drawn():
        # the editor's panel and canvas are on screen: many colours in its window
        w, h, px = s.capture("stale")
        colours = {px[(y * w + x) * 3:(y * w + x) * 3 + 3] for y in range(2, h - 2, 5) for x in range(2, w - 2, 5)}
        return len(colours) > 4

    for fault in ("swapchain=0", "swapchain=3", "swapchain-suboptimal=3"):
        s.launch(str(shot), IM_SHOT_COLOR="1:203", IM_CHAOS=fault)
        s.focus()
        s.said("surface HDR10 PQ")
        s.wait(drawn, f"{fault}: the HDR editor drawn after the rebuild")
        s.close()
    print("OK: a stale swapchain under the HDR editor is rebuilt in its mode")
