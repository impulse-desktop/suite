"""Every pixel format FFmpeg describes, drawn the player's one way (the
lanczos kernel in the tile's shared memory, or per pixel where it cannot be
built) against a CPU model of the filter, the layer scaled and shrunk
against that model in linear light, and every matrix,
chroma location, color system, transfer and set of primaries the video
shaders know, from a picture through its shader back to that picture. A
test node runs one bucket of these checks, by a hash of their names."""
import os
from pathlib import Path
from session import Session

with Session("video") as s:
    bucket, buckets = os.environ.get("IM_E2E_BUCKET", "0/1").split("/")
    probe = Path(os.environ["IM_E2E_HELPERS"]) / "video_test"
    code, log = s.run(bucket, buckets, command=probe, timeout=600)
    assert code == 0, f"exit status {code}\n{log}"
    assert "video color checks" in log, log
    print(log.strip().splitlines()[-1])
