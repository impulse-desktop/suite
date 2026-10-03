"""Every pixel format FFmpeg describes through its video shader with
lanczos filtering, against a CPU model of the filter."""
import os
from pathlib import Path
from session import Session

with Session("video_lanczos") as s:
    probe = Path(os.environ["IM_E2E_HELPERS"]) / "video_test"
    code, log = s.run("lanczos", command=probe, timeout=600)
    assert code == 0, log
    assert "video color checks" in log, log
    print(log.strip().splitlines()[-1])
