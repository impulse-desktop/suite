"""`im screenshot` saving with what its environment leaves out or
overstates: a lossy JPEG XL quality below 1 or above 100 is held to that
range (the low one saves far smaller than the high one, an unset one lands
in between), with neither a screenshot directory nor XDG_PICTURES_DIR the
file lands under $HOME/Pictures/screenshots, and without HOME or a name
either, under ./Pictures/screenshots with the stamped default name."""

import random
import re

from session import Session, is_jxl, png_size

with Session("save_env") as s:
    w, h = 256, 192
    rnd = random.Random(7)
    # noise, so the quality setting has something to throw away
    noise = bytes(b for _ in range(w * h) for b in (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), 255))
    shot = s.capture_file("noise.shot", w, h, pixels=noise)
    shots = s.artifacts / "shots"

    def save(what, expected, **env):
        code, log = s.run(str(shot), unset=("XDG_PICTURES_DIR",), IMWAY_SHOT_ACTION="save", **env)
        assert code == 0 and expected.is_file() and expected.stat().st_size, f"{what}: nothing saved at {expected} (rc={code}):\n{log}"

    save("quality 0", shots / "low.jxl", IMWAY_SHOT_DIR=str(shots), IMWAY_SHOT_NAME="low", IMWAY_SHOT_FORMAT="jxl", IMWAY_SHOT_LOSSLESS="0", IMWAY_SHOT_QUALITY="0")
    save("quality 150", shots / "high.jxl", IMWAY_SHOT_DIR=str(shots), IMWAY_SHOT_NAME="high", IMWAY_SHOT_FORMAT="jxl", IMWAY_SHOT_LOSSLESS="0", IMWAY_SHOT_QUALITY="150")
    save("no quality", shots / "default.jxl", IMWAY_SHOT_DIR=str(shots), IMWAY_SHOT_NAME="default", IMWAY_SHOT_FORMAT="jxl", IMWAY_SHOT_LOSSLESS="0")
    low, high, default = ((shots / f"{name}.jxl").stat().st_size for name in ("low", "high", "default"))
    print(f"jxl sizes: quality 0 -> {low}, unset -> {default}, quality 150 -> {high}")
    assert all(is_jxl(shots / f"{name}.jxl") for name in ("low", "high", "default")), "not JPEG XL streams"
    assert low * 2 < high, "the clamped qualities did not encode differently"
    assert low < default < high, "an unset quality is not the default between the extremes"

    home = s.artifacts / "home"
    home.mkdir()
    save("no directory at all", home / "Pictures" / "screenshots" / "homed.png", HOME=str(home), IMWAY_SHOT_DIR="", IMWAY_SHOT_NAME="homed", IMWAY_SHOT_FORMAT="png")

    # nothing set at all, not even HOME, and an empty XDG_PICTURES_DIR: the
    # stamped default name under ./Pictures/screenshots
    bare = s.artifacts / "bare"
    bare.mkdir()
    code, log = s.run(str(shot), cwd=str(bare), unset=("IMWAY_SHOT_DIR", "IMWAY_SHOT_NAME", "HOME"),
                      XDG_PICTURES_DIR="", IMWAY_SHOT_ACTION="save", IMWAY_SHOT_FORMAT="png")
    stamped = sorted((bare / "Pictures" / "screenshots").glob("imway-*.png")) if (bare / "Pictures" / "screenshots").is_dir() else []
    assert code == 0 and stamped, f"nothing set: no stamped file under ./Pictures/screenshots (rc={code}):\n{log}"
    assert re.fullmatch(r"imway-\d{8}-\d{6}\.png", stamped[0].name), f"nothing set: not the default name ({stamped[0].name})"
    assert png_size(stamped[0]) == (w, h)
    print("OK: out-of-range quality is clamped and HOME is the last directory fallback")
