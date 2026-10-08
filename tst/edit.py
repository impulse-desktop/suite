"""The editor: `imedit FILE` opens the file, or starts one that is not
there; typing changes it, a copy and a paste go through the compositor's
clipboard, Ctrl+S writes the file as typed and ends it with a newline,
Ctrl+Q leaves a clean file at once and asks over an unsaved one, where
Discard throws the changes away and Save keeps them; a window closed from
outside with unsaved changes leaves them next to the file under the
backup name; the language comes from the extension."""

from session import KEY_A, KEY_C, KEY_D, KEY_END, KEY_LEFTCTRL, KEY_Q, KEY_S, KEY_V, Session


def chord(s, code, client=True):
    """Ctrl with a key."""
    s.key(KEY_LEFTCTRL, 1)
    s.tap(code, client=client)
    s.key(KEY_LEFTCTRL, 0, client=client)


with Session("edit", tool="edit") as s:
    code, log = s.run()
    assert code == 2 and "usage: im edit" in log, f"no arguments did not give the usage (rc={code}):\n{log}"

    docs = s.artifacts / "docs"
    docs.mkdir()
    notes = docs / "notes.txt"

    # a file that is not there yet: typed, saved as typed, left cleanly
    s.launch(str(notes))
    s.focus()
    s.said("new notes.txt")
    s.said("language None")
    s.type("hello, world\nline two")
    s.said("modified")
    # the clipboard goes through the compositor: Ctrl+A, Ctrl+C set the
    # selection, the compositor announces it and the tool reads it back,
    # End, Ctrl+V paste what was read at the end
    chord(s, KEY_A)
    chord(s, KEY_C)
    s.said("clipboard set bytes=21")
    s.said("clipboard read bytes=21")
    s.tap(KEY_END)
    chord(s, KEY_V)
    chord(s, KEY_S)
    s.said("saved notes.txt bytes=43")
    assert notes.read_text() == "hello, world\nline two" * 2 + "\n", f"the file is not what was typed and pasted: {notes.read_text()!r}"
    chord(s, KEY_Q, client=False)
    s.said("quit")
    s.gone()
    assert s.finished() == 0, "the editor did not exit cleanly"

    # an existing C++ file: opened with its language, a change discarded
    source = docs / "main.cpp"
    original = "int main() {\n    return 0;\n}\n"
    source.write_text(original)
    s.launch(str(source))
    s.focus()
    s.said("opened main.cpp lines=4")
    s.said("language C++")
    s.type("// ")
    s.said("modified")
    chord(s, KEY_Q)
    s.said("asking")
    s.tap(KEY_D, client=False)
    s.said("discarded")
    s.said("quit")
    s.gone()
    assert s.finished() == 0, "the editor did not exit cleanly"
    assert source.read_text() == original, "Discard changed the file"

    # the same change saved from the question
    s.launch(str(source))
    s.focus()
    s.said("opened main.cpp lines=4")
    s.type("// ")
    s.said("modified")
    chord(s, KEY_Q)
    s.said("asking")
    s.tap(KEY_S, client=False)
    s.said("saved main.cpp bytes=34")
    s.said("quit")
    s.gone()
    assert s.finished() == 0, "the editor did not exit cleanly"
    assert source.read_text() == "// " + original, f"Save did not write the change: {source.read_text()!r}"

    # the window closed by the compositor with unsaved changes: the file
    # stays, the changes go to the backup name
    s.launch(str(source))
    s.focus()
    s.said("opened main.cpp lines=4")
    s.type("x")
    s.said("modified")
    node = s.window()
    s.ipc(f'[con_id={node["id"]}] kill')
    s.gone()
    assert s.finished() == 0, "the editor did not exit cleanly"
    assert source.read_text() == "// " + original, "the closed window changed the file"
    backup = docs / "main.cpp~"
    assert backup.read_text() == "x// " + original, f"the backup is not the unsaved text: {backup.read_text()!r}"
    assert "unsaved changes kept in" in s.client_log(), "the backup was not announced"
    print("OK: the editor opens, types, saves, asks over unsaved changes and keeps them on a close")
