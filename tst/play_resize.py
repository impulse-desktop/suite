"""Each decoded size reaches the screen in its own frames, in order; a seek back starts from the first size again.
A video taller at the window's width than its place is cut to it: nothing of it over the bar."""
import collections
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

    s.settled("ended")

    def colour():
        w, h, px = s.capture("middle", region=(460, 280, 40, 20))
        common = collections.Counter(tuple(px[i:i + 3]) for i in range(0, w * h * 3, 3)).most_common(1)[0][0]
        return common if sum(common) > 100 else None

    video = s.wait(colour, "the video in the window")
    r = s.window()["rect"]
    w, h, px = s.capture("bar", region=(0, r["height"] - 20, r["width"], 20))
    spilled = sum(1 for i in range(0, w * h * 3, 3) if all(abs(px[i + k] - video[k]) <= 6 for k in range(3)))
    assert spilled == 0, f"{spilled} pixels of the video {video} over the bar"
    s.close()

print("OK: every decoded size shown in order, and again from the first after a seek back")
