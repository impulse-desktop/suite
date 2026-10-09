"""Playback recycles the video swapchain, sleeps when paused and seeks with full queues."""
import re
import time
from session import KEY_HOME, KEY_I, KEY_LEFT, KEY_RIGHT, KEY_SPACE, Session, write_video


def shows(s):
    return [(int(g), int(p)) for g, p in re.findall(r"im play: show generation=(\d+) position_ms=(\d+)", s.client_log())]


def idle(s):
    until = time.monotonic() + 8
    while time.monotonic() < until:
        before = s.client_log().count("im frame ")
        time.sleep(0.5)
        if before == s.client_log().count("im frame "):
            time.sleep(0.7)
            if before == s.client_log().count("im frame "):
                return
    raise AssertionError("paused player never becomes idle")


for audio in (True, False):
    with Session("play_audio" if audio else "play_silent", tool="play") as s:
        path = s.artifacts / "movie.avi"
        write_video(path, audio=audio)
        s.launch(str(path), ALSOFT_DRIVERS="null", IM_TRACE_FRAMES="1")
        s.focus()
        s.said("opened")
        s.wait(lambda: len(shows(s)) >= 30, "more than ten image slots rendered")
        s.tap(KEY_SPACE)
        s.said("pause")
        idle(s)
        previous = shows(s)
        s.tap(KEY_RIGHT)
        s.said("seek generation=2")
        s.wait(lambda: shows(s)[-1][0] == 2, "seek on pause displays a preview")
        assert shows(s)[-1][1] >= 10000, shows(s)
        idle(s)
        # Stop while all data queues and the video swapchain can be full.
        s.tap(KEY_HOME)
        s.wait(lambda: shows(s)[-1] == (3, 0), "stop returns to the first frame")
        idle(s)
        for key in (KEY_RIGHT, KEY_LEFT) * 3:
            s.tap(key)
        s.wait(lambda: shows(s)[-1] == (9, 0), "rapid seeks settle on the latest generation")
        idle(s)
        n = len(shows(s))
        s.tap(KEY_SPACE)
        s.said("play generation=9")
        s.wait(lambda: len(shows(s)) > n + 10, "play resumes after seeks")
        assert all(g == 9 for g, _ in shows(s)[n:]), "old generation was displayed after seek"
        s.tap(KEY_I)
        s.said("panel on")
        s.tap(KEY_I)
        s.said("panel off")
        s.close()

print("OK: A/V and silent playback, buffer reuse, pause idle, seek preview, stop and rapid seeks")
