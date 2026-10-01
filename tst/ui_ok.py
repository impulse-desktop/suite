"""The tool runtime's skeleton: `im ui` is one window with one button, its
frames the events it iterates. A click on OK ends the tool; so does the
compositor closing the window."""

from session import Session

with Session("ui_ok", tool="ui") as s:
    s.launch()
    s.focus()
    s.said("presenting")
    w, h = s.size()
    s.click(w // 2, h // 2)
    s.said("ok")
    s.gone()
    assert s.finished() == 0, "OK did not end the tool cleanly"

    s.launch()
    node = s.focus()
    s.said("presenting")
    s.ipc(f'[con_id={node["id"]}] kill')
    s.said("closed")
    s.gone()
    assert s.finished() == 0, "a closed window did not end the tool cleanly"
    print("OK: the button and the compositor's close both end the tool")
