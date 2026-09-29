"""The 1280x800 capture becomes a 640x400 viewport at the editor's initial
50% zoom. The native window adds only the 200px panel and 8px inter-child
spacing. Grown by the compositor, the 0 shortcut's reset restores the
50% view and asks for the exact initial native size again."""

from session import KEY_ZERO, Session

with Session("initial_size") as s:
    shot = s.capture_file("frame.shot", 1280, 800)
    # the protocol trace is this scenario's evidence: which geometry each
    # commit carried, and what the compositor configured back
    s.launch(str(shot), WAYLAND_DEBUG="1")
    node = s.focus()
    assert s.size() == (848, 400), f"the editor opened at {s.size()}, expected 848x400"
    s.ipc(f'[con_id={node["id"]}] resize set 950 450')
    # the tool has taken the compositor's size once its committed geometry
    # says so, not once the container does: a reset that lands between the
    # configure and the tool's commit of it makes one commit ack the 950x450
    # configure while carrying an 848x400 geometry, and sway (1.10) then
    # keeps the container at 950x450 with the tool's 848x400 inside for good
    s.wait(lambda: s.size() == (950, 450) and s.geometry() == (950, 450), "the grown window, taken by the tool")
    s.tap(KEY_ZERO)
    s.wait(lambda: s.size() == (848, 400), "the initial size after the reset")
    s.close()
    print("OK: the editor opens at the viewport plus its chrome and the reset asks for that size again")
