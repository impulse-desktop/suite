"""Run a command with a hard timeout, killing its whole process group.

Guards test invocations in build.py. timeout(1) is not usable here: the
build runner resolves argv[0] through symlinks, which breaks multi-call
coreutils, and macOS has no timeout(1) at all.
"""
import os
import signal
import shutil
import subprocess
import sys


def main():
    limit = float(sys.argv[1])
    argv = sys.argv[2:]
    process = subprocess.Popen(argv, start_new_session=True)
    try:
        status = process.wait(limit)
        if status < 0 and sys.platform == "darwin" and shutil.which("lldb"):
            print(f"test terminated by signal {-status}; collecting LLDB backtrace", flush=True)
            subprocess.run(
                ["lldb", "--batch", "-o", "run", "-o", "thread backtrace all", "--", *argv],
                timeout=30, check=False,
            )
        sys.exit(status)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        print(f"timed out after {limit}s: {' '.join(argv)}", file=sys.stderr)
        sys.exit(124)


if __name__ == "__main__":
    main()
