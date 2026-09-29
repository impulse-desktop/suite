from session import Session

with Session("render_retry") as s:
    s.launch()
    s.logged("RETRY 3")
    s.logged("RETRY 2")
    s.logged("RETRY 1")
    s.screenshot("recovered", [(.1, .5, .9, .8, "40a060")])
    s.close()
