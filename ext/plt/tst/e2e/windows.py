from session import Session

with Session("windows") as s:
    s.launch()
    s.wait(lambda: len(s.windows()) == 2, "two independent windows")
    second = s.window("plt-second")
    s.ipc(f'[con_id={second["id"]}] move position 440 50')
    s.focus("plt-first")
    s.logged("FOCUS plt-first 1")
    s.screenshot("first-focused", [(.1, .5, .9, .7, "c04040"), (.08, .81, .38, .88, "e0b040")], "plt-first")
    s.focus("plt-second")
    s.logged("FOCUS plt-second 1")
    s.screenshot("second-focused", [(.1, .5, .9, .7, "40a060"), (.08, .81, .38, .88, "e0b040")], "plt-second")
    s.screenshot("first-unfocused", [(.08, .81, .38, .88, "808080")], "plt-first")
    s.close()
