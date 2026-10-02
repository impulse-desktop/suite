"""Each decoded size reaches the screen in its own frames, in order; a seek back starts from the first size again."""
import re
from session import KEY_HOME, KEY_SPACE, Session, write_video

sizes = [(64, 48), (128, 72), (32, 24), (96, 64)]

with Session("play_resize", tool="play") as s:
    path = s.artifacts / "sizes.avi"
    write_video(path, seconds=4, sizes=sizes)
    s.launch(str(path), ALSOFT_DRIVERS="null")
    s.focus()
    s.said("ended generation=1")
    shown = re.findall(r"im play: video (\d+)x(\d+) ", s.client_log())
    assert [(int(w), int(h)) for w, h in shown] == sizes, shown
    s.tap(KEY_HOME)
    s.said("show generation=2 position_ms=0")
    s.said("video 64x48 ", 2)
    s.tap(KEY_SPACE)
    s.said("ended generation=2")
    s.close()

print("OK: every decoded size shown in order, and again from the first after a seek back")
