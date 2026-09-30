"""The session ends under an open editor: its connection goes with the
compositor, its loop ends without a verdict, and the tool takes that as a
cancel, exiting 0 with nothing saved instead of hanging."""

import time

from session import Session

with Session("compositor_gone") as s:
    shot = s.capture_file("good.shot", 64, 48)
    shots = s.artifacts / "shots"
    s.launch(str(shot), IM_SHOT_DIR=str(shots), IM_SHOT_NAME="orphan", IM_SHOT_FORMAT="png")
    s.focus()
    time.sleep(0.5)
    s.compositor.terminate()
    s.compositor.wait(timeout=10)
    code = s.finished(timeout=10)
    assert code == 0, f"the tool exited {code} after its compositor left"
    assert not shots.is_dir() or not any(shots.iterdir()), "the tool saved a file without a verdict"
    print("OK: a tool whose compositor leaves exits as cancelled")
