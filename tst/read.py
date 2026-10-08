"""The reader on a PDF and a DjVu: `imread FILE` opens the document, its
pages down the list on the left, the pages themselves one under another
on the canvas, the widest as wide as the canvas; n/p and the arrows go
page by page, Home and End to the ends, the wheel scrolls, a click on a
row goes to its page; the thumbnails arrive; Tab hides the list and the
pages take its room; a file that is neither is refused, and a PDF that
does not open says so."""

import os
from pathlib import Path

from session import KEY_END, KEY_HOME, KEY_N, KEY_Q, KEY_TAB, Session, near


def pdf(pages):
    """A PDF whose pages are solid colours: (width, height, (r, g, b)) each,
    in points, as one xref'd file."""
    objects = []

    def add(body):
        objects.append(body)
        return len(objects)

    kids = []
    for width, height, (r, g, b) in pages:
        content = f"{r / 255:.3f} {g / 255:.3f} {b / 255:.3f} rg 0 0 {width} {height} re f".encode()
        stream = add(b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream")
        kids.append(add(f"<< /Type /Page /Parent {len(pages) * 2 + 2} 0 R /MediaBox [0 0 {width} {height}] /Contents {stream} 0 R >>".encode()))
    tree = add(b"<< /Type /Pages /Kids [" + b" ".join(b"%d 0 R" % kid for kid in kids) + b"] /Count %d >>" % len(kids))
    root = add(b"<< /Type /Catalog /Pages %d 0 R >>" % tree)
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for number, body in enumerate(objects, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % number + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
    for offset in offsets:
        out += b"%010d 00000 n \n" % offset
    out += b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objects) + 1, root, xref)
    return bytes(out)


def saturated(capture):
    """How many pixels of a capture are far from grey."""
    w, h, px = capture
    return sum(1 for i in range(0, w * h * 3, 3) if max(px[i:i + 3]) - min(px[i:i + 3]) > 80)


with Session("read", tool="read") as s:
    code, log = s.run()
    assert code == 2 and "usage: im read" in log, f"no arguments did not give the usage (rc={code}):\n{log}"

    docs = s.artifacts / "docs"
    docs.mkdir()
    red, green, blue = (255, 0, 0), (0, 255, 0), (0, 0, 255)
    # the second page is the widest: it sets the zoom, the others stand
    # narrower, centred
    (docs / "three.pdf").write_bytes(pdf([(200, 300, red), (300, 200, green), (200, 300, blue)]))

    s.launch(str(docs / "three.pdf"))
    s.focus()
    r = s.window()["rect"]
    side = r["width"] // 5
    canvas = (side, 0, r["width"] - side, r["height"])

    def shows(rgb, count, label, region=None):
        s.wait(lambda: near(s.capture(label, region=region or canvas), rgb) >= count, f"the canvas showing {label}")

    s.said("opened pages=3")
    s.said("page 1")
    s.said("showing page 1 ")
    # the first page, 2:3, is taller than the canvas: its top part, 512 px
    # wide, fills the view
    shows(red, 300000, "first")
    top = s.capture("top", region=canvas)
    # the wheel scrolls the column: the view moves down the first page
    s.pointer(canvas[0] + canvas[2] // 2, canvas[3] // 2)
    s.scroll(1)
    s.changed(top, "wheeled")
    s.tap(KEY_N)
    s.said("page 2")
    s.said("showing page 2 ")
    # the second page, 3:2, as wide as the canvas allows: 768x512
    shows(green, 350000, "second")
    s.tap(KEY_END)
    s.said("page 3")
    s.said("showing page 3 ")
    shows(blue, 300000, "last")
    s.tap(KEY_HOME)
    s.said("page 1", 2)
    shows(red, 300000, "home")
    # the pages down the list, each as wide as the list and shaped as the
    # page, 6 px apart: the first row 300 px tall, the second 133, the
    # third 300, the list 6 px down for the window's padding
    s.said("thumbnail 1")
    s.said("thumbnail 2")
    s.said("thumbnail 3")
    s.click(side // 2, 6 + 300 + 6 + 133 + 6 + 100)
    s.said("page 3", 2)
    shows(blue, 300000, "clicked")
    s.tap(KEY_TAB)
    s.said("panel off")
    # the canvas is the window now, and the page is laid out wider for it
    s.wait(lambda: near(s.capture("alone", region=(0, 0, r["width"], r["height"])), blue) >= 400000, "the page across the whole window")
    s.tap(KEY_TAB)
    s.said("panel on")
    s.close(KEY_Q)

    s.launch(str(Path(__file__).resolve().parent / "page.djvu"))
    s.focus()
    s.said("opened pages=1")
    s.said("showing page 1 ")
    # the 200x100 DjVu as wide as the canvas allows: a gradient of
    # saturated colours with a dark box in it, nothing of it the window's
    # own greys
    s.wait(lambda: saturated(s.capture("djvu", region=canvas)) >= 100000, "the gradient of the page")
    s.close()

    (docs / "notes").write_bytes(os.urandom(2048))
    code, log = s.run(str(docs / "notes"))
    assert code == 1 and "not a PDF or a DjVu" in log, f"a file of neither kind was not refused (rc={code}):\n{log}"

    (docs / "broken.pdf").write_bytes(b"%PDF-1.4\n" + os.urandom(2048))
    s.launch(str(docs / "broken.pdf"))
    s.focus()
    s.said("cannot open: ")
    s.close()
    print("OK: the pages of a PDF and a DjVu scroll as one column, turn by key, wheel and click, and bad files are refused")
