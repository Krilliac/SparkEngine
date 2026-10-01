#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the SparkGameMMO character row (MMO::DecodeCharacterRecord).

Each seed is one stored "character_<id>" value as the AsyncDatabase store holds it:
the current 15-field row BuildCharacterSave writes, the 14-field legacy row, or the
row InsertCharacter writes with the name as a quoted SQL literal.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "mmo-character-record"


def row(*fields: str) -> bytes:
    return "|".join(fields).encode("ascii")


STATS = ("1.5", "0", "2.25", "90", "100", "100", "50", "50", "12.5")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    seeds = {
        "valid-current.row": row("Alice", "7", "3", "120", "2", *STATS, "40"),
        "valid-apostrophe-name.row": row("'O''Brien'", "8", "1", "0", "1", *STATS, "0"),
        "valid-legacy.row": row("Legacy", "3", "120", "2", *STATS, "40"),
        "valid-insert-quoted-name.row": row(
            "'Bob'", "8", "1", "0", "1", "0.0", "1.0", "0.0", "0.0", "100.0", "100.0", "50.0", "50.0", "0.0", "0"
        ),
        "short-row.row": row("Alice", "7", "3"),
        # Accepted before: std::stoul wraps "-1" to 4294967295 (another account's id space).
        "regression-negative-account.row": row("Alice", "-1", "3", "120", "2", *STATS, "40"),
        # Accepted before: std::stoi stops at the first non-digit.
        "regression-trailing-text.row": row("Alice", "7", "12abc", "120", "2", *STATS, "40"),
        # Accepted before: std::stof reads "nan", and a NaN position or health reaches gameplay.
        "regression-nonfinite-stat.row": row("Alice", "7", "3", "120", "2", "nan", *STATS[1:], "40"),
        # Accepted before: fields past the 15th were ignored instead of failing the row.
        "regression-extra-fields.row": row("Alice", "7", "3", "120", "2", *STATS, "40", "extra"),
        # Loaded exactly, then written back with 6 significant digits: every save moved the character.
        "regression-lossy-float.row": row(
            "Surveyor", "31", "4", "0", "1", "12345.678", "1", "-0.1", "0", "99.99999", "100", "50", "50", "86400.125",
            "0",
        ),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
