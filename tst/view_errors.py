"""What the viewer does with what it cannot show: no arguments is a usage
error; a path that is not there, a directory without images or one that
cannot be read leaves the list empty, the path's trouble written at its
top; a file no coder takes or a truncated one stays in the list with its
error in place of its thumbnail and in the properties panel, its
neighbours showing all the same; a file that is no image by its bytes is
not in the list, nor is a document file calls an image; a Vulkan that
fails ends the tool with the error."""

import os
import shutil
from pathlib import Path

from session import KEY_RIGHT, Session, png, write_png

with Session("view_errors", tool="view") as s:
    code, log = s.run()
    assert code == 2 and "usage: im view" in log, f"no arguments did not give the usage (rc={code}):\n{log}"

    nowhere = s.artifacts / "nowhere"
    s.launch(str(nowhere))
    s.focus()
    s.said("listed 0")
    s.said(f"{nowhere}: No such file or directory")
    s.close()

    empty = s.artifacts / "empty"
    empty.mkdir()
    s.launch(str(empty))
    s.focus()
    s.said("listed 0")
    s.close()

    locked = s.artifacts / "locked"
    locked.mkdir()
    locked.chmod(0)
    s.launch(str(locked))
    s.focus()
    s.said("listed 0")
    s.close()
    assert "opendir() failed" in s.client_log(), "the unreadable directory was not reported"

    pics = s.artifacts / "pics"
    pics.mkdir()
    # a PNG by its first bytes, which is what makes the list, and garbage after
    (pics / "bad.png").write_bytes(b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR" + os.urandom(4096))
    write_png(pics / "good.png", 64, 48, (255, 0, 0))
    whole = png(64, 48, bytes((0, 255, 0)) * (64 * 48))
    (pics / "trunc.png").write_bytes(whole[: len(whole) * 2 // 3])
    (pics / "notes.png").write_bytes(b"not an image at all\n")
    # a DjVu is image/vnd.djvu to file, and a document to us
    shutil.copy(Path(__file__).parent / "page.djvu", pics / "scan.djvu")
    s.launch(str(pics))
    s.focus()
    s.said("listed 3")
    s.said("selected bad.png")
    s.said("cannot show bad.png: ")
    s.said("no thumbnail bad.png: ")
    s.said("thumbnail good.png")
    s.tap(KEY_RIGHT)
    s.said("showing good.png 64x48")
    s.tap(KEY_RIGHT)
    s.said("selected trunc.png")
    s.said("cannot show trunc.png: ")
    s.close()

    # a Vulkan that fails at its first call ends the tool with the error
    code, log = s.run(str(pics), IM_CHAOS="vulkan=1")
    assert code == 1 and "vulkan error" in log, f"a failed Vulkan did not end the viewer with the error (rc={code}):\n{log}"
    print("OK: unusable inputs are reported, the rest shows, and a failed Vulkan ends the viewer")
