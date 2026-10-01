"""The tool runtime's skeleton: `im ui` is one window with one button, its
frames the events it iterates. A click on OK ends the tool; so does the
compositor closing the window."""

from session import Session

with Session("ui_ok", tool="ui") as s:
    s.launch()
    s.focus()
    s.said("presenting")
    w, h = s.size()
    # the release is the click, and the tool is gone by the time it lands
    s.pointer(w // 2, h // 2)
    s.button()
    s.button(pressed=False, client=False)
    s.gone()
    assert s.finished() == 0, "OK did not end the tool cleanly"
    assert "im ui: ok" in s.client_log(), "the tool did not end by its button"

    s.launch()
    node = s.focus()
    s.said("presenting")
    s.ipc(f'[con_id={node["id"]}] kill')
    s.gone()
    assert s.finished() == 0, "a closed window did not end the tool cleanly"
    assert "im ui: closed" in s.client_log(), "the close did not reach the tool"
    print("OK: the button and the compositor's close both end the tool")
