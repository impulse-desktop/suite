from session import Session

with Session("scroller") as s:
    s.launch()
    s.screenshot("before", [(.1, .5, .9, .8, "204060")])
    s.pointer(160, 130)
    s.button("button5")
    s.logged("SCROLL ")
    s.screenshot("scrolled", [(.1, .5, .9, .8, "40a060")])
    s.close()
