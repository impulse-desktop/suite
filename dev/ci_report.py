#!/usr/bin/env python3
# Copyright (C) 2026 imway team
# MIT licensed
# See LICENSE for the full license.

"""Turn a failed CI log into concise GitHub annotations and a job summary."""

import argparse
from dataclasses import dataclass
import html
import os
from pathlib import Path
import re


ANSI_ESCAPE = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")
NIX_LOG_PREFIX = re.compile(r"^[A-Za-z0-9_.+-]+> ")
COMPILER_ERROR = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<column>\d+): "
    r"(?P<message>(?:fatal )?error: .+)$"
)
UBSAN_ERROR = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<column>\d+): "
    r"(?P<message>runtime error: .+)$"
)


@dataclass(frozen=True)
class Finding:
    text: str
    file: str | None = None
    line: int | None = None
    column: int | None = None


def clean_line(raw_line):
    line = ANSI_ESCAPE.sub("", raw_line).rstrip()
    return NIX_LOG_PREFIX.sub("", line)


def source_path(path):
    marker = "/source/"
    if marker in path:
        return path.split(marker, 1)[1]
    workspace = os.environ.get("GITHUB_WORKSPACE")
    if workspace:
        try:
            return str(Path(path).resolve().relative_to(Path(workspace).resolve()))
        except (OSError, ValueError):
            pass
    return path


def collect_findings(log, limit=80):
    findings = []
    seen = set()

    def add(finding):
        key = (finding.text, finding.file, finding.line, finding.column)
        if key not in seen and len(findings) < limit:
            seen.add(key)
            findings.append(finding)

    lines = [clean_line(line) for line in log.splitlines()]
    for line in lines:
        match = COMPILER_ERROR.match(line) or UBSAN_ERROR.match(line)
        if match:
            add(
                Finding(
                    match.group("message"),
                    source_path(match.group("file")),
                    int(match.group("line")),
                    int(match.group("column")),
                )
            )
            continue

        if "XFAIL" in line:
            continue

        if (
            line.startswith(("FAIL ", "FAIL:", "FAILED ", "XPASS "))
            or "ERROR: AddressSanitizer:" in line
            or "SUMMARY: AddressSanitizer:" in line
            or "runtime error:" in line
        ):
            add(Finding(line))

    if findings:
        return findings

    # Setup and Nix evaluation failures do not have test-style markers.
    for line in lines:
        if line.startswith("error:") or "error: builder for " in line:
            add(Finding(line))
    return findings


def workflow_escape(value, property_value=False):
    replacements = {
        "%": "%25",
        "\r": "%0D",
        "\n": "%0A",
    }
    if property_value:
        replacements.update({":": "%3A", ",": "%2C"})
    for original, replacement in replacements.items():
        value = value.replace(original, replacement)
    return value


def annotation(finding, title):
    message = workflow_escape(finding.text)
    properties = [f"title={workflow_escape(title, property_value=True)}"]
    if finding.file is not None:
        properties.append(
            f"file={workflow_escape(finding.file, property_value=True)}"
        )
    if finding.line is not None:
        properties.append(f"line={finding.line}")
    if finding.column is not None:
        properties.append(f"col={finding.column}")
    return f"::error {','.join(properties)}::{message}"


def render_summary(title, reproduce, findings, log_exists):
    lines = [f"## {html.escape(title)} failed", ""]
    if reproduce:
        lines.extend(
            [
                "Reproduce locally:",
                "",
                f"<pre>{html.escape(reproduce)}</pre>",
                "",
            ]
        )
    if findings:
        lines.extend(["Detected failures:", "", "<pre>"])
        lines.extend(html.escape(finding.text) for finding in findings)
        lines.extend(["</pre>", ""])
    elif log_exists:
        lines.extend(
            [
                "No standard failure marker was found. Open the failed step "
                "or download its complete log.",
                "",
            ]
        )
    else:
        lines.extend(
            [
                "The check failed before it produced a log. Inspect the setup "
                "steps above it.",
                "",
            ]
        )
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True)
    parser.add_argument("--title", required=True)
    parser.add_argument("--reproduce", default="")
    arguments = parser.parse_args()

    log_path = Path(arguments.log)
    log_exists = log_path.is_file()
    log = log_path.read_text(errors="replace") if log_exists else ""
    findings = collect_findings(log)

    for finding in findings[:10]:
        print(annotation(finding, arguments.title))

    summary = render_summary(
        arguments.title,
        arguments.reproduce,
        findings,
        log_exists,
    )
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with Path(summary_path).open("a") as output:
            output.write(summary)
    else:
        print(summary)


if __name__ == "__main__":
    main()
