"""Saving from an HDR session, without a window (IM_SHOT_ACTION=save).
The capture is a PQ frame: bands of neutral grey at known luminances,
black, and colours beyond the sRGB gamut or the SDR peak. A PNG is an SDR
image: the tool decodes the PQ codes to nits, maps them onto the SDR
range, holds the chroma inside the gamut and writes sRGB with SDR white
(203 nits) at 255, so every band must land on the bytes that transform
gives. A JPEG XL keeps the frame HDR: lossless, tagged PQ, its samples
are the codes themselves. The colour metadata names the whole display
volume, as a compositor with an EDID does."""

from session import Session, hdr_png_pixel, is_jxl, jxl_pixels, png_pixels, pq_decode, pq_encode

GREYS = [1, 10, 50, 100, 150, 182, 203, 300, 600, 1000, 4000]
# black; BT.2020 green, outside the sRGB gamut; red brighter than the
# peak; a blue-tinted grey
BANDS = [(n, n, n) for n in GREYS] + [(0, 0, 0), (0, 300, 0), (1000, 0, 0), (50, 50, 400)]
BAND = 4

with Session("hdr_save") as s:
    s.require("wp_color_manager_v1")
    w, h = 64, len(BANDS) * BAND
    codes = [tuple(round(pq_encode(n) * 255) for n in band) for band in BANDS]
    pixels = b"".join(bytes([*code, 255]) * (w * BAND) for code in codes)
    shot = s.capture_file("hdr.shot", w, h, pixels=pixels)
    shots = s.artifacts / "shots"

    def save(fmt):
        code, log = s.run(str(shot), IM_SHOT_COLOR="1:203:0.005:1000:400", IM_SHOT_ACTION="save", IM_SHOT_FORMAT=fmt,
                          IM_SHOT_DIR=str(shots), IM_SHOT_NAME="hdr")
        assert code == 0 and "im screenshot: surface HDR10 PQ" in log, f"the HDR {fmt} save failed (rc={code}):\n{log}"
        return shots / f"hdr.{fmt}"

    pw, ph, px = png_pixels(save("png"))
    assert (pw, ph) == (w, h), f"the PNG is {pw}x{ph}, not {w}x{h}"
    for i, (band, code) in enumerate(zip(BANDS, codes)):
        # the tool decodes the 8-bit codes it was handed, not the nits asked for
        expected = hdr_png_pixel(tuple(pq_decode(c / 255) for c in code))
        at = ((i * BAND + BAND // 2) * w + w // 2) * 4
        got = tuple(px[at:at + 3])
        print(f"{band} nits: codes {code} -> png {got}, expected {expected}")
        assert all(abs(a - b) <= 2 for a, b in zip(got, expected)), f"{band} nits saved as {got}, the tool's mapping gives {expected}"

    saved = save("jxl")
    assert is_jxl(saved), f"{saved} is not a JPEG XL stream"
    jw, jh, samples = jxl_pixels(s, saved)
    assert (jw, jh) == (w, h), f"the JPEG XL is {jw}x{jh}, not {w}x{h}"
    for i, code in enumerate(codes):
        at = ((i * BAND + BAND // 2) * w + w // 2) * 3
        assert samples[at:at + 3] == tuple(c * 257 for c in code), f"codes {code} saved as {samples[at:at + 3]}, not lossless"
    print("OK: an HDR session saves an sRGB PNG mapped from its PQ codes and a lossless PQ JPEG XL")
