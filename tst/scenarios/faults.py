"""The editor's own faults (IM_CHAOS). A device without a memory type that
fits the texture fails the tool with a report and status 1. A swapchain
gone stale under it, at an acquire or a present, out of date or
suboptimal, is rebuilt and the editor carries on to close cleanly. A
Vulkan call it cannot do without failing at setup ends it with the error
on record and status 1, no window, and so does a device without the
swapchain extension, naming it. An encoder without memory for libpng's or
libjxl's objects makes a save fail without a file: the window a save
never shows comes up on the error panel, and Escape ends the tool
cleanly; with memory to spare the save writes its file without a window."""

import time

from session import Session

with Session("faults") as s:
    shot = s.capture_file("good.shot", 64, 48)
    shots = s.artifacts / "shots"

    code, log = s.run(str(shot), IM_CHAOS="memory-types=1")
    assert code == 1, f"a device without a fitting memory type did not fail the tool (rc={code}):\n{log}"
    assert "no vulkan memory type fits" in log, f"the missing memory type was not reported:\n{log}"

    def drawn():
        # the editor's panel and canvas are on screen: many colours in its window
        w, h, px = s.capture("stale")
        colours = {px[(y * w + x) * 3:(y * w + x) * 3 + 3] for y in range(2, h - 2, 5) for x in range(2, w - 2, 5)}
        return len(colours) > 4

    for fault in ("swapchain=0", "swapchain=3", "swapchain-suboptimal=0", "swapchain-suboptimal=3"):
        s.launch(str(shot), IM_CHAOS=fault)
        s.focus()
        s.wait(drawn, f"{fault}: the editor drawn after the rebuild")
        s.close()

    code, log = s.run(str(shot), IM_CHAOS="vulkan=0")
    assert code == 1 and "vulkan error" in log, f"a failed Vulkan instance did not end the tool with the error (rc={code}):\n{log}"
    code, log = s.run(str(shot), IM_CHAOS="no-ext=VK_KHR_swapchain")
    assert code == 1 and "vulkan lacks VK_KHR_swapchain" in log, f"a device without swapchains was not named (rc={code}):\n{log}"

    for fmt, k in (("png", 0), ("png", 1), ("jxl", 0), ("jxl", 1)):
        s.launch(str(shot), IM_CHAOS=f"encoder-alloc={k}", IMWAY_SHOT_ACTION="save", IMWAY_SHOT_FORMAT=fmt,
                 IMWAY_SHOT_DIR=str(shots), IMWAY_SHOT_NAME=f"{fmt}-{k}")
        s.focus()
        time.sleep(0.3)
        s.close()
        assert not shots.is_dir() or not any(shots.iterdir()), f"{fmt} encoder-alloc={k}: a failed encoder saved a file"

    code, log = s.run(str(shot), IMWAY_SHOT_ACTION="save", IMWAY_SHOT_FORMAT="png", IMWAY_SHOT_DIR=str(shots), IMWAY_SHOT_NAME="spare")
    assert code == 0 and (shots / "spare.png").stat().st_size, f"a save with memory to spare failed (rc={code}):\n{log}"
    print("OK: the editor's faults end it with a report or are survived")
