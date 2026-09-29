from session import Session

with Session("clipboard") as s:
    s.launch(PLT_COPY_TEXT="external reader sees plt")
    s.focus()
    s.key("c", "ctrl")
    s.logged("COPIED")
    assert s.command("wl-paste", "--no-newline").decode() == "external reader sees plt"
    s.screenshot("copied", [(.1, .5, .9, .8, "e0b040")])
    # An independent real Wayland client becomes the new clipboard owner.
    payload = s.artifacts / "clipboard.txt"
    payload.write_text("external writer to plt")
    import subprocess
    with payload.open("rb") as data:
        copy = subprocess.Popen(["wl-copy", "--foreground", "--type", "text/plain;charset=utf-8"],
                                stdin=data, env=s.env)
    s.processes.append(copy)
    s.wait(lambda: s.command("wl-paste", "--no-newline").decode() == payload.read_text(), "external clipboard")
    s.key("v", "ctrl")
    s.logged("PASTED external writer to plt")
    s.screenshot("pasted", [(.1, .5, .9, .8, "40a060")])
    s.close()
