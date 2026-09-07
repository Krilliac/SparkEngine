#!/usr/bin/env python3
"""SEC-120 — Fail-closed CI/static validation for fuzz policy.

Enforces that every stable-v1 parser in the authoritative inventory is:
  1. Classified (present in KNOWN_PARSERS), AND
  2. Either wired to a fuzz target, OR carries an explicit blocker with ticket.

Also validates:
  - No duplicate parser IDs
  - No parser references source files that don't exist on disk
  - No path traversal in source file references
  - Corpus entries (when present) have valid resource budgets
  - Corpus entries are not stale (> 90 days since last verification)

Exit codes:
  0 — all checks pass
  1 — policy violations found (CI must block)
  2 — internal error

Usage:
    python tools/fuzz-policy/check_fuzz_policy.py [--source-root .] [--ci] [--emit-json]

In --ci mode, unclassified parsers are treated as hard failures (exit 1).
Without --ci, they are warnings (exit 0).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from datetime import date
from pathlib import Path

# Ensure tools/fuzz-policy is importable
_script_dir = Path(__file__).resolve().parent
if str(_script_dir) not in sys.path:
    sys.path.insert(0, str(_script_dir))

from parser_inventory import (
    KNOWN_PARSERS,
    FuzzStatus,
    TrustBoundary,
    check_duplicate_ids,
    find_unclassified_parsers,
)
from corpus_manifest import CORPUS_MANIFEST, validate_manifest


def _normalize_path(p: str) -> str:
    return p.replace("\\", "/")


def _is_path_traversal(p: str) -> bool:
    normalized = _normalize_path(p)
    parts = normalized.split("/")
    return ".." in parts or any(part.startswith("/") for part in [normalized])


def check_source_files_exist(source_root: Path) -> list[str]:
    """Verify every source file reference in the inventory actually exists."""
    errors: list[str] = []
    for p in KNOWN_PARSERS:
        for sf in p.source_files:
            if _is_path_traversal(sf):
                errors.append(f"{p.parser_id}: path traversal in source_file: {sf}")
                continue
            full = source_root / sf
            if not full.exists():
                errors.append(f"{p.parser_id}: source file not found: {sf}")
    return errors


def check_classification_completeness() -> list[str]:
    """Every parser must have a valid fuzz_status with required fields."""
    errors: list[str] = []
    for p in KNOWN_PARSERS:
        if p.fuzz_status == FuzzStatus.UNCLASSIFIED:
            errors.append(f"{p.parser_id}: fuzz_status is UNCLASSIFIED — must be fuzzed or blocked")
        errs = p.validate()
        errors.extend(errs)
    return errors


def check_blocked_parsers_have_tickets() -> list[str]:
    """Every blocked parser must reference a tracking ticket."""
    errors: list[str] = []
    for p in KNOWN_PARSERS:
        if p.fuzz_status == FuzzStatus.BLOCKED:
            if not p.blocker_ticket:
                errors.append(f"{p.parser_id}: blocked but no blocker_ticket")
            if not p.blocker_reason:
                errors.append(f"{p.parser_id}: blocked but no blocker_reason")
    return errors


def check_fuzzed_parsers_have_targets() -> list[str]:
    """Every fuzzed parser must point to a real harness."""
    errors: list[str] = []
    for p in KNOWN_PARSERS:
        if p.fuzz_status == FuzzStatus.FUZZED:
            if not p.fuzz_target:
                errors.append(f"{p.parser_id}: fuzzed but no fuzz_target path")
    return errors


def check_untrusted_parsers_not_ignored() -> list[str]:
    """Untrusted-boundary parsers must not be in trusted-internal without justification."""
    warnings: list[str] = []
    for p in KNOWN_PARSERS:
        if p.trust_boundary == TrustBoundary.TRUSTED_INTERNAL:
            if any(fmt in p.formats_handled for fmt in [".json", ".xml", ".csv"]):
                warnings.append(
                    f"{p.parser_id}: trusted-internal parser handles common external formats "
                    f"({p.formats_handled}) — verify trust boundary classification"
                )
    return warnings


def run_all_checks(source_root: Path, ci_mode: bool) -> dict:
    """Run all policy checks; return structured result."""
    hard_errors: list[str] = []
    warnings: list[str] = []

    # Duplicate IDs
    dupes = check_duplicate_ids()
    if dupes:
        hard_errors.append(f"Duplicate parser IDs: {', '.join(dupes)}")

    # Classification completeness
    hard_errors.extend(check_classification_completeness())

    # Blocked parsers
    hard_errors.extend(check_blocked_parsers_have_tickets())

    # Fuzzed parsers
    hard_errors.extend(check_fuzzed_parsers_have_targets())

    # Source files exist
    hard_errors.extend(check_source_files_exist(source_root))

    # Trust boundary sanity
    warnings.extend(check_untrusted_parsers_not_ignored())

    # Unclassified parsers in source tree
    unclassified = find_unclassified_parsers(source_root)
    if unclassified:
        msg_lines = [f"  {u['file']}" for u in unclassified[:30]]
        label = "UNCLASSIFIED source files with parser patterns"
        if ci_mode:
            hard_errors.append(f"{label} ({len(unclassified)}):\n" + "\n".join(msg_lines))
        else:
            warnings.append(f"{label} ({len(unclassified)}):\n" + "\n".join(msg_lines))

    # Corpus manifest validation
    corpus_errors = validate_manifest(CORPUS_MANIFEST)
    hard_errors.extend(corpus_errors)

    # Summary stats
    total = len(KNOWN_PARSERS)
    fuzzed = sum(1 for p in KNOWN_PARSERS if p.fuzz_status == FuzzStatus.FUZZED)
    blocked = sum(1 for p in KNOWN_PARSERS if p.fuzz_status == FuzzStatus.BLOCKED)
    untrusted_file = sum(1 for p in KNOWN_PARSERS if p.trust_boundary == TrustBoundary.UNTRUSTED_FILE)
    untrusted_net = sum(1 for p in KNOWN_PARSERS if p.trust_boundary == TrustBoundary.UNTRUSTED_NETWORK)

    return {
        "passed": len(hard_errors) == 0,
        "total_parsers": total,
        "fuzzed": fuzzed,
        "blocked": blocked,
        "untrusted_file_parsers": untrusted_file,
        "untrusted_network_parsers": untrusted_net,
        "unclassified_source_count": len(unclassified),
        "hard_errors": hard_errors,
        "warnings": warnings,
        "check_date": date.today().isoformat(),
        "ci_mode": ci_mode,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", default=".", help="Repository root")
    parser.add_argument("--ci", action="store_true", help="CI mode: unclassified = hard failure")
    parser.add_argument("--emit-json", action="store_true", help="JSON output")
    args = parser.parse_args()

    source_root = Path(args.source_root).resolve()

    try:
        result = run_all_checks(source_root, args.ci)
    except Exception as e:
        print(f"Internal error: {e}", file=sys.stderr)
        return 2

    if args.emit_json:
        print(json.dumps(result, indent=2))
    else:
        status = "PASS" if result["passed"] else "FAIL"
        print(f"Fuzz policy check: {status}")
        print(f"  Parsers: {result['total_parsers']} total, {result['fuzzed']} fuzzed, {result['blocked']} blocked")
        print(f"  Untrusted surfaces: {result['untrusted_file_parsers']} file, {result['untrusted_network_parsers']} network")

        if result["hard_errors"]:
            print(f"\n  ERRORS ({len(result['hard_errors'])}):")
            for e in result["hard_errors"]:
                print(f"    {e}")

        if result["warnings"]:
            print(f"\n  WARNINGS ({len(result['warnings'])}):")
            for w in result["warnings"]:
                print(f"    {w}")

    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
