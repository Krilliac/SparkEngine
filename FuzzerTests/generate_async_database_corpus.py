#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the AsyncDatabase store file (SQLiteConnection::Open).

Each seed is a whole store file as FlushToDisk writes it (escaped format) or as
builds before escaping wrote it (legacy format). The harness opens it under a
64 KiB store budget.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "async-database-kv"

MARKER = b"#!spark-kv-v2\n"
BUDGET = 64 * 1024


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    records = (
        b"character_1\tAlice|1|7|120|3|1.5|0|2.25|0|100|100|50|50|12.5|40\n"
        b"guild_3\t1|Night Watch|NW|1|250|50|line one\\nline two\\ttabbed\n"
        b"path\tC:\\\\temp\\\\saves\n"
    )
    # 40 KiB of raw backslashes: the file fits the budget, its escaped rewrite does not.
    legacy_growth = b"blob\t" + b"\\" * (40 * 1024) + b"\n"
    # A raw tab inside an escaped-format value is kept, and doubles when rewritten.
    raw_tab_growth = MARKER + b"blob\t" + b"\t" * (40 * 1024) + b"\n"
    assert len(legacy_growth) <= BUDGET and len(raw_tab_growth) <= BUDGET
    seeds = {
        "valid-escaped.db": MARKER + records,
        "valid-legacy.db": b"path\tC:\\temp\\saves\nname\tBob\n",
        "truncated-record.db": MARKER + b"character_1\tAlice|1",
        "unknown-escape.db": MARKER + b"key\tvalue\\x41\n",
        "repeated-key.db": MARKER + b"a\t1\na\t2\n",
        # One byte over the budget: the largest input the harness accepts.
        "over-budget.db": MARKER + b"big\t" + b"x" * (BUDGET + 1 - len(MARKER) - 5) + b"\n",
        "regression-legacy-rewrite-over-budget.db": legacy_growth,
        "regression-raw-tab-rewrite-over-budget.db": raw_tab_growth,
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
