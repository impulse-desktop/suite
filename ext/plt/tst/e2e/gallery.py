from session import Session

with Session("gallery") as s:
    s.launch()
    quadrants = [(.1, .3, .4, .4, "204060"), (.6, .3, .9, .4, "c04040"),
                 (.1, .6, .4, .8, "40a060"), (.6, .6, .9, .8, "e0b040")]
    s.screenshot("initial", quadrants)
    node = s.window()
    s.ipc(f'[con_id={node["id"]}] resize set width 600 px height 400 px')
    s.wait(lambda: s.window()["rect"]["width"] == 600, "resize to 600px")
    s.ipc(f'[con_id={node["id"]}] move position 40 50')
    s.screenshot("resized", quadrants)
    s.ipc("output HEADLESS-1 mode 2048x1536")
    s.ipc("output HEADLESS-1 scale 1.5")
    s.wait(lambda: "1.500" in (s.artifacts / "client.log").read_text(), "fractional scale")
    s.ipc(f'[con_id={node["id"]}] move position 40 50')
    s.screenshot("fractional-scale", quadrants)
    s.ipc("output HEADLESS-1 scale 2")
    s.wait(lambda: "2.000" in (s.artifacts / "client.log").read_text(), "integer scale")
    s.ipc(f'[con_id={node["id"]}] move position 40 50')
    s.screenshot("integer-scale", quadrants)
    s.close()
