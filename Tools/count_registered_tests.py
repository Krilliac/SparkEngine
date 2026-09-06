#!/usr/bin/env python3
"""Count the TEST cases that are actually compiled into the SparkTests binary.

The CI test floors (`--min-tests` / `--minimum-tests`) are hand-maintained
literals. A literal cannot notice that a whole test family stopped being
registered: the suite shrinks, the run stays green, and the floor is only
adjusted downwards the next time someone trips over it. This tool derives the
number from the source list CMake actually compiles, so the floor can be
computed instead of remembered.

Two numbers are emitted because only one of them is a sound floor:

  testMacrosUnconditional  TEST(...) macros at preprocessor depth 0. These are
                           compiled in every configuration, so the executed
                           count can never legitimately fall below this.
  testMacrosTotal          every TEST(...) macro in the registered sources.
                           An upper bound: the difference is gated behind
                           #if/#ifdef and may or may not be compiled.

Usage:
  python Tools/count_registered_tests.py --source-list <file> --output <json>

<file> is a newline- or semicolon-separated list of source paths (the SOURCES
property of the test target). Relative entries resolve against --source-dir.

Fail-closed: an unreadable source, an empty source list, or a list whose
entries do not exist is an error, never a zero-count success.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

TEST_MACRO = re.compile(r"^\s*TEST(?:_F)?\s*\(")
IF_DIRECTIVE = re.compile(r"^\s*#\s*if")
ENDIF_DIRECTIVE = re.compile(r"^\s*#\s*endif")


def count_file(path: Path) -> tuple[int, int]:
    """Return (total TEST macros, unconditional TEST macros) for one source."""
    depth = 0
    total = 0
    unconditional = 0
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if IF_DIRECTIVE.match(line):
                depth += 1
            elif ENDIF_DIRECTIVE.match(line) and depth > 0:
                depth -= 1
            if TEST_MACRO.match(line):
                total += 1
                if depth == 0:
                    unconditional += 1
    return total, unconditional


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-list", required=True, help="file holding the target's source list")
    parser.add_argument("--source-dir", required=True, help="directory relative entries resolve against")
    parser.add_argument("--output", required=True, help="JSON file to write")
    parser.add_argument("--target", default="SparkTests", help="name recorded in the report")
    args = parser.parse_args()

    listing = Path(args.source_list)
    if not listing.is_file():
        print(f"error: source list does not exist: {listing}", file=sys.stderr)
        return 1

    raw = listing.read_text(encoding="utf-8", errors="replace")
    entries = [entry.strip() for entry in raw.replace(";", "\n").splitlines()]
    entries = [entry for entry in entries if entry]
    if not entries:
        print(f"error: source list is empty: {listing}", file=sys.stderr)
        return 1

    source_dir = Path(args.source_dir)
    total = 0
    unconditional = 0
    counted_files = 0
    missing: list[str] = []
    for entry in entries:
        path = Path(entry)
        if not path.is_absolute():
            path = source_dir / path
        if path.suffix.lower() not in (".cpp", ".mm"):
            continue
        if not path.is_file():
            missing.append(str(path))
            continue
        file_total, file_unconditional = count_file(path)
        if file_total:
            counted_files += 1
        total += file_total
        unconditional += file_unconditional

    if missing:
        print("error: registered sources do not exist:\n  " + "\n  ".join(sorted(missing)), file=sys.stderr)
        return 1
    if total == 0:
        print("error: no TEST macros found in the registered sources", file=sys.stderr)
        return 1

    report = {
        "target": args.target,
        "sourceFilesWithTests": counted_files,
        "testMacrosTotal": total,
        "testMacrosUnconditional": unconditional,
        "note": (
            "testMacrosUnconditional is a sound floor for the executed count; "
            "testMacrosTotal additionally counts macros behind #if/#ifdef and is an upper bound."
        ),
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(
        f"count_registered_tests: {counted_files} files, {total} TEST macros "
        f"({unconditional} unconditional) -> {output}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
