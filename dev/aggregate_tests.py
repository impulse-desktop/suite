#!/usr/bin/env python3
# The final test node: read every scenario's JSON verdict, print the summary
# with the evidence of each failure, write it to --out, and exit nonzero when
# any scenario failed. This is the node that makes `./build test` fail: it
# depends on every scenario node and is given their verdicts by path.

import argparse
import json
import os


def indent(text):
    return "".join("      " + line + "\n" for line in text.splitlines())


def main():
    parser = argparse.ArgumentParser(description="aggregate the scenarios' JSON verdicts")
    parser.add_argument("--out", required=True, help="verdict summary file")
    parser.add_argument("verdicts", nargs="+", help="the scenarios' JSON records")
    args = parser.parse_args()
    records = []
    for path in args.verdicts:
        with open(path) as f:
            records.append(json.load(f))
    records.sort(key=lambda record: record["name"])
    lines = []
    details = []
    failed = 0
    skipped = 0
    for record in records:
        ok = record["status"] == "PASS"
        skip = record["status"] == "SKIP"
        failed += not ok and not skip
        skipped += skip
        # a skip says what the compositor lacked, or a scenario that quietly
        # stops running is invisible
        note = f"  [{record['detail']}]" if skip else ""
        lines.append(f"  {'OK' if ok else record['status']:<8}{record['name']} ({record['seconds']:.1f}s){note}")
        if not ok and not skip:
            details.append(f"--- {record['name']}: {record['detail']}")
            details.append(f"    reproduce: ./build test -Dfilter='{record['name']}'")
            for name, text in record["artifacts"].items():
                details.append(f"    {name}:")
                details.append(indent(text))
    body = "\n".join(lines)
    if details:
        body += "\n\n" + "\n".join(details)
    body += f"\n\n{len(records) - failed - skipped} ok, {skipped} skip, {failed} fail\n"
    print(body, end="")
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        f.write(body)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
