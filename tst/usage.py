"""The one binary's dispatch. `im` alone, `im` with a tool it has not got
and `im screenshot` without a path each print their usage and exit 2, and
so does the link imscreenshot without a path; `im screenshot PATH` runs
the tool by its first word as the link does by its name."""

from session import Session, png_size

with Session("usage") as s:
    shot = s.capture_file("good.shot", 64, 48)
    shots = s.artifacts / "shots"

    code, log = s.run(command=s.binary)
    assert code == 2 and "usage: im <tool> [args...]; tools: screenshot" in log, f"im alone: rc={code}:\n{log}"
    code, log = s.run("view", command=s.binary)
    assert code == 2 and "usage: im <tool>" in log, f"an unknown tool: rc={code}:\n{log}"
    code, log = s.run("screenshot", command=s.binary)
    assert code == 2 and "usage: im screenshot <path|fd:N>" in log, f"im screenshot without a path: rc={code}:\n{log}"
    code, log = s.run()
    assert code == 2 and "usage: im screenshot <path|fd:N>" in log, f"imscreenshot without a path: rc={code}:\n{log}"

    code, log = s.run("screenshot", str(shot), command=s.binary, IMWAY_SHOT_ACTION="save", IMWAY_SHOT_FORMAT="png",
                      IMWAY_SHOT_DIR=str(shots), IMWAY_SHOT_NAME="byword")
    assert code == 0 and png_size(shots / "byword.png") == (64, 48), f"im screenshot PATH did not save (rc={code}):\n{log}"
    print("OK: the binary prints its usage where it has no tool or no path, and runs a tool by its first word")
