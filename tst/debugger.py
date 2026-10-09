import os
from pathlib import Path
import signal
from session import Session, debugger

with Session("debugger") as s:
    if not debugger():
        s.skip("no debugger runs the tools here")
    helper = Path(os.environ["IM_E2E_HELPERS"]) / "fault"
    code, log = s.run("exit", "3", command=helper)
    assert code == 3, f"exit status {code}\n{log}"
    assert "fault: none" in log, log
    code, log = s.run("fault", command=helper)
    assert code == 128 + signal.SIGSEGV, f"exit status {code}\n{log}"
    assert "fault.cpp:" in log, log
    print(f"exit status {code}, the stacks in the log")
