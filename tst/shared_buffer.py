"""The capture as a KMS session hands it over: the scanout buffer itself,
a dma-buf on fd 3, its layout and its exporting device in
IM_SHOT_DMABUF. A udmabuf stands in for the scanout here: the tool
imports it on the device the UUID names, shows it, and reads the
selection back from it for a PNG and a JPEG XL; with HDR colour metadata
the PNG comes from the readback's 16-bit samples."""

from session import Session, hdr_png_pixel, jxl_pixels, png_pixels, pq_decode, udmabuf

with Session("shared_buffer") as s:
    w, h = 64, 48
    # B8G8R8A8 (VkFormat 44): B, G, R, A in memory, every pixel its own
    rgb = [((x * 4) & 255, (y * 5) & 255, ((x ^ y) * 3) & 255) for y in range(h) for x in range(w)]
    fd = udmabuf(bytes(b for r, g, b_ in rgb for b in (b_, g, r, 255)))
    if fd is None:
        s.skip("the kernel offers no udmabuf")
    s.require_import(44)
    spec = f"{w}:{h}:44:0:{w * 4}:0:{w * h * 4}:{s.device_uuid()}"
    shots = s.artifacts / "shots"
    probes = [(0, 0), (17, 5), (63, 47), (40, 20)]

    def save(name, fmt, **colour):
        code, log = s.run("fd:3", fd3=fd, IM_SHOT_DMABUF=spec, IM_SHOT_ACTION="save", IM_SHOT_FORMAT=fmt,
                          IM_SHOT_DIR=str(shots), IM_SHOT_NAME=name, **colour)
        assert code == 0, f"{name}: the save from the shared buffer failed (rc={code}):\n{log}"
        return shots / f"{name}.{fmt}"

    pw, ph, px = png_pixels(save("shared", "png"))
    assert (pw, ph) == (w, h), f"the PNG is {pw}x{ph}, not {w}x{h}"
    for x, y in probes:
        at = (y * w + x) * 4
        assert tuple(px[at:at + 4]) == rgb[y * w + x] + (255,), f"pixel {x},{y} read back as {tuple(px[at:at + 4])}, written {rgb[y * w + x]}"

    jw, jh, samples = jxl_pixels(s, save("shared", "jxl"))
    assert (jw, jh) == (w, h), f"the JPEG XL is {jw}x{jh}, not {w}x{h}"
    for x, y in probes:
        at = (y * w + x) * 3
        assert samples[at:at + 3] == tuple(c * 257 for c in rgb[y * w + x]), f"pixel {x},{y} read back as {samples[at:at + 3]}"

    # the editor shows the imported buffer
    s.launch("fd:3", fd3=fd, IM_SHOT_DMABUF=spec)
    s.focus()

    def drawn():
        vw, vh, capture = s.capture("shown")
        colours = {capture[(yy * vw + xx) * 3:(yy * vw + xx) * 3 + 3] for yy in range(2, vh - 2, 5) for xx in range(vw * 3 // 4, vw - 2, 3)}
        return len(colours) > 8

    s.wait(drawn, "the editor showing the shared buffer")
    s.close()

    if "wp_color_manager_v1" in s.globals:
        # HDR: the PNG comes from the readback's 16-bit samples, PQ decoded
        pw, ph, px = png_pixels(save("shared-hdr", "png", IM_SHOT_COLOR="1:203"))
        for x, y in probes:
            at = (y * w + x) * 4
            expected = hdr_png_pixel(tuple(pq_decode(c / 255) for c in rgb[y * w + x]))
            got = tuple(px[at:at + 3])
            assert all(abs(a - b) <= 2 for a, b in zip(got, expected)), f"HDR pixel {x},{y} saved as {got}, the tool's mapping gives {expected}"
    print("OK: a shared dma-buf is imported, shown and read back for PNG and JPEG XL")
