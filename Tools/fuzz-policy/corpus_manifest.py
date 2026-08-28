#!/usr/bin/env python3
"""SEC-120 — Corpus/seed metadata and bounded resource budgets.

Defines the schema for fuzz corpus manifests and enforces resource limits
that prevent unbounded resource consumption during fuzz campaigns.

Each fuzz target must declare:
  - A seed corpus directory (or explicit empty-corpus justification)
  - Maximum input size in bytes (hard cap for the fuzzer)
  - Maximum parse wall-time in milliseconds (timeout per iteration)
  - Maximum corpus entries (prevents unbounded disk growth)
  - Corpus freshness: last-verified date (stale if > 90 days)

Usage:
    python tools/fuzz-policy/corpus_manifest.py [--check] [--emit-json]
"""
from __future__ import annotations

import json
import sys
from dataclasses import asdict, dataclass
from datetime import date, timedelta
from pathlib import Path
from typing import Optional


STALENESS_THRESHOLD_DAYS = 90


@dataclass(frozen=True)
class ResourceBudget:
    """Hard resource limits for a single fuzz target iteration."""
    max_input_bytes: int
    max_parse_time_ms: int
    max_memory_mb: int
    max_corpus_entries: int
    max_corpus_total_mb: int

    def validate(self) -> list[str]:
        errors: list[str] = []
        if self.max_input_bytes <= 0:
            errors.append("max_input_bytes must be > 0")
        if self.max_input_bytes > 100 * 1024 * 1024:
            errors.append("max_input_bytes exceeds 100 MB hard ceiling")
        if self.max_parse_time_ms <= 0:
            errors.append("max_parse_time_ms must be > 0")
        if self.max_parse_time_ms > 60_000:
            errors.append("max_parse_time_ms exceeds 60s hard ceiling")
        if self.max_memory_mb <= 0:
            errors.append("max_memory_mb must be > 0")
        if self.max_memory_mb > 4096:
            errors.append("max_memory_mb exceeds 4 GB hard ceiling")
        if self.max_corpus_entries <= 0:
            errors.append("max_corpus_entries must be > 0")
        if self.max_corpus_entries > 100_000:
            errors.append("max_corpus_entries exceeds 100K hard ceiling")
        if self.max_corpus_total_mb <= 0:
            errors.append("max_corpus_total_mb must be > 0")
        if self.max_corpus_total_mb > 10_000:
            errors.append("max_corpus_total_mb exceeds 10 GB hard ceiling")
        return errors


@dataclass(frozen=True)
class CorpusEntry:
    """Metadata for a fuzz target's seed corpus."""
    parser_id: str
    corpus_dir: Optional[str]
    seed_count: int
    budget: ResourceBudget
    last_verified: str
    empty_corpus_justification: Optional[str] = None

    def validate(self) -> list[str]:
        errors: list[str] = []
        prefix = f"corpus[{self.parser_id}]"

        if not self.corpus_dir and not self.empty_corpus_justification:
            errors.append(f"{prefix}: no corpus_dir and no empty_corpus_justification")

        if self.corpus_dir and self.seed_count <= 0:
            errors.append(f"{prefix}: corpus_dir set but seed_count <= 0")

        errors.extend(f"{prefix}: budget: {e}" for e in self.budget.validate())

        try:
            verified = date.fromisoformat(self.last_verified)
            staleness = (date.today() - verified).days
            if staleness > STALENESS_THRESHOLD_DAYS:
                errors.append(
                    f"{prefix}: corpus stale — last verified {self.last_verified} "
                    f"({staleness} days ago, threshold={STALENESS_THRESHOLD_DAYS})"
                )
        except ValueError:
            errors.append(f"{prefix}: invalid last_verified date: {self.last_verified}")

        return errors


# ---------------------------------------------------------------------------
# Default resource budget tiers — parsers pick the tier that fits
# ---------------------------------------------------------------------------

TIER_SMALL = ResourceBudget(
    max_input_bytes=1 * 1024 * 1024,
    max_parse_time_ms=1000,
    max_memory_mb=256,
    max_corpus_entries=5000,
    max_corpus_total_mb=500,
)

TIER_MEDIUM = ResourceBudget(
    max_input_bytes=10 * 1024 * 1024,
    max_parse_time_ms=5000,
    max_memory_mb=1024,
    max_corpus_entries=10000,
    max_corpus_total_mb=2000,
)

TIER_LARGE = ResourceBudget(
    max_input_bytes=50 * 1024 * 1024,
    max_parse_time_ms=30000,
    max_memory_mb=2048,
    max_corpus_entries=50000,
    max_corpus_total_mb=5000,
)

TIER_NETWORK = ResourceBudget(
    max_input_bytes=64 * 1024,
    max_parse_time_ms=500,
    max_memory_mb=128,
    max_corpus_entries=10000,
    max_corpus_total_mb=100,
)


# ---------------------------------------------------------------------------
# Planned corpus manifest — populated as fuzz harnesses are implemented
# ---------------------------------------------------------------------------

CORPUS_MANIFEST: list[CorpusEntry] = [
    # Placeholder entries for highest-priority targets.
    # These will be populated with real seed corpora as harnesses land.
]


def validate_manifest(entries: list[CorpusEntry]) -> list[str]:
    """Validate all corpus entries; return list of errors."""
    errors: list[str] = []

    seen_ids: set[str] = set()
    for entry in entries:
        if entry.parser_id in seen_ids:
            errors.append(f"Duplicate parser_id in corpus manifest: {entry.parser_id}")
        seen_ids.add(entry.parser_id)
        errors.extend(entry.validate())

    return errors


def emit_manifest_json() -> dict:
    errors = validate_manifest(CORPUS_MANIFEST)
    return {
        "schema_version": "1.0.0",
        "staleness_threshold_days": STALENESS_THRESHOLD_DAYS,
        "budget_tiers": {
            "small": asdict(TIER_SMALL),
            "medium": asdict(TIER_MEDIUM),
            "large": asdict(TIER_LARGE),
            "network": asdict(TIER_NETWORK),
        },
        "entries": [asdict(e) for e in CORPUS_MANIFEST],
        "validation_errors": errors,
    }


def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--emit-json", action="store_true")
    args = parser.parse_args()

    if args.emit_json:
        print(json.dumps(emit_manifest_json(), indent=2))
        return 0

    errors = validate_manifest(CORPUS_MANIFEST)
    print(f"Corpus manifest: {len(CORPUS_MANIFEST)} entries")
    if errors:
        print(f"  ERRORS ({len(errors)}):")
        for e in errors:
            print(f"    {e}")
        return 1 if args.check else 0

    print("  All entries valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
