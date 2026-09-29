#!/usr/bin/env python3
# Copyright (C) 2026 imway team
# MIT licensed
# See LICENSE for the full license.

"""Merge lcov tracefiles from coverage shards into one report.

The coverage CI shards each export an lcov file for the tests their
partition ran; execution counts for a line simply add up across shards,
so the merge is a per-file sum of DA/FNDA/BRDA records with the LF/LH,
FNF/FNH and BRF/BRH summaries recomputed. Empty input tracefiles are
tolerated.
"""

import argparse
from collections import defaultdict
from pathlib import Path
import sys


class FileRecord:
    def __init__(self):
        self.functions = {}  # name -> line
        self.function_hits = defaultdict(int)  # name -> count
        self.lines = defaultdict(int)  # line -> count
        self.branches = {}  # (line, block, branch) -> count or None ('-')


def merge_tracefile(path, files):
    record = None
    for raw in Path(path).read_text().splitlines():
        if raw == "end_of_record":
            record = None
            continue
        kind, _, rest = raw.partition(":")
        if kind == "SF":
            record = files.setdefault(rest, FileRecord())
        elif record is None:
            continue
        elif kind == "FN":
            line, _, name = rest.partition(",")
            record.functions.setdefault(name, int(line))
        elif kind == "FNDA":
            count, _, name = rest.partition(",")
            record.function_hits[name] += int(count)
        elif kind == "DA":
            line, count = rest.split(",")[:2]
            record.lines[int(line)] += int(count)
        elif kind == "BRDA":
            line, block, branch, taken = rest.split(",")
            key = (int(line), int(block), int(branch))
            count = None if taken == "-" else int(taken)
            previous = record.branches.get(key)
            if previous is None:
                record.branches[key] = count
            elif count is not None:
                record.branches[key] = previous + count
        # FNF/FNH/LF/LH/BRF/BRH are recomputed on output.


def write_merged(files, output):
    totals = defaultdict(int)
    with open(output, "w") as out:
        for path in sorted(files):
            record = files[path]
            out.write(f"SF:{path}\n")
            for name, line in sorted(record.functions.items(), key=lambda kv: (kv[1], kv[0])):
                out.write(f"FN:{line},{name}\n")
            for name, _ in sorted(record.functions.items(), key=lambda kv: (kv[1], kv[0])):
                out.write(f"FNDA:{record.function_hits.get(name, 0)},{name}\n")
            function_hits = sum(1 for name in record.functions if record.function_hits.get(name, 0) > 0)
            out.write(f"FNF:{len(record.functions)}\n")
            out.write(f"FNH:{function_hits}\n")
            for key in sorted(record.branches):
                taken = record.branches[key]
                out.write(f"BRDA:{key[0]},{key[1]},{key[2]},{'-' if taken is None else taken}\n")
            branch_hits = sum(1 for taken in record.branches.values() if taken)
            out.write(f"BRF:{len(record.branches)}\n")
            out.write(f"BRH:{branch_hits}\n")
            for line in sorted(record.lines):
                out.write(f"DA:{line},{record.lines[line]}\n")
            line_hits = sum(1 for count in record.lines.values() if count > 0)
            out.write(f"LF:{len(record.lines)}\n")
            out.write(f"LH:{line_hits}\n")
            out.write("end_of_record\n")
            totals["functions"] += len(record.functions)
            totals["function_hits"] += function_hits
            totals["branches"] += len(record.branches)
            totals["branch_hits"] += branch_hits
            totals["lines"] += len(record.lines)
            totals["line_hits"] += line_hits
    return totals


def write_summary(files, totals, summary):
    def percent(hits, total):
        return f"{100.0 * hits / total:6.2f}%" if total else "     -"

    with open(summary, "w") as out:
        width = max((len(path) for path in files), default=4)
        out.write(f"{'File'.ljust(width)}  {'Lines':>13} {'Cover':>7}  {'Branches':>13} {'Cover':>7}\n")
        for path in sorted(files):
            record = files[path]
            line_hits = sum(1 for count in record.lines.values() if count > 0)
            branch_hits = sum(1 for taken in record.branches.values() if taken)
            out.write(
                f"{path.ljust(width)}  "
                f"{line_hits:6d}/{len(record.lines):<6d} {percent(line_hits, len(record.lines))}  "
                f"{branch_hits:6d}/{len(record.branches):<6d} {percent(branch_hits, len(record.branches))}\n"
            )
        out.write(
            f"{'TOTAL'.ljust(width)}  "
            f"{totals['line_hits']:6d}/{totals['lines']:<6d} {percent(totals['line_hits'], totals['lines'])}  "
            f"{totals['branch_hits']:6d}/{totals['branches']:<6d} {percent(totals['branch_hits'], totals['branches'])}\n"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, help="merged lcov tracefile to write")
    parser.add_argument("--summary", help="optional per-file text summary to write")
    parser.add_argument("inputs", nargs="+", help="shard lcov tracefiles")
    arguments = parser.parse_args()

    files = {}
    for path in arguments.inputs:
        merge_tracefile(path, files)
    if not files:
        print("merge_lcov: every input tracefile is empty", file=sys.stderr)
        return 1
    totals = write_merged(files, arguments.output)
    if arguments.summary:
        write_summary(files, totals, arguments.summary)
    print(
        f"merge_lcov: {len(files)} files, "
        f"lines {totals['line_hits']}/{totals['lines']}, "
        f"branches {totals['branch_hits']}/{totals['branches']}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
