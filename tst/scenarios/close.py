"""The editor closed by the compositor: xdg_toplevel.close stops its loop as
a cancel, so it exits cleanly without saving."""

import time

from session import Session

with Session("close") as s:
    shot = s.capture_file("good.shot", 64, 48)
    shots = s.artifacts / "shots"
    s.launch(str(shot), IMWAY_SHOT_DIR=str(shots), IMWAY_SHOT_NAME="closed", IMWAY_SHOT_FORMAT="png")
    node = s.focus()
    time.sleep(0.5)
    s.ipc(f'[con_id={node["id"]}] kill')
    s.gone()
    assert s.finished() == 0, "the editor did not exit cleanly on close"
    assert not shots.is_dir() or not any(shots.iterdir()), "closing the editor saved a file"
    print("OK: the compositor's close cancels without saving")
