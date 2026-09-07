#!/usr/bin/env python3
"""Blocking SEC-120 policy gate with deterministic evidence verification."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

from corpus_manifest import DEFAULT_CORPUS_MANIFEST, build_corpus_report
from parser_inventory import DEFAULT_INVENTORY, build_inventory_report
from policy_common import PolicyError, load_json_document, read_confined_file


DEFAULT_EVIDENCE = "docs/sec120-fuzz-policy-check.json"


def _decode(root: Path, path: str, field: str, maximum: int = 2 * 1024 * 1024) -> str:
    payload = read_confined_file(root, path, field, max_bytes=maximum)
    try:
        return payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise PolicyError(f"{field} must be strict UTF-8") from exc


def _job_block(workflow: str, job_name: str) -> str:
    lines = workflow.splitlines()
    header = f"  {job_name}:"
    starts = [index for index, line in enumerate(lines) if line == header]
    if len(starts) != 1:
        raise PolicyError(f"build workflow must define exactly one {job_name} job")
    start = starts[0]
    end = len(lines)
    for index in range(start + 1, len(lines)):
        if re.fullmatch(r"  [A-Za-z0-9_-]+:", lines[index]):
            end = index
            break
    return "\n".join(lines[start:end])


def validate_ci_and_cmake_binding(root: Path) -> None:
    workflow = _decode(root, ".github/workflows/build.yml", "build workflow")
    fuzz_job = _job_block(workflow, "fuzz-policy")
    for literal in (
        "runs-on: ubuntu-24.04",
        "cmake -S tools/fuzz-policy -B build/fuzz-policy",
        "cmake --build build/fuzz-policy --target check-fuzz-policy",
        "ctest --test-dir build/fuzz-policy --output-on-failure --no-tests=error",
    ):
        if literal not in fuzz_job:
            raise PolicyError(f"fuzz-policy CI job is missing {literal!r}")
    required_gate = _job_block(workflow, "required-ci-gate")
    if not re.search(r"(?m)^\s+- fuzz-policy\s*$", required_gate):
        raise PolicyError("Required CI Gate does not depend on fuzz-policy")

    root_cmake = _decode(root, "CMakeLists.txt", "root CMakeLists")
    required_include = 'include("${CMAKE_SOURCE_DIR}/cmake/SparkFuzzPolicy.cmake")'
    if required_include not in root_cmake:
        raise PolicyError("root CMakeLists does not include SparkFuzzPolicy.cmake")
    module = _decode(root, "cmake/SparkFuzzPolicy.cmake", "fuzz policy CMake module")
    for literal in ("check-fuzz-policy", "FuzzPolicy", "--ci", "fuzz-policy"):
        if literal not in module:
            raise PolicyError(f"fuzz policy CMake module is missing {literal!r}")
    standalone = _decode(root, "tools/fuzz-policy/CMakeLists.txt", "standalone fuzz policy CMakeLists")
    for literal in ("enable_testing()", "spark_enable_fuzz_policy"):
        if literal not in standalone:
            raise PolicyError(f"standalone fuzz policy CMakeLists is missing {literal!r}")


def build_check_report(root: Path, inventory_path: str, corpus_path: str) -> dict[str, Any]:
    inventory = build_inventory_report(root, inventory_path)
    corpus = build_corpus_report(root, inventory_path, corpus_path)
    blockers: list[str] = []
    if inventory["blocked_count"]:
        blockers.append(f"{inventory['blocked_count']} inventoried parsers have no fuzz target")
    if inventory["deferred_candidate_count"]:
        blockers.append(f"{inventory['deferred_candidate_count']} detected candidates await classification")
    if not corpus["bound_target_count"]:
        blockers.append("no corpus/resource budget is bound to a runnable fuzz target")
    return {
        "schema_version": 1,
        "passed": True,
        "policy_complete": False if blockers else inventory["policy_complete"],
        "inventory": {
            key: inventory[key]
            for key in (
                "parser_count",
                "fuzzed_count",
                "blocked_count",
                "candidate_count",
                "deferred_candidate_count",
                "unclassified_candidate_count",
                "scan_roots",
                "excluded_subtrees",
            )
        },
        "corpus": {
            key: corpus[key]
            for key in ("corpus_count", "seed_count", "seed_bytes", "bound_target_count", "max_staleness_days")
        },
        "closure_blockers": blockers,
    }


def validate_evidence(root: Path, expected: dict[str, Any], evidence_path: str) -> None:
    actual = load_json_document(root, evidence_path, "fuzz policy evidence")
    if actual != expected:
        raise PolicyError(
            f"{evidence_path} is stale; regenerate it from check_fuzz_policy.py --emit-json"
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", default=".")
    parser.add_argument("--inventory", default=DEFAULT_INVENTORY)
    parser.add_argument("--corpus", default=DEFAULT_CORPUS_MANIFEST)
    parser.add_argument("--evidence", default=DEFAULT_EVIDENCE)
    parser.add_argument("--ci", action="store_true")
    parser.add_argument("--emit-json", action="store_true")
    args = parser.parse_args(argv)
    root = Path(args.source_root)
    try:
        report = build_check_report(root, args.inventory, args.corpus)
        if args.ci:
            validate_ci_and_cmake_binding(root)
            validate_evidence(root, report, args.evidence)
    except (OSError, PolicyError) as exc:
        failure = {"schema_version": 1, "passed": False, "error": str(exc)}
        if args.emit_json:
            print(json.dumps(failure, indent=2, sort_keys=True))
        else:
            print(f"fuzz policy: FAIL: {exc}", file=sys.stderr)
        return 1
    if args.emit_json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(
            "fuzz policy: PASS "
            f"({report['inventory']['parser_count']} parsers, "
            f"{report['inventory']['deferred_candidate_count']} deferred candidates; "
            f"closure blockers: {len(report['closure_blockers'])})"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
