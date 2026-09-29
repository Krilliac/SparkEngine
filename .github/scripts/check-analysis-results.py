#!/usr/bin/env python3
"""Fail the required analysis lane on new CodeQL findings or incomplete SARIF.

The gate blocks regressions relative to a reviewed baseline
(``.github/codeql-baseline.json``). Each baseline entry names one finding by
language, rule, path and CodeQL's ``primaryLocationLineHash`` fingerprint, plus
a written review. A result that no entry matches blocks (exit 1). A baseline
entry that no result matches is stale and also blocks, so fixed findings leave
the baseline in the same change and it can only shrink without review.

SARIF ``suppressions``, ``baselineState`` and result levels are ignored: only
the committed, reviewed baseline can accept a finding. Scanner completion is
required (``executionSuccessful``), and any error-level tool notification fails
closed; CodeQL always emits informational notifications (extracted files,
extractor summaries), so ``none``/``note``/``warning`` notifications are allowed.

Exit status: 0 no new or stale findings, 1 new or stale findings, 2 the report
or baseline is malformed or the scan did not complete.
"""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import sys

MAX_FILE_BYTES = 64 * 1024 * 1024
LANGUAGES = ("actions", "c-cpp", "python")
DEFAULT_BASELINE = Path(__file__).resolve().parents[1] / "codeql-baseline.json"


def reject_duplicates(pairs: list[tuple]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate key: {key}")
        result[key] = value
    return result


def _nonempty_string(value: object) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _check_notifications(invocation: dict) -> None:
    for field in ("toolExecutionNotifications", "toolConfigurationNotifications"):
        notifications = invocation.get(field, [])
        if not isinstance(notifications, list):
            raise ValueError(f"{field} is not a list")
        for notification in notifications:
            if not isinstance(notification, dict):
                raise ValueError(f"malformed entry in {field}")
            if notification.get("level") == "error":
                descriptor = notification.get("descriptor", {})
                ident = descriptor.get("id") if isinstance(descriptor, dict) else None
                raise ValueError(f"scanner reported an error notification ({ident or 'unknown'}) in {field}")


def finding_key(result: object) -> tuple[str, str, str]:
    """Return (ruleId, path, fingerprint) for one SARIF result, failing closed."""
    if not isinstance(result, dict) or not _nonempty_string(result.get("ruleId")):
        raise ValueError("malformed analysis result: missing ruleId")
    locations = result.get("locations")
    if not isinstance(locations, list) or not locations or not isinstance(locations[0], dict):
        raise ValueError(f"result {result['ruleId']} has no location")
    physical = locations[0].get("physicalLocation", {})
    artifact = physical.get("artifactLocation", {}) if isinstance(physical, dict) else {}
    uri = artifact.get("uri") if isinstance(artifact, dict) else None
    fingerprints = result.get("partialFingerprints")
    fingerprint = fingerprints.get("primaryLocationLineHash") if isinstance(fingerprints, dict) else None
    if not _nonempty_string(uri) or not _nonempty_string(fingerprint):
        raise ValueError(f"result {result['ruleId']} lacks a path or primaryLocationLineHash fingerprint")
    return result["ruleId"], uri, fingerprint


def report_findings(report: object) -> list[tuple[str, str, str]]:
    """Validate scanner completion and return every finding's baseline key."""
    if not isinstance(report, dict) or report.get("version") != "2.1.0":
        raise ValueError("expected SARIF 2.1.0")
    runs = report.get("runs")
    if not isinstance(runs, list) or not runs:
        raise ValueError("analysis has no runs")
    findings: list[tuple[str, str, str]] = []
    for run in runs:
        if not isinstance(run, dict) or run.get("tool", {}).get("driver", {}).get("name") != "CodeQL":
            raise ValueError("expected a CodeQL run")
        invocations = run.get("invocations")
        if not isinstance(invocations, list) or not invocations:
            raise ValueError("missing scanner completion evidence")
        for invocation in invocations:
            if not isinstance(invocation, dict) or invocation.get("executionSuccessful") is not True:
                raise ValueError("scanner did not complete successfully")
            _check_notifications(invocation)
        results = run.get("results")
        if not isinstance(results, list):
            raise ValueError("missing results array")
        findings.extend(finding_key(result) for result in results)
    return findings


def load_baseline(document: object, language: str) -> list[tuple[str, str, str]]:
    """Return the reviewed baseline keys for one language, validating every entry."""
    if not isinstance(document, dict) or document.get("schema") != 1:
        raise ValueError("baseline must be an object with schema 1")
    entries = document.get("findings")
    if not isinstance(entries, list):
        raise ValueError("baseline findings must be a list")
    keys: list[tuple[str, str, str]] = []
    seen: set[tuple[str, str, str, str]] = set()
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {"language", "ruleId", "path", "fingerprint", "review"}:
            raise ValueError(f"baseline entry must have exactly language/ruleId/path/fingerprint/review: {entry}")
        if entry["language"] not in LANGUAGES:
            raise ValueError(f"baseline entry has unknown language {entry['language']!r}")
        if not all(_nonempty_string(entry[field]) for field in ("ruleId", "path", "fingerprint", "review")):
            raise ValueError(f"baseline entry has an empty field: {entry}")
        identity = (entry["language"], entry["ruleId"], entry["path"], entry["fingerprint"])
        if identity in seen:
            raise ValueError(f"duplicate baseline entry: {identity}")
        seen.add(identity)
        if entry["language"] == language:
            keys.append(identity[1:])
    return keys


def compare(findings: list[tuple[str, str, str]], baseline: list[tuple[str, str, str]]):
    """Return (new, stale) finding keys as sorted lists."""
    found = Counter(findings)
    accepted = Counter(baseline)
    new = sorted((found - accepted).elements())
    stale = sorted((accepted - found).elements())
    return new, stale


def _read_json(path: Path) -> object:
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_FILE_BYTES:
        raise ValueError(f"{path} must be one bounded regular file")
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=reject_duplicates)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("report", type=Path)
    parser.add_argument("--language", required=True, choices=LANGUAGES)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    args = parser.parse_args(argv)
    try:
        findings = report_findings(_read_json(args.report))
        baseline = load_baseline(_read_json(args.baseline), args.language)
    except (OSError, ValueError, TypeError, AttributeError) as error:
        print(f"analysis: error: {error}", file=sys.stderr)
        return 2
    new, stale = compare(findings, baseline)
    for rule, path, fingerprint in new:
        print(f"analysis: NEW finding {rule} at {path} ({fingerprint}); fix it or add a reviewed baseline entry")
    for rule, path, fingerprint in stale:
        print(f"analysis: STALE baseline entry {rule} at {path} ({fingerprint}); remove it from the baseline")
    print(f"analysis: {args.language}: {len(findings)} finding(s), {len(baseline)} baselined, "
          f"{len(new)} new, {len(stale)} stale")
    return 1 if new or stale else 0


if __name__ == "__main__":
    raise SystemExit(main())
