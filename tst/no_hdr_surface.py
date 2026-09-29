"""Under a compositor without colour management there is no HDR10
swapchain to be had: the tool asked to show an HDR capture says so and
exits 1, with no window. The HDR scenarios' counterpart, run where they
are skipped."""

from session import Session

with Session("no_hdr_surface") as s:
    if "wp_color_manager_v1" in s.globals:
        s.skip("the compositor offers wp_color_manager_v1")
    shot = s.capture_file("good.shot", 64, 48)
    code, log = s.run(str(shot), IMWAY_SHOT_COLOR="1:203")
    assert code == 1, f"the tool did not fail for want of a PQ surface (rc={code}):\n{log}"
    assert "vulkan WSI has no BT.2020/PQ surface" in log, f"the want of a PQ surface was not reported:\n{log}"
    assert not s.windows(), "the failed tool left a window"
    print("OK: without colour management the HDR editor reports the missing PQ surface")
