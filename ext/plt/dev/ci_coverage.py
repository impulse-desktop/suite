#!/usr/bin/env python3
"""Export coverage from all plt test executables, including forked clients."""

import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("profile_dir", type=Path)
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()
    root = Path.cwd().resolve()
    profiles = sorted(args.profile_dir.glob("*.profraw"))
    if not profiles:
        parser.error("no coverage profiles produced by tests")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    profile = args.output_dir / "coverage.profdata"
    subprocess.run([
        "llvm-profdata", "merge", "-sparse", *map(str, profiles), "-o", str(profile),
    ], check=True)
    binaries = [args.build_dir / name for name in (
        "plt_wayland_integration_tests", "plt_cocoa_tests",
    ) if (args.build_dir / name).exists()]
    binaries.extend(sorted((args.build_dir / "e2e").glob("*")))
    if not binaries:
        parser.error("no test executables found for coverage export")
    # Include every compiled production object, even if no executable links
    # it yet. Unused backends and helpers must count as uncovered code.
    binaries.append(args.build_dir / "libplt.a")
    objects = [str(binaries[0])]
    for binary in binaries[1:]:
        objects.extend(["-object", str(binary)])
    sources = sorted(
        str(path) for path in root.iterdir()
        if path.suffix in {".cpp", ".h", ".mm"} and not path.stem.endswith("_ut")
    )
    options = [*objects, f"-instr-profile={profile}", *sources]
    trace = subprocess.check_output([
        "llvm-cov", "export", "-format=lcov", *options,
    ], text=True)
    records = []
    for record in trace.split("end_of_record"):
        lines = record.strip().splitlines()
        for index, line in enumerate(lines):
            if line.startswith("SF:"):
                path = Path(line[3:]).resolve()
                # Only project code; exclude dependencies, tests and generated files.
                if path.parent == root and str(path) in sources:
                    lines[index] = f"SF:{path.name}"
                    records.append("\n".join(lines) + "\nend_of_record\n")
                break
    if not records:
        parser.error("coverage export contains no project sources")
    (args.output_dir / "coverage.info").write_text("".join(records))
    summary = subprocess.check_output(["llvm-cov", "report", *options], text=True)
    (args.output_dir / "summary.txt").write_text(summary)
    print(summary)


if __name__ == "__main__":
    main()
