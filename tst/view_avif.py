"""An AVIF opens in the viewer: the decoder's module takes it through
libheif and libaom's decoder, and the viewer shows it at its size and
lists its thumbnail. The file is the module's own test corpus's, 320x240
with alpha."""

import shutil
from pathlib import Path

from session import Session

with Session("view_avif", tool="view") as s:
    pics = s.artifacts / "pics"
    pics.mkdir()
    shutil.copy(Path(__file__).resolve().parent / "alpha.avif", pics / "alpha.avif")
    s.launch(str(pics))
    s.focus()
    s.said("listed 1")
    s.said("showing alpha.avif 320x240")
    s.said("thumbnail alpha.avif")
    s.close()
    print("OK: an AVIF decodes and shows")
