"""A media the player cannot open ends it before its window; a failure while playing stops every component and leaves an idle error panel; both exit 1."""
import time
from session import KEY_ESC, Session, write_video


def idle(s):
    until = time.monotonic() + 8
    while time.monotonic() < until:
        before = s.client_log().count("im frame ")
        time.sleep(0.5)
        if before == s.client_log().count("im frame "):
            time.sleep(0.7)
            if before == s.client_log().count("im frame "):
                return
    raise AssertionError("the failed player never becomes idle")


with Session("play_failure_open", tool="play") as s:
    for name, video, drivers, said in [
        ("device", dict(audio=True), "nothing", "cannot open the audio device"),
        ("codec", dict(audio=False, codec=b"XXXX"), "null", "no decoder for the video codec"),
    ]:
        path = s.artifacts / (name + ".avi")
        write_video(path, seconds=2, **video)
        code, log = s.run(str(path), ALSOFT_DRIVERS=drivers)
        assert code == 1 and "im play: " in log and said in log, (name, code, log)

with Session("play_failure_texture", tool="play") as s:
    path = s.artifacts / "wide.avi"
    write_video(path, seconds=2, sizes=[(40000, 2)])
    s.launch(str(path), ALSOFT_DRIVERS="null", IM_TRACE_FRAMES="1")
    s.focus()
    s.said("failed: ")
    idle(s)
    s.tap(KEY_ESC, client=False)
    s.gone()
    assert s.finished() == 1, "a failed player must exit 1"

print("OK: open failures exit 1 before the window, a texture failure stops the player behind an idle error panel")
