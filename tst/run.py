#!/usr/bin/env python3
"""Run every scenario (tst/scenarios/*.py) under its own compositor; keep
the artifacts and write results.json; continue after failures."""

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True, help="the tools binary under test (im_test)")
    parser.add_argument("--devices", type=Path, required=True, help="the driver's input devices helper")
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--filter", default="*")
    parser.add_argument("--runtime", type=Path, default=None,
                        help="where each scenario's runtime dir goes (short: it holds Wayland sockets)")
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    scenarios = sorted(Path(__file__).resolve().parent.glob(f"scenarios/{args.filter}.py"))
    if not scenarios:
        parser.error("no matching scenarios")
    results = []
    for scenario in scenarios:
        start = time.monotonic()
        output = (args.artifacts / scenario.stem).resolve()
        output.mkdir(parents=True, exist_ok=True)
        env = {
            **os.environ,
            "IM_E2E_BINARY": str(args.binary.resolve()),
            "IM_E2E_DEVICES": str(args.devices.resolve()),
            "IM_E2E_ARTIFACTS": str(output),
            "PYTHONPATH": str(Path(__file__).resolve().parent),
        }
        if args.runtime:
            args.runtime.mkdir(parents=True, exist_ok=True)
            env["IM_E2E_TMP"] = str(args.runtime.resolve())
        try:
            with (output / "driver.log").open("w") as log:
                process = subprocess.Popen([sys.executable, str(scenario)], env=env, stdout=log,
                                           stderr=subprocess.STDOUT, start_new_session=True)
                code = process.wait(timeout=args.timeout)
            status = "PASS" if code == 0 else "FAIL"
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            status = "TIMEOUT"
        results.append({"name": scenario.stem, "status": status, "seconds": round(time.monotonic() - start, 2)})
        print(f"{scenario.stem}: {status} ({results[-1]['seconds']}s)", flush=True)
        if status != "PASS":
            print((output / "driver.log").read_text(), flush=True)
    (args.artifacts / "results.json").write_text(json.dumps(results, indent=2))
    failed = [r["name"] for r in results if r["status"] != "PASS"]
    print(f"{len(results) - len(failed)} passed, {len(failed)} failed" + (": " + " ".join(failed) if failed else ""))
    return int(bool(failed))


if __name__ == "__main__":
    raise SystemExit(main())
