#!/usr/bin/env python3
# One scenario run, emitting a JSON verdict.
#
# This is a graph node's command (see build.py): the tool under test and the
# devices helper are dependencies, so they are built by the time we run. The
# scenario gets a scratch dir of its own for its compositor, logs and
# captures, runs under a timeout, and its verdict {status, seconds, detail,
# artifacts} goes to --out with the tails of the logs it left behind. With
# --evidence DIR, what a failed scenario captured (its PNGs, with the window
# trees they came with) and its whole logs are kept under DIR/<name>/: the
# graph knows only the verdict, this is for the person reading a failure.
#
# We ALWAYS exit 0: a failure is recorded in the JSON, not in the process
# exit code, so the build graph does not abort and every scenario still
# runs. The aggregator node reads all the JSONs and produces the verdict that
# fails the build.

import argparse
import json
import os
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time

PASS, FAIL, TIMEOUT, SKIP = "PASS", "FAIL", "TIMEOUT", "SKIP"
# a scenario that found the compositor short of what it needs exits with
# this, its last line saying what
SKIPPED = 77


def tail(path, lines):
    try:
        with open(path, errors="replace") as f:
            return "".join(f.readlines()[-lines:])
    except OSError:
        return ""


def last_line(text):
    lines = [line for line in text.strip().splitlines() if line.strip()]
    return lines[-1] if lines else ""


def collect(artifacts):
    """The evidence a failed scenario left: its own account, what the tool,
    the devices and the compositor said; embedded so the verdict stands on
    its own once the scratch dir is gone."""
    evidence = {}
    names = ["driver.log", "devices.log", "sway.log"]
    names += sorted(name for name in os.listdir(artifacts) if name.startswith("client") and name.endswith(".log"))
    for name in names:
        text = tail(os.path.join(artifacts, name), 60 if name == "driver.log" else 30)
        if text.strip():
            evidence[name] = text
    return evidence


def open_to_all(path):
    """A directory the scenario writes into when it runs as another user."""
    os.chmod(path, stat.S_IRWXU | stat.S_IRWXG | stat.S_IRWXO | stat.S_ISVTX)


def run(name, args):
    # the compositor refuses to run as root: a container builds as root and
    # runs the scenario as nobody, with the directories it writes open to it
    as_nobody = os.geteuid() == 0
    artifacts = tempfile.mkdtemp(prefix="im-")
    env = {
        **os.environ,
        "IM_E2E_BINARY": os.path.abspath(args.binary),
        "IM_E2E_HELPERS": os.path.abspath(args.helpers),
        "IM_E2E_ARTIFACTS": artifacts,
        # the fixture, tst/session.py, next to the scenario
        "PYTHONPATH": os.path.dirname(os.path.abspath(args.scenario)),
    }
    if args.runtime:
        os.makedirs(args.runtime, exist_ok=True)
        env["IM_E2E_TMP"] = os.path.abspath(args.runtime)
    if args.bucket:
        env["IM_E2E_BUCKET"] = args.bucket
    # a coverage run names every process's profile after this scenario:
    # pids are reused over a run, and two processes sharing a name means
    # one of them is lost
    profile = env.get("LLVM_PROFILE_FILE")
    if profile:
        env["LLVM_PROFILE_FILE"] = os.path.join(os.path.dirname(profile), f"{name}-%p.profraw")
    if as_nobody:
        open_to_all(artifacts)
        if args.runtime:
            open_to_all(args.runtime)
    started = time.monotonic()
    log = os.path.join(artifacts, "driver.log")
    command = [sys.executable, args.scenario]
    if as_nobody:
        command = ["runuser", "-u", "nobody", "--", *command]
    with open(log, "w") as out:
        # its own session: a timeout kills the compositor and the devices
        # the scenario started along with it
        process = subprocess.Popen(command, env=env, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=args.timeout)
            status = PASS if code == 0 else SKIP if code == SKIPPED else FAIL
            detail = "" if code == 0 else last_line(tail(log, 5)) if code == SKIPPED else f"scenario rc={code}: {last_line(tail(log, 5))}"
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            status, detail = TIMEOUT, f"no verdict after {args.timeout:g}s: {last_line(tail(log, 5))}"
    seconds = time.monotonic() - started
    evidence = collect(artifacts) if status not in (PASS, SKIP) else {}
    if args.evidence:
        kept = os.path.join(args.evidence, name)
        shutil.rmtree(kept, ignore_errors=True)
        if status not in (PASS, SKIP):
            os.makedirs(args.evidence, exist_ok=True)
            shutil.copytree(artifacts, kept)
    shutil.rmtree(artifacts, ignore_errors=True)
    return dict(status=status, seconds=round(seconds, 2), detail=detail, artifacts=evidence)


def main():
    parser = argparse.ArgumentParser(description="one scenario run -> JSON verdict")
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--name", default="")
    parser.add_argument("--binary", required=True, help="the tools binary under test (im_test)")
    parser.add_argument("--helpers", required=True, help="the directory of the scenarios' helpers: devices, jxl_dump, device_uuid")
    parser.add_argument("--out", required=True)
    parser.add_argument("--evidence", default="", help="where a failed scenario's captures and logs are kept, under its name")
    parser.add_argument("--runtime", default="", help="where the scenario's runtime dir goes (short: it holds Wayland sockets)")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--bucket", default="", help="K/N: the scenario runs the K-th of its N buckets of checks")
    args = parser.parse_args()
    name = os.path.basename(args.scenario)[:-len(".py")]
    if args.bucket:
        name += "_" + args.bucket.split("/")[0]
    name = args.name or name
    try:
        record = run(name, args)
    except Exception as e:  # never let a runner bug abort the graph
        record = dict(status=FAIL, seconds=0.0, detail=f"runner error: {e}", artifacts={})
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(dict(name=name, **record), f)
    # always succeed: the verdict lives in the JSON, not the exit code
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
