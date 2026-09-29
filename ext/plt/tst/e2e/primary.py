import subprocess
from session import Session

with Session("primary") as s:
    s.launch(PLT_SELECTION_TEXT="selected by real plt client")
    s.click(100, 130)
    s.logged("SELECTED")
    assert s.command("wl-paste", "--primary", "--no-newline").decode() == "selected by real plt client"
    s.screenshot("selection", [(.1, .5, .9, .8, "e0b040")])
    payload = s.artifacts / "primary.txt"
    payload.write_text("external primary selection")
    with payload.open("rb") as data:
        copy = subprocess.Popen(["wl-copy", "--foreground", "--primary", "--type", "text/plain;charset=utf-8"],
                                stdin=data, env=s.env)
    s.processes.append(copy)
    s.wait(lambda: s.command("wl-paste", "--primary", "--no-newline").decode() == payload.read_text(), "external primary")
    s.click(100, 130, "button2")
    s.logged("PRIMARY external primary selection")
    s.screenshot("middle-paste", [(.1, .5, .9, .8, "40a060")])
    s.close()
