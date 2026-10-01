"""A save that cannot produce its file does not fail silently: the
save-mode tool, which otherwise never maps, opens on its error panel
instead and leaves when dismissed. Three ways to get there: a filename
template that expands past the name limit, a directory that cannot be
created because a regular file sits where it should be, and a disk that
is full."""

import errno
import os
import time

from session import Session, KEY_ENTER

with Session("save_errors") as s:
    shot = s.capture_file("good.shot", 64, 48)

    def attempt(what, **env):
        s.launch(str(shot), IM_SHOT_ACTION="save", IM_SHOT_FORMAT="png", **env)
        s.focus()
        time.sleep(0.5)
        s.close()

    shots = s.artifacts / "shots"
    attempt("an overlong name", IM_SHOT_DIR=str(shots), IM_SHOT_NAME="x" * 300)
    assert not shots.is_dir() or not any(shots.iterdir()), "an overlong name still wrote a file"

    blocker = s.artifacts / "blocker"
    blocker.write_bytes(b"")
    attempt("a directory behind a file", IM_SHOT_DIR=str(blocker / "shots"), IM_SHOT_NAME="blocked")
    assert blocker.is_file() and blocker.stat().st_size == 0, "the blocking file was touched"

    # a disk that fills up under the write: the file opens, the write fails
    full = s.artifacts / "full"
    full.mkdir()
    os.symlink("/dev/full", full / "disk.png")
    attempt("a full disk", IM_SHOT_DIR=str(full), IM_SHOT_NAME="disk")
    assert "saved" not in s.client_log(), "a write into a full disk was reported saved"

    s.launch(str(shot), IM_SHOT_ACTION="editor", IM_SHOT_FORMAT="png",
             IM_SHOT_DIR=str(full), IM_SHOT_NAME="disk")
    s.focus()
    s.tap(KEY_ENTER)
    s.wait(lambda: f"(code {errno.ENOSPC}," in s.client_log(), "the editor's save error")
    s.close()
    assert "saved" not in s.client_log(), "an editor save into a full disk was reported saved"
    print("OK: a save that cannot write its file opens the tool on the error")
