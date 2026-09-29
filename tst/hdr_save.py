"""Saving from an HDR session, without a window (IMWAY_SHOT_ACTION=save).
The capture is a PQ frame: bands of neutral grey at known luminances. A
PNG is an SDR image: the tool decodes the PQ codes to nits, maps them onto
the SDR range and writes sRGB with SDR white (203 nits) at 255, so every
band must land on the byte that transform gives. A JPEG XL keeps the frame
HDR: lossless, tagged PQ, its samples are the codes themselves."""

from session import Session, hdr_png_grey, is_jxl, jxl_pixels, png_pixels, pq_decode, pq_encode

NITS = [1, 10, 50, 100, 150, 182, 203, 300, 600, 1000, 4000]
BAND = 4

with Session("hdr_save") as s:
    s.require("wp_color_manager_v1")
    w, h = 64, len(NITS) * BAND
    codes = [round(pq_encode(nits) * 255) for nits in NITS]
    pixels = b"".join(bytes([code, code, code, 255]) * (w * BAND) for code in codes)
    shot = s.capture_file("hdr.shot", w, h, pixels=pixels)
    shots = s.artifacts / "shots"

    def save(fmt):
        code, log = s.run(str(shot), IMWAY_SHOT_COLOR="1:203", IMWAY_SHOT_ACTION="save", IMWAY_SHOT_FORMAT=fmt,
                          IMWAY_SHOT_DIR=str(shots), IMWAY_SHOT_NAME="hdr")
        assert code == 0 and "im screenshot: surface HDR10 PQ" in log, f"the HDR {fmt} save failed (rc={code}):\n{log}"
        return shots / f"hdr.{fmt}"

    pw, ph, px = png_pixels(save("png"))
    assert (pw, ph) == (w, h), f"the PNG is {pw}x{ph}, not {w}x{h}"
    for i, (nits, code) in enumerate(zip(NITS, codes)):
        # the tool decodes the 8-bit code it was handed, not the nits asked for
        expected = hdr_png_grey(pq_decode(code / 255))
        at = ((i * BAND + BAND // 2) * w + w // 2) * 4
        r, g, b = px[at:at + 3]
        print(f"{nits} nits: code {code} -> png {(r, g, b)}, expected {expected}")
        assert max(abs(r - expected), abs(g - expected), abs(b - expected)) <= 2, f"{nits} nits saved as {(r, g, b)}, the tool's mapping gives {expected}"
        assert abs(r - g) <= 1 and abs(g - b) <= 1, f"{nits} nits saved as {(r, g, b)}, not neutral"

    saved = save("jxl")
    assert is_jxl(saved), f"{saved} is not a JPEG XL stream"
    jw, jh, samples = jxl_pixels(s, saved)
    assert (jw, jh) == (w, h), f"the JPEG XL is {jw}x{jh}, not {w}x{h}"
    for i, code in enumerate(codes):
        at = ((i * BAND + BAND // 2) * w + w // 2) * 3
        assert samples[at:at + 3] == (code * 257,) * 3, f"code {code} saved as {samples[at:at + 3]}, not {code * 257} lossless"
    print("OK: an HDR session saves an sRGB PNG mapped from its PQ codes and a lossless PQ JPEG XL")
