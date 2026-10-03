"""Every matrix, chroma location, color system, transfer and set of
primaries the video shaders know, from a picture through the shader back
to that picture."""
import os
from pathlib import Path
from session import Session

with Session("video_colors") as s:
    probe = Path(os.environ["IM_E2E_HELPERS"]) / "video_test"
    code, log = s.run("colors", command=probe, timeout=600)
    assert code == 0, log
    assert "video color checks" in log, log
    print(log.strip().splitlines()[-1])
