"""A blocked decode leaves the UI live; late results cannot replace a newer selection.
FIFOs hold real file reads without sleeps or test-only controls in the decoder.
"""
import errno
import os
from pathlib import Path

from session import KEY_HOME, KEY_I, KEY_LEFT, KEY_RIGHT, Session, near, png, write_png


with Session("view_async", tool="view") as s:
    pics = Path(s.runtime.name) / "pics"
    pics.mkdir()
    for name, rgb in (("a", (255, 0, 0)), ("b", (0, 255, 0)), ("c", (0, 0, 255))):
        write_png(pics / f"{name}.png", 64, 48, rgb)
    s.launch(str(pics))
    s.focus()
    s.said("showing a.png")
    for name in ("a", "b", "c"):
        s.said(f"thumbnail {name}.png")
    s.tap(KEY_I)
    s.said("info on")
    r = s.window()["rect"]
    canvas = (220, 0, r["width"] - 420, r["height"])
    panel = (r["width"] - 200, 0, 200, r["height"])
    first_panel = s.settled("first-panel", region=panel)

    def shows(rgb, label):
        s.wait(lambda: near(s.capture(label, region=canvas), rgb) >= 2900, f"canvas {label}")

    # Its thumbnail is already cached: selecting b now starts a Show callback.
    blocked = pics / "b.png"
    blocked.unlink()
    os.mkfifo(blocked)
    s.click(100, 220)
    s.said("loading image b.png")
    shows((255, 0, 0), "old-image-during-load")
    s.same(first_panel, "old-panel-during-load", region=panel)
    s.tap(KEY_RIGHT)
    s.said("showing c.png")
    shows((0, 0, 255), "newer-selection")
    s.changed(first_panel, "newer-panel", region=panel, threshold=20)
    newer_panel = s.settled("newer-panel", region=panel)

    def release(data):
        def writer():
            try:
                return os.open(blocked, os.O_WRONLY | os.O_NONBLOCK)
            except OSError as error:
                if error.errno != errno.ENXIO:
                    raise
                return None
        fd = s.wait(writer, "blocked decoder opens FIFO")
        try:
            assert os.write(fd, data) == len(data)
        finally:
            os.close(fd)

    release(png(64, 48, bytes((0, 255, 0)) * (64 * 48)))
    s.said("discarded image b.png")
    assert "im view: showing b.png" not in s.client_log(), "late image replaced the selection"
    shows((0, 0, 255), "late-success-ignored")
    s.same(newer_panel, "late-success-panel-ignored", region=panel)

    # A stale error must also leave the newer selection alone.
    s.tap(KEY_LEFT)
    s.said("loading image b.png", 2)
    shows((0, 0, 255), "old-image-during-second-load")
    s.same(newer_panel, "old-panel-during-second-load", region=panel)
    s.tap(KEY_HOME)
    s.said("showing a.png", 2)
    release(b"not an image")
    s.said("discarded image b.png", 2)
    assert "im view: cannot show b.png" not in s.client_log(), "stale error replaced the selection"
    shows((255, 0, 0), "late-error-ignored")

    # Exit while a worker is blocked in file I/O: the test build joins it
    # once its read ends.
    s.tap(KEY_RIGHT)
    s.said("loading image b.png", 3)
    s.close_releasing([blocked])

print("OK: image and properties retained together, stale success and error ignored, exit with a worker blocked in a read")
