"""Every pixel format FFmpeg describes, with bilinear filtering, with
lanczos against a CPU model of the filter and through the compute kernel,
and every matrix, chroma location, color system, transfer and set of
primaries the video shaders know, from a picture through its shader back
to that picture. A test node runs one bucket of these checks, by a hash of
their names."""
import os
from pathlib import Path
from session import Session

with Session("video") as s:
    bucket, buckets = os.environ.get("IM_E2E_BUCKET", "0/1").split("/")
    probe = Path(os.environ["IM_E2E_HELPERS"]) / "video_test"
    code, log = s.run(bucket, buckets, command=probe, timeout=600)
    assert code == 0, log
    assert "video color checks" in log, log
    print(log.strip().splitlines()[-1])
