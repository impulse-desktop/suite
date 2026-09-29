#!/usr/bin/env python3
"""Run each C++ program through its paired Python scenario; preserve artifacts."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import signal
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary-dir", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--renderer", choices=("shm", "lavapipe", "metal"), default="shm")
    parser.add_argument("--filter", default="*")
    parser.add_argument("--source-dir", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    source = args.source_dir.resolve()
    scenarios = [p for p in sorted(source.glob(args.filter + ".py")) if p.with_suffix(".cpp").exists()]
    if not scenarios:
        parser.error("no matching C++/Python scenario pairs")
    results = []
    for scenario in scenarios:
        start = time.monotonic()
        output = (args.artifacts / scenario.stem).resolve()
        output.mkdir(parents=True, exist_ok=True)
        env = {**os.environ, "PLT_E2E_BINARY": str((args.binary_dir / scenario.stem).resolve()),
               "PLT_E2E_DEVICES": str((args.binary_dir / "devices").resolve()),
               "PLT_E2E_ARTIFACTS": str(output), "PLT_E2E_RENDERER": args.renderer}
        try:
            with (output / "driver.log").open("w") as log:
                process = subprocess.Popen([sys.executable, str(scenario)], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                code = process.wait(timeout=90)
            status = "PASS" if code == 0 else "FAIL"
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            status = "TIMEOUT"
        results.append({"name": scenario.stem, "renderer": args.renderer, "status": status, "seconds": round(time.monotonic() - start, 2)})
        print(f"{scenario.stem} ({args.renderer}): {status}", flush=True)
        if status != "PASS":
            print((output / "driver.log").read_text(), flush=True)
    (args.artifacts / "results.json").write_text(json.dumps(results, indent=2))
    return int(any(result["status"] != "PASS" for result in results))


if __name__ == "__main__":
    raise SystemExit(main())
