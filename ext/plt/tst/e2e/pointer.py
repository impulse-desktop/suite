from session import Session

with Session("pointer") as s:
    s.launch()
    s.click(100, 120)
    s.logged("POINTER 100 120 1 1")
    s.screenshot("click", [(.06, .83, .29, .89, "40a060"), (.23, .4, .27, .45, "e0b040")])
    s.pointer(100, 120)
    s.button()
    s.pointer(220, 160)
    s.logged("DRAG 220 160")
    s.button(pressed=False)
    s.screenshot("drag", [(.53, .55, .57, .59, "e0b040"), (.23, .4, .27, .45, "204060")])
    s.close()
