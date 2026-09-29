from session import Session

with Session("animation") as s:
    s.launch()
    s.logged("TICK 30")
    s.screenshot("completed", [(.1, .36, .75, .49, "e0b040"), (.1, .6, .9, .8, "40a060")])
    log = (s.artifacts / "client.log").read_text()
    assert log.count("FRAME ") >= 10, "animation did not present intermediate frames"
    s.close()
