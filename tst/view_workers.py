"""Four blocked file reads prove concurrency, bounded dispatch and selection priority."""
import errno
import os
import time
from pathlib import Path

from session import Session, png


with Session("view_four_workers", tool="view") as s:
    pics = Path(s.runtime.name) / "blocked"
    pics.mkdir()
    for i in range(20):
        os.mkfifo(pics / f"f{i:02}.png")
    s.launch(*(str(fifo) for fifo in sorted(pics.iterdir())), IM_TRACE_FRAMES="1")
    s.focus()
    s.said("presenting")
    s.wait(lambda: s.client_log().count("im view: loading ") == 4, "four dispatched loads")
    time.sleep(0.3)
    before = s.client_log().count("im frame ")
    time.sleep(0.4)
    assert s.client_log().count("im frame ") == before, "pending loads cause a busy loop"
    assert s.client_log().count("im view: loading ") == 4, "unbounded dispatch"
    # A nonblocking writer opens only when a different worker is reading each FIFO.
    readers = []
    try:
        for fifo in sorted(pics.iterdir()):
            try:
                readers.append(os.open(fifo, os.O_WRONLY | os.O_NONBLOCK))
            except OSError as error:
                if error.errno != errno.ENXIO:
                    raise
        assert len(readers) == 4, f"{len(readers)} files are being read, not four"
        w, h = s.size()
        s.pointer(100, h // 2)
        s.scroll(100)
        s.click(100, h - 20)
        s.said("selected f19.png")
        assert s.client_log().count("im view: loading ") == 4, "selection queued extra work behind occupied workers"
        # Completing one thumbnail frees one credit. The latest Show goes next.
        data = png(8, 8, bytes((0, 255, 0)) * 64)
        assert os.write(readers[1], data) == len(data)
        os.close(readers[1])
        readers[1] = -1
        s.said("loading image f19.png")
        assert s.client_log().count("im view: loading ") == 5, "more than one freed credit was used"
    finally:
        for fd in readers:
            if fd >= 0:
                os.close(fd)
    s.close_releasing(sorted(pics.iterdir()))

print("OK: four concurrent workers, bounded dispatch, no idle polling, latest Show has priority, exit with workers blocked in reads")
