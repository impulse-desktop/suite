"""Every pixel format FFmpeg describes, and every color description the
video shaders know, from a picture through the shader the generator made
for it back to that picture."""
import os
from pathlib import Path
from session import Session

with Session("video_formats") as s:
    probe = Path(os.environ["IM_E2E_HELPERS"]) / "video_test"
    code, log = s.run(command=probe, timeout=600)
    assert code == 0, log
    assert "video color checks" in log, log
    print(log.strip().splitlines()[-1])
