"""The HDR editor blends its crop veil in linear light. The capture is a
PQ frame whose bands step through known luminances. On screen the
compositor draws the PQ surface however it does, but the same way for
every pixel, so the unveiled bands give its response to each luminance. A
selection then veils the rest of the image with 140/255 black: a veiled
band of N nits must look like the compositor's rendering of N times
(1 - 140/255), a blend in linear light, not of N's PQ code scaled by the
same factor, a blend in code space; the two are far apart."""

from session import Session, pq_decode, pq_encode

# black and the low bands are there for the predictions to fall between
# bands measured; the veil is judged on the bands from 32 nits up
NITS = [0, 4, 8, 16, 32, 64, 128, 203, 256, 512, 1000]
JUDGED = [32, 64, 128, 203, 256, 512]
ALPHA = 140 / 255
# the canvas begins past the 200px panel and 8px of spacing; the frame is
# shown at 50%
CANVAS_X = 208


def response(capture, y):
    """The screen's colour of the band drawn at row y, away from the veil's
    selection: the mean over a few pixels of one column."""
    w, h, px = capture
    x = CANVAS_X + 60
    samples = [px[((y + dy) * w + x) * 3:((y + dy) * w + x) * 3 + 3] for dy in range(-3, 4)]
    return tuple(sum(p[c] for p in samples) / len(samples) for c in range(3))


def interpolate(table, nits):
    """The compositor's rendering of a luminance between the bands measured,
    the outermost bands standing for anything beyond them."""
    nits = min(max(nits, min(table)), max(table))
    below = max(n for n in table if n <= nits)
    above = min(n for n in table if n >= nits)
    if above == below:
        return table[below]
    t = (nits - below) / (above - below)
    return tuple(a + (b - a) * t for a, b in zip(table[below], table[above]))


with Session("linear_blend") as s:
    s.require("wp_color_manager_v1")
    w, h = 1280, 800
    band = h // len(NITS)
    pixels = b"".join(bytes([round(pq_encode(n) * 255)] * 3 + [255]) * (w * band) for n in NITS)
    shot = s.capture_file("bands.shot", w, h, pixels=pixels)
    s.launch(str(shot), IMWAY_SHOT_COLOR="1:203")
    s.focus()
    s.said("surface HDR10 PQ")
    # the pointer parked over the panel's text, away from the canvas
    s.pointer(100, 300)
    base = s.settled("base")
    rows = {n: i * band // 2 + band // 4 for i, n in enumerate(NITS)}
    table = {n: response(base, rows[n]) for n in NITS}
    print("response:", {n: tuple(round(v) for v in rgb) for n, rgb in table.items()})
    # a selection on the right: everything else, the measured column too,
    # goes under the veil
    s.drag(CANVAS_X + 400, 60, CANVAS_X + 600, 340)
    s.changed(base, "veiled")
    veiled = s.settled("veiled")
    closer = 0
    for n in JUDGED:
        actual = response(veiled, rows[n])
        linear = interpolate(table, n * (1 - ALPHA))
        in_code = interpolate(table, pq_decode(pq_encode(n) * (1 - ALPHA)))
        linear_error = sum(abs(a - e) for a, e in zip(actual, linear))
        code_error = sum(abs(a - e) for a, e in zip(actual, in_code))
        print(f"{n} nits veiled: {tuple(round(v) for v in actual)} linear {tuple(round(v) for v in linear)} ({linear_error:.0f}) code-space {tuple(round(v) for v in in_code)} ({code_error:.0f})")
        closer += linear_error < code_error
    assert closer >= len(JUDGED) - 1, f"only {closer} of {len(JUDGED)} bands are veiled in linear light"
    s.close()
    print("OK: the HDR editor's crop veil is blended in linear light")
