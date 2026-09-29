from session import Session

with Session("worker") as s:
    s.launch()
    s.logged("WORK 20")
    s.screenshot("worker-completed", [(.1, .36, .75, .49, "e0b040"), (.1, .6, .9, .8, "40a060")])
    assert (s.artifacts / "client.log").read_text().count("WORK ") >= 2
    s.close()
