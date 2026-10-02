"""Each decoded size gets its own mapping of ten video buffers; a replaced mapping is unmapped once all its buffers are back."""
import re
from session import KEY_HOME, KEY_SPACE, Session, write_video

sizes = [(64, 48), (128, 72), (32, 24), (96, 64)]

with Session("play_resize", tool="play") as s:
    path = s.artifacts / "sizes.avi"
    write_video(path, seconds=4, sizes=sizes)
    s.launch(str(path), ALSOFT_DRIVERS="null")
    s.focus()
    s.said("ended generation=1")
    mapped = re.findall(r"im play: mapped (\d+) bytes for (\d+)x(\d+)", s.client_log())
    assert [(int(w), int(h)) for _, w, h in mapped] == sizes, mapped
    assert all(int(size) <= 2 << 20 for size, _, _ in mapped), mapped
    s.said("unmapped ", 3)
    s.tap(KEY_HOME)
    s.said("show generation=2 position_ms=0")
    s.said("mapped 655360 bytes for 64x48", 2)
    s.said("unmapped 96x64")
    s.tap(KEY_SPACE)
    s.said("ended generation=2")
    s.close()

print("OK: a mapping per decoded size, replaced mappings unmapped, seek back and replay")
