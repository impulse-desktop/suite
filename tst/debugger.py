import os
from pathlib import Path
import signal
import sys
from session import Session, debugger

with Session("debugger") as s:
    if not debugger() and sys.platform != "darwin":
        s.skip("no debugger runs the tools here")
    helper = Path(os.environ["IM_E2E_HELPERS"]) / "fault"
    code, log = s.run("exit", "3", command=helper)
    assert code == 3, f"exit status {code}\n{log}"
    assert "fault: none" in log, log
    code, log = s.run("fault", command=helper)
    if debugger():
        stacks = s.artifacts / f"client{s.clients}-stacks.log"
        log += stacks.read_text(errors="replace") if stacks.exists() else ""
        assert code == 128 + signal.SIGSEGV, f"exit status {code}\n{log}"
        assert "fault.cpp:" in log, log
    else:
        crash = s.artifacts / f"client{s.clients}-crash.log"
        assert code == -signal.SIGSEGV, f"exit status {code}\n{log}"
        assert crash.exists(), "no crash report for the fault"
        assert "fault" in crash.read_text(errors="replace"), crash.read_text(errors="replace")
    print(f"exit status {code}, the stacks in the log")
