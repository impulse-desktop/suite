"""Under a compositor without colour management there is no HDR10
swapchain to be had: the HDR capture is shown in SDR frames, its light
mapped into BT.709 and clipped at SDR white, and the editor closes cleanly.
The HDR scenarios' counterpart, run where they are skipped."""

from session import Session

with Session("no_hdr_surface") as s:
    if "wp_color_manager_v1" in s.globals:
        s.skip("the compositor offers wp_color_manager_v1")
    shot = s.capture_file("good.shot", 64, 48)
    s.launch(str(shot), IM_SHOT_COLOR="1:203")
    s.focus()

    def drawn():
        # the editor's panel and canvas are on screen: many colours in its window
        w, h, px = s.capture("sdr")
        colours = {px[(y * w + x) * 3:(y * w + x) * 3 + 3] for y in range(2, h - 2, 5) for x in range(2, w - 2, 5)}
        return len(colours) > 4

    s.wait(drawn, "the HDR capture shown in SDR frames")
    s.close()
    print("OK: without colour management the HDR editor shows its capture in SDR frames")
