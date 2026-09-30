"""A save with no screenshot directory and no filename template configured
lands under $XDG_PICTURES_DIR/screenshots (created on demand) with the
default imway-YYYYMMDD-HHMMSS name."""

import re

from session import Session, png_size

with Session("save_defaults") as s:
    shot = s.capture_file("good.shot", 64, 48)
    pics = s.artifacts / "pics"
    code, log = s.run(str(shot), cwd=str(s.artifacts), unset=("IM_SHOT_DIR",),
                      XDG_PICTURES_DIR="pics", IM_SHOT_NAME="", IM_SHOT_FORMAT="png", IM_SHOT_ACTION="save")
    assert code == 0, f"the save failed ({code}):\n{log}"
    saved = sorted((pics / "screenshots").glob("imway-*.png")) if (pics / "screenshots").is_dir() else []
    assert saved, f"the save did not land in the default place:\n{log}"
    assert re.fullmatch(r"imway-\d{8}-\d{6}\.png", saved[0].name), f"unexpected default name: {saved[0].name}"
    assert png_size(saved[0]) == (64, 48), f"{saved[0]} is not the 64x48 capture"
    print("OK: an unconfigured save goes to $XDG_PICTURES_DIR/screenshots/imway-<stamp>.png")
