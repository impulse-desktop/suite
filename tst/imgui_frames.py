"""ImGui gets frames only from input, local requests and independently scheduled timers."""
import os
from pathlib import Path
import subprocess

subprocess.run([str(Path(os.environ["IM_E2E_HELPERS"]) / "imgui_frames_test")], check=True)
