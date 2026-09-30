"""`imscreenshot PATH` on files: a raw capture file loads through the file
path and Enter saves it as JPEG XL into IM_SHOT_DIR; a file that is too
small, one with a bad header and a truncated one each open the error
panel, which Escape dismisses, and so does Enter."""

import struct
import time

from session import KEY_ENTER, KEY_ESC, Session, is_jxl

with Session("file") as s:
    w, h = 64, 48
    pixels = bytes([0xFF, 0x00, 0xFF, 0xFF]) * (w * h)
    good = s.capture_file("good.shot", w, h, pixels=pixels)
    small = s.artifacts / "small.shot"
    small.write_bytes(b"IMW1")
    bad = s.artifacts / "bad.shot"
    bad.write_bytes(struct.pack("<III", 0x12345678, w, h) + pixels)
    trunc = s.artifacts / "trunc.shot"
    trunc.write_bytes(struct.pack("<III", 0x31574D49, w, h) + pixels[: len(pixels) // 2])
    shots = s.artifacts / "shots"

    s.launch(str(good), IM_SHOT_DIR=str(shots), IM_SHOT_NAME="fromfile")
    s.focus()
    time.sleep(0.5)
    s.close(KEY_ENTER)
    saved = shots / "fromfile.jxl"
    assert saved.is_file() and saved.stat().st_size, "Enter did not save the file capture"
    assert is_jxl(saved), f"{saved} is not a JPEG XL stream"

    for path, key in ((small, KEY_ESC), (bad, KEY_ESC), (trunc, KEY_ENTER)):
        s.launch(str(path), IM_SHOT_DIR=str(shots), IM_SHOT_NAME="broken")
        s.focus()
        assert s.size() == (480, 180), f"{path.name}: not the error panel ({s.size()})"
        time.sleep(0.3)
        s.close(key)
        assert not (shots / "broken.jxl").exists(), f"{path.name}: the error panel saved a file"
    print("OK: raw files load and save, broken files show the error panel")
