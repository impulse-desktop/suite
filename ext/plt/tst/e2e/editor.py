from session import Session

with Session("editor") as s:
    s.launch()
    s.focus()
    s.type("hello world")
    s.logged("TEXT hello world")
    s.screenshot("typed", [(.06, .83, .36, .89, "40a060")])
    s.key("BackSpace")
    s.key("BackSpace")
    s.logged("TEXT hello wor\n")
    s.screenshot("edited", [(.06, .83, .30, .89, "40a060"), (.34, .83, .38, .89, "204060")])
    s.close()
