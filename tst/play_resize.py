"""Each decoded size reaches the screen in its own frames, in order; a seek back starts from the first size again.
A window shorter than the video at its width shows the video cut to its place: nothing of it over the bar."""
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

    node = s.window()
    s.ipc(f'[con_id={node["id"]}] resize set 640 px 160 px')
    s.wait(lambda: s.window()["rect"]["height"] == 160, "the short window")

    def colour():
        w, h, px = s.capture("middle", region=(300, 60, 40, 20))
        common = collections.Counter(tuple(px[i:i + 3]) for i in range(0, w * h * 3, 3)).most_common(1)[0][0]
        return common if sum(common) > 100 else None

    video = s.wait(colour, "the video in the short window")
    w, h, px = s.capture("bar", region=(0, 140, 640, 20))
    spilled = sum(1 for i in range(0, w * h * 3, 3) if all(abs(px[i + k] - video[k]) <= 6 for k in range(3)))
    assert spilled == 0, f"{spilled} pixels of the video {video} over the bar"
    s.close()

print("OK: every decoded size shown in order, and again from the first after a seek back")
