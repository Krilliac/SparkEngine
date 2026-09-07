#!/usr/bin/env python3
"""Blocking SEC-120 policy gate with deterministic evidence verification.

Two verdicts, kept apart on purpose:

* the **structural gate** (``--ci``) proves the inventory, the exclusions, the
  build registration and the corpus bindings are well formed. It is what blocks
  a merge.
* the **closure gate** (``--require-closure``) proves SEC-120 is actually done.
  It fails while any parser lacks a sanitizer-backed fuzz target, and it is what
  keeps SEC-120 release-blocking.

``passed`` reflects the closure verdict, so it can never read ``true`` while
blockers exist.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

from build_binding import commands_named, parse_cmake
from corpus_manifest import DEFAULT_CORPUS_MANIFEST, build_corpus_report
from parser_inventory import DEFAULT_INVENTORY, build_inventory_report, load_inventory
from policy_common import Deadline, PolicyError, load_json_document, read_confined_file


DEFAULT_EVIDENCE = "docs/sec120-fuzz-policy-check.json"
DEFAULT_LEDGER = "docs/sec120-fuzz-policy-evidence.json"
WORKFLOW = ".github/workflows/build.yml"
FUZZ_JOB = "fuzz-policy"
GATE_SECONDS = 300

REQUIRED_JOB_COMMANDS = (
    "cmake -S tools/fuzz-policy -B build/fuzz-policy",
    "cmake --build build/fuzz-policy --target check-fuzz-policy",
    "ctest --test-dir build/fuzz-policy --output-on-failure --no-tests=error -C Release",
)
# Only meaningful once a fuzz target exists; asserted conditionally below.
FUZZ_SMOKE_COMMAND = "ctest --test-dir build/fuzz-policy --output-on-failure -L fuzz -C Release"


def _decode(root: Path, path: str, field: str, maximum: int = 2 * 1024 * 1024) -> str:
    payload = read_confined_file(root, path, field, max_bytes=maximum)
    try:
        return payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise PolicyError(f"{field} must be strict UTF-8") from exc


def _digest(root: Path, path: str, field: str) -> str:
    return hashlib.sha256(read_confined_file(root, path, field, max_bytes=1024 * 1024)).hexdigest()


def _strip_yaml_comment(line: str) -> str:
    """Drop a trailing YAML comment without touching '#' inside quotes."""
    quote: str | None = None
    for index, char in enumerate(line):
        if quote is not None:
            if char == quote:
                quote = None
            continue
        if char in "\"'":
            quote = char
            continue
        if char == "#" and (index == 0 or line[index - 1].isspace()):
            return line[:index]
    return line


def _job_block(workflow: str, job_name: str) -> list[str]:
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
    return lines[start:end]


def _run_commands(block: list[str]) -> set[str]:
    """Collect the shell commands a job actually executes.

    A literal that appears in a comment, a job name or an ``echo`` is not a
    command; only ``run:`` scalars and ``run: |`` block bodies count.
    """
    commands: set[str] = set()
    index = 0
    while index < len(block):
        line = _strip_yaml_comment(block[index])
        match = re.match(r"^(\s*)-?\s*run:\s*(.*)$", line)
        if match is None:
            index += 1
            continue
        indent, inline = match.group(1), match.group(2).strip()
        if inline and inline not in ("|", ">", "|-", ">-"):
            commands.add(inline)
            index += 1
            continue
        index += 1
        while index < len(block):
            body = _strip_yaml_comment(block[index])
            if body.strip() and not body.startswith(indent + " "):
                break
            if body.strip():
                commands.add(body.strip())
            index += 1
    return commands


def _assert_job_is_live(block: list[str], job_name: str) -> None:
    for line in block[1:]:
        stripped = _strip_yaml_comment(line)
        if re.fullmatch(r"  [A-Za-z0-9_-]+:", stripped):
            break
        key = re.match(r"^    ([A-Za-z0-9_-]+):\s*(.*)$", stripped)
        if key is None:
            continue
        name, value = key.group(1), key.group(2).strip()
        if name == "if":
            raise PolicyError(f"{job_name} job must not be conditional (found 'if: {value}')")
        if name == "continue-on-error" and value.lower() not in ("", "false"):
            raise PolicyError(f"{job_name} job must not set continue-on-error: {value}")


def validate_ci_and_cmake_binding(root: Path, *, fuzz_target_count: int) -> None:
    workflow = _decode(root, WORKFLOW, "build workflow")
    fuzz_job = _job_block(workflow, FUZZ_JOB)
    _assert_job_is_live(fuzz_job, FUZZ_JOB)
    if "    runs-on: ubuntu-24.04" not in [_strip_yaml_comment(line).rstrip() for line in fuzz_job]:
        raise PolicyError(f"{FUZZ_JOB} job must run on ubuntu-24.04")
    commands = _run_commands(fuzz_job)
    for literal in REQUIRED_JOB_COMMANDS:
        if literal not in commands:
            raise PolicyError(f"{FUZZ_JOB} CI job does not run {literal!r}")
    if fuzz_target_count and FUZZ_SMOKE_COMMAND not in commands:
        raise PolicyError(
            f"{fuzz_target_count} fuzz targets are declared but the CI job never runs {FUZZ_SMOKE_COMMAND!r}"
        )

    gate = _job_block(workflow, "required-ci-gate")
    if not any(re.fullmatch(r"\s+- fuzz-policy", _strip_yaml_comment(line).rstrip()) for line in gate):
        raise PolicyError("Required CI Gate does not depend on fuzz-policy")

    root_commands = parse_cmake(_decode(root, "CMakeLists.txt", "root CMakeLists"), "root CMakeLists")
    if not any(
        any(argument.endswith("cmake/SparkFuzzPolicy.cmake") for argument in command.arguments)
        for command in commands_named(root_commands, "include")
    ):
        raise PolicyError("root CMakeLists does not include cmake/SparkFuzzPolicy.cmake")
    if not commands_named(root_commands, "spark_enable_fuzz_policy"):
        raise PolicyError("root CMakeLists never calls spark_enable_fuzz_policy()")

    module = parse_cmake(_decode(root, "cmake/SparkFuzzPolicy.cmake", "fuzz policy CMake module"), "fuzz policy module")
    module_names = {command.name for command in module}
    if "function" not in module_names or not any(
        command.arguments and command.arguments[0] == "spark_enable_fuzz_policy"
        for command in commands_named(module, "function")
    ):
        raise PolicyError("fuzz policy CMake module does not define spark_enable_fuzz_policy()")
    registered = {
        command.arguments[1]
        for command in commands_named(module, "add_test")
        if len(command.arguments) > 1 and command.arguments[0].upper() == "NAME"
    }
    for required in ("FuzzPolicy", "FuzzPolicyAdversarial"):
        if required not in registered:
            raise PolicyError(f"fuzz policy CMake module does not register the {required} test")
    if not commands_named(module, "option"):
        raise PolicyError("fuzz policy CMake module does not gate its Python dependency behind an option()")

    standalone = parse_cmake(
        _decode(root, "tools/fuzz-policy/CMakeLists.txt", "standalone fuzz policy CMakeLists"),
        "standalone fuzz policy CMakeLists",
    )
    standalone_names = {command.name for command in standalone}
    for required in ("enable_testing", "spark_enable_fuzz_policy"):
        if required not in standalone_names:
            raise PolicyError(f"standalone fuzz policy CMakeLists is missing {required}()")


def build_check_report(
    root: Path,
    inventory_path: str,
    corpus_path: str,
    *,
    as_of: Any = None,
    deadline: Deadline | None = None,
) -> dict[str, Any]:
    deadline = deadline or Deadline(GATE_SECONDS, "fuzz policy gate")
    # One read of the inventory feeds both halves of the report; two independent
    # reads could describe a manifest that was never target-validated.
    inventory_document = load_inventory(root, inventory_path, as_of=as_of)
    inventory = build_inventory_report(root, inventory_path, as_of=as_of, deadline=deadline)
    corpus = build_corpus_report(root, inventory_document, corpus_path, as_of=as_of, deadline=deadline)

    blockers: list[str] = []
    if inventory["blocked_count"]:
        blockers.append(f"{inventory['blocked_count']} inventoried parsers have no fuzz target")
    if inventory["deferred_candidate_count"]:
        blockers.append(f"{inventory['deferred_candidate_count']} detected candidates await classification")
    if not corpus["bound_target_count"]:
        blockers.append("no corpus/resource budget is bound to a runnable fuzz target")

    return {
        "schema_version": 1,
        "passed": not blockers,
        "structural_gate": True,
        "policy_complete": False if blockers else inventory["policy_complete"],
        "coverage_claim": False,
        "runtime_evidence": {
            "sanitizer_smoke_executed": False,
            "scheduled_campaign": False,
        },
        "inputs": {
            "inventory": _digest(root, inventory_path, "inventory digest"),
            "corpus_manifest": _digest(root, corpus_path, "corpus digest"),
        },
        "inventory": {
            key: inventory[key]
            for key in (
                "parser_count",
                "fuzzed_count",
                "blocked_count",
                "candidate_count",
                "scanned_file_count",
                "deferred_candidate_count",
                "unclassified_candidate_count",
                "scan_roots",
                "excluded_subtrees",
                "excluded_files",
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
        raise PolicyError(f"{evidence_path} is stale; regenerate it from check_fuzz_policy.py --emit-json")


def validate_ledger(root: Path, report: dict[str, Any], ledger_path: str) -> None:
    """The narrative ledger must not out-claim the computed report."""
    ledger = load_json_document(root, ledger_path, "fuzz policy ledger")
    if not isinstance(ledger, dict):
        raise PolicyError(f"{ledger_path} must be an object")
    for key in ("status", "blocking", "closure_claim", "verified_snapshot", "not_completed"):
        if key not in ledger:
            raise PolicyError(f"{ledger_path} is missing {key}")
    if ledger["verified_snapshot"] != DEFAULT_EVIDENCE:
        raise PolicyError(f"{ledger_path}.verified_snapshot must name {DEFAULT_EVIDENCE}")
    if ledger["closure_claim"] is not False and report["closure_blockers"]:
        raise PolicyError(f"{ledger_path} claims closure while {len(report['closure_blockers'])} blockers remain")
    if report["closure_blockers"]:
        if ledger["status"] != "open" or ledger["blocking"] is not True:
            raise PolicyError(f"{ledger_path} must stay open and blocking while closure blockers remain")
        if not isinstance(ledger["not_completed"], list) or not ledger["not_completed"]:
            raise PolicyError(f"{ledger_path}.not_completed must list the outstanding work")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", default=".")
    parser.add_argument("--inventory", default=DEFAULT_INVENTORY)
    parser.add_argument("--corpus", default=DEFAULT_CORPUS_MANIFEST)
    parser.add_argument("--evidence", default=DEFAULT_EVIDENCE)
    parser.add_argument("--ledger", default=DEFAULT_LEDGER)
    parser.add_argument("--ci", action="store_true", help="also verify CI/CMake wiring and committed evidence")
    parser.add_argument(
        "--require-closure",
        action="store_true",
        help="fail while SEC-120 closure blockers remain (the release gate)",
    )
    parser.add_argument("--emit-json", action="store_true")
    parser.add_argument("--as-of-date", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    root = Path(args.source_root)
    as_of = None
    if args.as_of_date:
        from datetime import date

        try:
            as_of = date.fromisoformat(args.as_of_date)
        except ValueError:
            print("invalid --as-of-date", file=sys.stderr)
            return 2
    try:
        report = build_check_report(root, args.inventory, args.corpus, as_of=as_of)
        if args.ci:
            validate_ci_and_cmake_binding(root, fuzz_target_count=report["inventory"]["fuzzed_count"])
            validate_evidence(root, report, args.evidence)
            validate_ledger(root, report, args.ledger)
    except (OSError, PolicyError) as exc:
        failure = {"schema_version": 1, "passed": False, "structural_gate": False, "error": str(exc)}
        if args.emit_json:
            print(json.dumps(failure, indent=2, sort_keys=True))
        else:
            print(f"fuzz policy: FAIL: {exc}", file=sys.stderr)
        return 1

    if args.emit_json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(
            "fuzz policy: structural gate PASS "
            f"({report['inventory']['parser_count']} parsers, "
            f"{report['inventory']['fuzzed_count']} fuzzed, "
            f"{report['inventory']['deferred_candidate_count']} deferred candidates); "
            f"SEC-120 closure blockers: {len(report['closure_blockers'])}"
        )
    if args.require_closure and report["closure_blockers"]:
        for blocker in report["closure_blockers"]:
            print(f"fuzz policy: SEC-120 closure blocker: {blocker}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
