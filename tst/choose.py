"""The chooser: `imchoose DIR` lists the directory, the line at the bottom
holds its path and what is typed after the slash narrows the list; Enter
on a directory enters it, on a file takes it and the path comes out on
stdout with status 0; Escape leaves with status 1 and nothing; a filter
of globs or types keeps the rest out; `--save --name N` answers with the
name typed, asking before an existing file is replaced; `--directory`
answers with the directory; Ctrl+G shows the grid with thumbnails."""

from session import KEY_BACKSPACE, KEY_DOWN, KEY_END, KEY_ENTER, KEY_ESC, KEY_G, KEY_LEFTCTRL, Session, write_png


def answer(s):
    """The tool's stdout lines that are not its own trace."""
    return [line for line in s.client_log().splitlines() if line and not line.startswith("im choose:")]


with Session("choose", tool="choose") as s:
    code, log = s.run("--filter", "broken")
    assert code == 2 and "usage: im choose" in log, f"a bad filter did not give the usage (rc={code}):\n{log}"

    root = s.artifacts / "root"
    (root / "pics" / "deep").mkdir(parents=True)
    write_png(root / "pics" / "a.png", 32, 24, (255, 0, 0))
    write_png(root / "pics" / "b.png", 32, 24, (0, 255, 0))
    (root / "pics" / "notes.txt").write_text("hello\n")
    (root / "pics" / ".hidden").write_text("x\n")
    (root / "readme.txt").write_text("top\n")

    # a file taken by its row: the arrow stands on it, Enter takes it
    s.launch("--filter", "Images|*.png|image/*", "--filter", "All|*", str(root / "pics"))
    s.focus()
    s.said(f"going {root / 'pics'}")
    s.said("listed 5")
    s.said("showing 3")
    s.tap(KEY_DOWN)
    s.said("selected deep")
    s.tap(KEY_DOWN)
    s.said("selected a.png")
    s.tap(KEY_ENTER, client=False)
    s.said("chosen 1")
    s.gone()
    assert s.finished() == 0, "the chooser did not exit 0 after a choice"
    assert answer(s) == [str(root / "pics" / "a.png")], f"the answer is not the file: {answer(s)!r}"

    # typing after the slash narrows the list, and Enter takes the one left
    s.launch("--filter", "Images|*.png", str(root / "pics"))
    s.focus()
    s.said("showing 3")
    s.type("b")
    s.said("showing 1")
    s.tap(KEY_ENTER, client=False)
    s.said("chosen 1")
    s.gone()
    assert s.finished() == 0
    assert answer(s) == [str(root / "pics" / "b.png")], f"the narrowed answer is wrong: {answer(s)!r}"

    # Backspace on an empty name goes up, a typed directory goes down, Escape leaves
    s.launch(str(root / "pics"))
    s.focus()
    s.said("listed 5")
    s.tap(KEY_BACKSPACE)
    s.said(f"going {root}")
    s.said("listed 2")
    s.type("pics/")
    s.said(f"going {root / 'pics'}", 2)
    s.said("listed 5", 2)
    s.tap(KEY_ESC, client=False)
    s.said("cancelled")
    s.gone()
    assert s.finished() == 1, "Escape did not exit 1"
    assert answer(s) == [], f"Escape answered: {answer(s)!r}"

    # save: the name typed is the answer; an existing one asks first
    s.launch("--save", "--name", "out.png", str(root / "pics"))
    s.focus()
    s.said("listed 5")
    s.tap(KEY_ENTER, client=False)
    s.said("chosen 1")
    s.gone()
    assert s.finished() == 0
    assert answer(s) == [str(root / "pics" / "out.png")], f"the save answer is wrong: {answer(s)!r}"

    s.launch("--save", "--name", "a.png", str(root / "pics"))
    s.focus()
    s.said("listed 5")
    s.tap(KEY_ENTER)
    s.said("asking to replace")
    s.tap(KEY_ENTER, client=False)
    s.said("chosen 1")
    s.gone()
    assert s.finished() == 0
    assert answer(s) == [str(root / "pics" / "a.png")], f"the replace answer is wrong: {answer(s)!r}"

    # directory: Enter with nothing typed is the directory shown
    s.launch("--directory", str(root / "pics"))
    s.focus()
    s.said("listed 5")
    s.said("showing 1")
    s.tap(KEY_ENTER, client=False)
    s.said("chosen 1")
    s.gone()
    assert s.finished() == 0
    assert answer(s) == [str(root / "pics")], f"the directory answer is wrong: {answer(s)!r}"

    # the grid draws thumbnails of the images
    s.launch("--filter", "Images|image/*", str(root / "pics"))
    s.focus()
    s.said("showing 3")
    s.key(KEY_LEFTCTRL, 1)
    s.tap(KEY_G)
    s.key(KEY_LEFTCTRL, 0)
    s.said("grid on")
    s.said("thumbnail a.png")
    s.said("thumbnail b.png")
    s.tap(KEY_END)
    s.tap(KEY_ESC, client=False)
    s.gone()
    assert s.finished() == 1
    print("OK: the chooser lists, narrows, walks, saves, picks a directory and draws thumbnails")
