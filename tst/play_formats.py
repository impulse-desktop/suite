"""Every pixel format the decoders emit, and every color description the
shader knows, from a picture through swscale and the player's shader back
to that picture."""
from session import Session

with Session("play_formats", tool="play") as s:
    code, log = s.run("--formats", timeout=600)
    assert code == 0, log
    assert "video color checks" in log, log
    print(log.strip().splitlines()[-1])
