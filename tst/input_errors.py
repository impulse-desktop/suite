"""`im screenshot` given what it cannot use. A file that does not exist or
whose header claims a zero width or height, and shared-buffer metadata it
cannot parse (too few, empty, negative or run-together fields, a field past
64 bits, a GPU id that is not 32 lowercase hex digits, a zero width,
height, stride or size), or whose buffer cannot be opened, each open the
480x180 error panel, which Escape dismisses with status 0. Well-formed
metadata naming a GPU no device has gets no window: the tool says so and
exits 1. Colour metadata it only partly understands falls back to SDR and
still shows the image, and a ui scale below zero is no scale."""

import struct
import time

from session import Session

with Session("input_errors") as s:
    good = s.capture_file("good.shot", 64, 48)
    no_width = s.artifacts / "no-width.shot"
    no_width.write_bytes(struct.pack("<III", 0x31574D49, 0, 48) + bytes(64))
    no_height = s.artifacts / "no-height.shot"
    no_height.write_bytes(struct.pack("<III", 0x31574D49, 64, 0) + bytes(64))
    missing = s.artifacts / "missing.shot"
    uuid = "00112233445566778899aabbccddeeff"

    def opened(what, path, **env):
        s.launch(str(path), **env)
        s.focus()
        time.sleep(0.3)
        return s.size()

    def error_panel(what, path, **env):
        assert opened(what, path, **env) == (480, 180), f"{what}: not the error panel ({s.size()})"
        s.close()

    def image(what, path, **env):
        assert opened(what, path, **env) != (480, 180), f"{what}: the image did not load"
        s.close()

    error_panel("a missing file", missing)
    error_panel("a file of zero width", no_width)
    error_panel("a file of zero height", no_height)
    error_panel("too few fields", good, IMWAY_SHOT_DMABUF="64:48:44")
    error_panel("a field past 64 bits", good, IMWAY_SHOT_DMABUF=f"64:48:44:0:256:0:99999999999999999999999:{uuid}")
    error_panel("a GPU id that is not hex", good, IMWAY_SHOT_DMABUF="64:48:44:0:256:0:12288:zz112233445566778899aabbccddeeff")
    error_panel("an empty field", good, IMWAY_SHOT_DMABUF=f"64::44:0:256:0:12288:{uuid}")
    error_panel("a negative field", good, IMWAY_SHOT_DMABUF=f"64:-48:44:0:256:0:12288:{uuid}")
    error_panel("two fields run together", good, IMWAY_SHOT_DMABUF=f"64x48:44:0:256:0:12288:{uuid}")
    error_panel("a GPU id too short", good, IMWAY_SHOT_DMABUF="64:48:44:0:256:0:12288:00112233445566778899aabbccddee")
    error_panel("a GPU id in capitals", good, IMWAY_SHOT_DMABUF="64:48:44:0:256:0:12288:00112233445566778899AABBCCDDEEFF")
    error_panel("a GPU id with punctuation", good, IMWAY_SHOT_DMABUF="64:48:44:0:256:0:12288:00112233-45566778899aabbccddeeff")
    error_panel("a zero stride", good, IMWAY_SHOT_DMABUF=f"64:48:44:0:0:0:12288:{uuid}")
    error_panel("a zero width", good, IMWAY_SHOT_DMABUF=f"0:48:44:0:256:0:12288:{uuid}")
    error_panel("a zero height", good, IMWAY_SHOT_DMABUF=f"64:0:44:0:256:0:12288:{uuid}")
    error_panel("a zero size", good, IMWAY_SHOT_DMABUF=f"64:48:44:0:256:0:0:{uuid}")
    error_panel("a buffer that cannot be opened", missing, IMWAY_SHOT_DMABUF=f"64:48:44:0:256:0:12288:{uuid}")
    image("colour without a volume", good, IMWAY_SHOT_COLOR="0:203")
    image("colour without a transfer", good, IMWAY_SHOT_COLOR="garbage")
    # a ui scale below zero is no scale: the editor opens at scale 1, the
    # 64x48 capture at 50% beside the 200px panel and 8px of spacing, as
    # tall as the panel's minimum
    assert opened("a negative ui scale", good, IMGUI_SCALE="-2") == (240, 220), f"a negative ui scale: {s.size()}"
    s.close()

    code, log = s.run(str(good), IMWAY_SHOT_DMABUF=f"64:48:44:0:256:0:12288:{uuid}")
    assert code == 1, f"an unknown GPU did not fail the tool (rc={code}):\n{log}"
    assert "shared screenshot gpu is unavailable" in log, f"the unknown GPU was not reported:\n{log}"
    print("OK: unusable inputs open the error panel or are reported")
