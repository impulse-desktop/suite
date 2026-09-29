import os
from pathlib import Path
from session import Session

with Session("pipe_dashboard") as s:
    pipe = Path(s.env["XDG_RUNTIME_DIR"]) / "telemetry.fifo"
    pipe.unlink(missing_ok=True)
    os.mkfifo(pipe)
    s.launch(PLT_DATA_PIPE=str(pipe))
    with pipe.open("w", buffering=1) as writer:
        writer.write("30\n")
        s.logged("VALUE 30")
        s.screenshot("progress-30", [(.06, .38, .25, .48, "40a060"), (.4, .38, .7, .48, "204060")])
        writer.write("90\n")
        s.logged("VALUE 90")
        s.screenshot("progress-90", [(.06, .38, .7, .48, "40a060")])
    s.close()
