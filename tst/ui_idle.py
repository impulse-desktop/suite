"""A mapped window sleeps, wakes on input/resize, and sleeps again after repeat and texture retirement."""
import time

from session import KEY_RIGHT, Session, write_png


def frames(s):
    return sum(line.startswith("im frame ") for line in s.client_log().splitlines())


def idle(s):
    until = time.monotonic() + 8
    while time.monotonic() < until:
        before = frames(s)
        time.sleep(0.4)
        if frames(s) == before:
            time.sleep(1)
            if frames(s) == before:
                return
    raise AssertionError("window never stops rendering")


with Session("ui_idle", tool="ui") as s:
    s.launch(IM_TRACE_FRAMES="1")
    s.focus()
    s.said("presenting")
    s.pointer(1, 1)
    idle(s)
    before = frames(s)
    w, h = s.size()
    s.pointer(w // 2, h // 2)
    s.wait(lambda: frames(s) > before, "pointer wakes rendering")
    idle(s)
    before = frames(s)
    node = s.window()
    s.ipc(f'[con_id={node["id"]}] resize set 360 px 180 px')
    s.said("presenting 360x180")
    s.wait(lambda: frames(s) > before, "resize wakes rendering")
    idle(s)
    s.ipc(f'[con_id={node["id"]}] kill')
    s.gone()
    assert s.finished() == 0, "a closed window did not end the tool"

with Session("view_idle", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    for i in range(20):
        write_png(pics / f"f{i:02}.png", 8, 8, (i * 10, 128, 255 - i * 10))
    s.launch(str(pics), IM_TRACE_FRAMES="1")
    s.focus()
    s.said("showing f00.png")
    idle(s)
    s.key(KEY_RIGHT, 1)
    s.said("selected f03.png")
    s.key(KEY_RIGHT, 0)
    idle(s)
    selections = s.client_log().count("selected ")
    before = frames(s)
    time.sleep(0.7)
    assert s.client_log().count("selected ") == selections, "released key still repeats"
    assert frames(s) == before, "texture retirement does not finish"
    s.close()

print("OK: idle window sleeps; input, resize and key repeat wake it; release and texture retirement settle")
