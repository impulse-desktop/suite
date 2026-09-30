"""One image of each format the scenario can write (PNG, PPM, PGM, PBM,
PAM, BMP, TGA) and the small ones kept next to the scenarios (JPEG, WebP,
JPEG XL, GIF): each decodes to its size and colour."""

import shutil
import struct
from pathlib import Path

from session import KEY_RIGHT, Session, near, png

with Session("view_formats", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    w, h = 16, 16
    n = w * h

    def bmp(rgb):
        row = bytes(reversed(rgb)) * w
        row += b"\0" * ((4 - len(row) % 4) % 4)
        pixels = row * h
        return struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54) + struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(pixels), 2835, 2835, 0, 0) + pixels

    def tga(rgb):
        return struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 24, 0x20) + bytes(reversed(rgb)) * n

    files = [
        ("a.png", png(w, h, bytes((255, 0, 0)) * n), (255, 0, 0)),
        ("b.ppm", b"P6\n%d %d\n255\n" % (w, h) + bytes((0, 255, 0)) * n, (0, 255, 0)),
        ("c.pgm", b"P5\n%d %d\n255\n" % (w, h) + bytes((128,)) * n, (128, 128, 128)),
        ("d.pbm", b"P4\n%d %d\n" % (w, h) + b"\0\0" * h, (255, 255, 255)),
        ("e.pam", b"P7\nWIDTH %d\nHEIGHT %d\nDEPTH 3\nMAXVAL 255\nTUPLTYPE RGB\nENDHDR\n" % (w, h) + bytes((0, 0, 255)) * n, (0, 0, 255)),
        ("f.bmp", bmp((255, 255, 0)), (255, 255, 0)),
        ("g.tga", tga((0, 255, 255)), (0, 255, 255)),
    ]
    for name, data, _ in files:
        (pics / name).write_bytes(data)
    kept = Path(__file__).parent
    for name, colour in (("h.jpg", (255, 0, 0)), ("i.webp", (0, 255, 0)), ("j.jxl", (0, 0, 255)), ("k.gif", (255, 255, 0))):
        shutil.copy(kept / f"solid_{name}", pics / name)
        files.append((name, None, colour))
    s.launch(str(pics))
    s.focus()
    s.said(f"listed {len(files)}")
    r = s.window()["rect"]
    canvas = (250, 0, r["width"] - 250, r["height"] - 30)
    for i, (name, _, colour) in enumerate(files):
        if i:
            s.tap(KEY_RIGHT)
        s.said(f"showing {name} {w}x{h}")
        s.wait(lambda: near(s.capture(name, region=canvas), colour, tolerance=12) >= n * 9 // 10, f"the canvas showing {name}")
    s.close()
    print("OK: every format decodes to its pixels")
