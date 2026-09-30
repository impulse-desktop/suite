"""The HDR editor builds its linear-light scene target after its window
and swapchain; the device refuses the target's first object, its render
pass (IM_CHAOS vulkan-at=scene-pass). The editor reports the refusal and
exits 1, tearing down the part of the target it had not built without
faulting on it; the next editor opens as usual."""

from session import Session

with Session("linear_hdr_fault") as s:
    s.require("wp_color_manager_v1")
    shot = s.capture_file("good.shot", 64, 48)
    code, log = s.run(str(shot), IM_SHOT_COLOR="1:203", IM_CHAOS="vulkan-at=scene-pass")
    assert code == 1, f"the refused scene target did not fail the editor with 1 (rc={code}):\n{log}"
    assert "vulkan error -2 at scene-pass" in log, f"the refusal was not reported:\n{log}"
    assert not s.windows(), "the failed editor left its window"
    s.launch(str(shot), IM_SHOT_COLOR="1:203")
    s.focus()
    s.said("surface HDR10 PQ")
    s.close()
    print("OK: a refused HDR scene target fails the editor cleanly")
