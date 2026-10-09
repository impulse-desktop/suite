"""The HDR editor builds the compositor's pipelines with its renderer; the
device refuses them (IM_CHAOS vulkan-at=compose). The editor reports the
refusal and exits 1, tearing down what it had built without faulting on it;
the next editor opens as usual and shows its HDR frames."""

from session import Session

with Session("linear_hdr_fault") as s:
    s.require("wp_color_manager_v1")
    shot = s.capture_file("good.shot", 64, 48)
    code, log = s.run(str(shot), IM_SHOT_COLOR="1", IM_CHAOS="vulkan-at=compose")
    assert code == 1, f"the refused compositor did not fail the editor with 1 (rc={code}):\n{log}"
    assert "vulkan error -2 at compose" in log, f"the refusal was not reported:\n{log}"
    assert not s.windows(), "the failed editor left its window"
    s.launch(str(shot), IM_SHOT_COLOR="1")
    s.focus()
    s.said("surface HDR10 PQ")
    s.close()
    print("OK: a refused compositor fails the HDR editor cleanly")
