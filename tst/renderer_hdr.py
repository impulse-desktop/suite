"""The renderer contract with PQ images and a linear HDR scene."""
import os
from pathlib import Path
from session import Session

with Session("renderer_hdr") as s:
    s.require("wp_color_manager_v1")
    probe = Path(os.environ["IM_E2E_HELPERS"]) / "renderer_test"
    code, log = s.run("--hdr", command=probe)
    assert code == 0, log
    assert "OK: renderer upload" in log, log
    print(log)
