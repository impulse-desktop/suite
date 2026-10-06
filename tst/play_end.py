"""EOF, replay, audio-only seek and CLI failures."""
import time
import wave
from session import KEY_HOME, KEY_RIGHT, KEY_SPACE, Session, write_video

for mode in ("av", "video", "audio"):
    with Session("play_end_" + mode, tool="play") as s:
        path = s.artifacts / ("sound.wav" if mode == "audio" else "short.avi")
        if mode == "audio":
            with wave.open(str(path), "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(48000)
                w.writeframes(b"\0" * 48000 * 2 * 2)
        else:
            write_video(path, seconds=2, audio=mode == "av")
        s.launch(str(path), ALSOFT_DRIVERS="null", IM_TRACE_FRAMES="1")
        s.focus()
        s.said("ended generation=1")
        # the end asks for one more frame, which a software renderer may take
        # a second to finish, and the compositor's pointer entering and
        # leaving the mapped window draws too: the frames must stop, not
        # have stopped already
        deadline = time.monotonic() + 10
        before = s.client_log().count("im frame ")
        while True:
            time.sleep(0.7)
            after = s.client_log().count("im frame ")
            if after == before:
                break
            assert time.monotonic() < deadline, "EOF keeps rendering"
            before = after
        s.tap(KEY_SPACE)
        s.said("seek generation=2 position_ms=0")
        s.said("ended generation=2")
        s.tap(KEY_HOME)
        s.said("seek generation=3 position_ms=0")
        s.tap(KEY_RIGHT)
        s.said("seek generation=4")
        s.tap(KEY_SPACE)
        s.said("ended generation=4")
        s.close()

with Session("play_cli", tool="play") as s:
    code, log = s.run()
    assert code == 2 and "usage: im play <file>" in log, (code, log)
    code, log = s.run(str(s.artifacts / "missing.avi"), ALSOFT_DRIVERS="null")
    assert code == 1 and "im play: " in log and "No such file or directory" in log, (code, log)

print("OK: A/V, silent and audio-only EOF, replay, seek and CLI errors")
