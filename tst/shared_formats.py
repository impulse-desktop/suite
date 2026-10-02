"""Native texture readback preserves all channels in 8-bit and packed 10-bit layouts."""
import os
import struct
from session import Session, jxl_pixels, udmabuf

with Session("shared_formats") as s:
    shots = s.artifacts / "shots"
    uuid = s.device_uuid()
    w, h = 64, 48
    for fmt, pixel, expected in (
        (37, 0xFF332211, (4369, 8738, 13107)),
        (44, 0xFF112233, (4369, 8738, 13107)),
        (58, 0xFFF80001, (65535, 32800, 64)),
        (64, 0xC01803FF, (65535, 32800, 64)),
    ):
        s.require_import(fmt)
        fd = udmabuf(struct.pack("<I", pixel) * (w * h))
        if fd is None:
            s.skip("the kernel offers no udmabuf")
        try:
            spec = f"{w}:{h}:{fmt}:0:{w * 4}:0:{w * h * 4}:{uuid}"
            code, log = s.run("fd:3", fd3=fd, IM_SHOT_DMABUF=spec, IM_SHOT_ACTION="save",
                              IM_SHOT_FORMAT="jxl", IM_SHOT_DIR=str(shots), IM_SHOT_NAME=str(fmt))
            assert code == 0, log
            width, height, pixels = jxl_pixels(s, shots / f"{fmt}.jxl")
            assert (width, height) == (w, h)
            assert pixels == expected * (w * h), f"format {fmt}: {pixels[:3]} != {expected}"
        finally:
            os.close(fd)
    print("OK: RGBA, BGRA and both packed 10-bit layouts preserve readback precision")
