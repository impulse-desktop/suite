"""Renderer operations exercised through the same interface on both platforms."""
import os
from pathlib import Path
from session import Session

with Session("renderer_pixels") as s:
    probe = Path(os.environ["IM_E2E_HELPERS"]) / "renderer_test"
    code, log = s.run(command=probe)
    assert code == 0, log
    assert "OK: renderer upload" in log, log
    print(log)
