#!/usr/bin/env python3
"""CI-110 clang-tidy diagnostic budget ratchet.

The clang-tidy lane analyzes every shipped-product translation unit with
``--warnings-as-errors=""``, so individual diagnostics never fail the job on
their own. This tool turns the diagnostic stream into a ratchet: a committed
budget (``Tools/clang-tidy-budget.json``) records how many distinct diagnostics
each check produces in each file, and ``check`` fails when

* a (file, check) entry produces more diagnostics than its budget (a
  regression, reported with the file so it is actionable);
* a (file, check) pair that is not in the budget produces any diagnostic;
* a (file, check) entry produces fewer diagnostics than its budget (the budget
  is stale and must be lowered in the same change, so a cleanup can never be
  silently spent by a later regression elsewhere);
* the clang-tidy major version differs from the one the budget was measured
  with (counts from another toolchain are not comparable);
* the log or the budget is missing, unreadable or malformed.

Diagnostics are deduplicated by (repository-relative path, line, column, check,
message) before counting. A header included by many translation units reports
the same diagnostic once per translation unit; deduplication makes the count
independent of include fan-out and of the order parallel workers finish in.

``update`` rewrites the budget from a log. It is the only way to raise a
budget, and the resulting diff is what reviewers approve.

Exit status: 0 when the budget holds, 1 on a budget violation, 2 on unreadable
or malformed input.
"""

from __future__ import annotations

import argparse
import json
import os
import posixpath
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUDGET = REPO_ROOT / "Tools" / "clang-tidy-budget.json"
SCHEMA_VERSION = 1

# Diagnostics printed for one regressed or unbudgeted (file, check) entry. The
# full log is uploaded as a CI artifact; the job log only needs enough to act on.
MAX_REPORTED_PER_ENTRY = 40

_DIAGNOSTIC_RE = re.compile(
    r"^(?P<path>.+?):(?P<line>\d+):(?P<column>\d+): (?P<severity>warning|error): "
    r"(?P<message>.*) \[(?P<checks>[A-Za-z0-9_.,-]+)\]$"
)
_VERSION_RE = re.compile(r"LLVM version (?P<major>\d+)\.\d+")
_WARNINGS_AS_ERRORS_SUFFIX = "-warnings-as-errors"


class BudgetInputError(Exception):
    """Raised for missing, unreadable or malformed inputs (exit status 2)."""


@dataclass(frozen=True, order=True)
class Diagnostic:
    path: str
    line: int
    column: int
    check: str
    severity: str
    message: str

    def render(self) -> str:
        return f"{self.path}:{self.line}:{self.column}: {self.severity}: {self.message} [{self.check}]"


def is_posix_root(root: Path) -> bool:
    """True for a POSIX-style absolute root such as the CI runner's /home/runner/... checkout."""

    text = root.as_posix()
    return text.startswith("/") and not text.startswith("//")


def normalize_path(raw_path: str, repo_root: Path) -> str:
    """Return a repository-relative POSIX path, or the normalized absolute path outside it.

    A POSIX-style root (a log from the Linux clang-tidy lane) is normalized with
    POSIX path rules on every host, so replaying a CI log on Windows keys the same
    files as CI does; any other root uses the host's own path rules.
    """

    candidate = raw_path.strip()
    paths = posixpath if is_posix_root(repo_root) else os.path
    root_text = repo_root.as_posix() if paths is posixpath else str(repo_root)
    if not paths.isabs(candidate):
        candidate = paths.join(root_text, candidate)
    normalized = paths.normpath(candidate)
    root = paths.normpath(root_text)
    if normalized == root or normalized.startswith(root + paths.sep):
        return Path(paths.relpath(normalized, root)).as_posix()
    return Path(normalized).as_posix()


def normalize_check(raw_checks: str) -> str | None:
    """Return the check key for a bracket list, or None when it names no check.

    Compiler diagnostics surface as ``clang-diagnostic-*`` and are budgeted like
    any other check. clang-tidy appends ``,-warnings-as-errors`` when WarningsAsErrors promotes a
    check; the promotion is configuration, not a different check.
    """

    names = [name for name in raw_checks.split(",") if name and name != _WARNINGS_AS_ERRORS_SUFFIX]
    if not names:
        return None
    return ",".join(names)


def parse_diagnostics(text: str, repo_root: Path) -> set[Diagnostic]:
    """Extract the distinct clang-tidy diagnostics from raw clang-tidy output."""

    diagnostics: set[Diagnostic] = set()
    for line in text.splitlines():
        match = _DIAGNOSTIC_RE.match(line)
        if match is None:
            continue
        check = normalize_check(match.group("checks"))
        if check is None:
            continue
        diagnostics.add(
            Diagnostic(
                path=normalize_path(match.group("path"), repo_root),
                line=int(match.group("line")),
                column=int(match.group("column")),
                check=check,
                severity=match.group("severity"),
                message=match.group("message"),
            )
        )
    return diagnostics


def count_by_check(diagnostics: set[Diagnostic]) -> dict[str, int]:
    return dict(sorted(Counter(diagnostic.check for diagnostic in diagnostics).items()))


def count_by_file(diagnostics: set[Diagnostic]) -> dict[str, dict[str, int]]:
    """Return {path: {check: count}} with both levels sorted."""

    pairs = Counter((diagnostic.path, diagnostic.check) for diagnostic in diagnostics)
    files: dict[str, dict[str, int]] = {}
    for (path, check), count in sorted(pairs.items()):
        files.setdefault(path, {})[check] = count
    return files


def parse_major_version(version_text: str) -> int:
    match = _VERSION_RE.search(version_text)
    if match is None:
        raise BudgetInputError(f"cannot read an LLVM major version from clang-tidy --version output: {version_text!r}")
    return int(match.group("major"))


def read_text(path: Path, what: str) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise BudgetInputError(f"cannot read {what} {path}: {error}") from error


def _is_positive_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def load_budget(path: Path) -> dict:
    """Load and validate a budget; the per-check and total summaries must match the per-file entries."""

    try:
        budget = json.loads(read_text(path, "budget"))
    except json.JSONDecodeError as error:
        raise BudgetInputError(f"budget {path} is not valid JSON: {error}") from error
    if not isinstance(budget, dict):
        raise BudgetInputError(f"budget {path} must be a JSON object")
    if budget.get("schemaVersion") != SCHEMA_VERSION:
        raise BudgetInputError(f"budget {path} schemaVersion must be {SCHEMA_VERSION}")
    if not _is_positive_int(budget.get("clangTidyMajorVersion")):
        raise BudgetInputError(f"budget {path} clangTidyMajorVersion must be a positive integer")
    files = budget.get("files")
    if not isinstance(files, dict):
        raise BudgetInputError(f"budget {path} files must be an object")
    per_check: Counter[str] = Counter()
    for file_path, checks in files.items():
        if not isinstance(checks, dict) or not checks:
            raise BudgetInputError(f"budget {path} file {file_path!r} must map to a non-empty object of checks")
        for check, allowed in checks.items():
            if not _is_positive_int(allowed):
                raise BudgetInputError(
                    f"budget {path} {file_path} {check!r} must be a positive integer; remove entries that reach zero"
                )
            per_check[check] += allowed
    if budget.get("checks") != dict(sorted(per_check.items())):
        raise BudgetInputError(f"budget {path} checks summary does not match the per-file entries")
    if budget.get("total") != sum(per_check.values()):
        raise BudgetInputError(f"budget {path} total {budget.get('total')!r} does not match the per-file entries")
    return budget


def evaluate(budget: dict, diagnostics: set[Diagnostic], major_version: int) -> list[str]:
    """Return every ratchet violation for the measured diagnostics, one per (file, check)."""

    budget_major = budget["clangTidyMajorVersion"]
    if major_version != budget_major:
        return [
            f"clang-tidy {major_version} ran but the budget was measured with clang-tidy {budget_major}; "
            "regenerate the budget with the new toolchain (Tools/clang_tidy_budget.py update)"
        ]

    allowed_files = budget["files"]
    measured_files = count_by_file(diagnostics)
    by_pair: dict[tuple[str, str], list[Diagnostic]] = {}
    for diagnostic in sorted(diagnostics):
        by_pair.setdefault((diagnostic.path, diagnostic.check), []).append(diagnostic)

    pairs = {(path, check) for path, checks in allowed_files.items() for check in checks}
    pairs |= set(by_pair)
    errors: list[str] = []
    for path, check in sorted(pairs):
        limit = allowed_files.get(path, {}).get(check, 0)
        count = measured_files.get(path, {}).get(check, 0)
        if count > limit:
            label = "not in the budget" if limit == 0 else f"over its budget of {limit}"
            lines = [f"{path} [{check}]: {count} diagnostic(s), {label} (+{count - limit})"]
            reported = by_pair[(path, check)][:MAX_REPORTED_PER_ENTRY]
            lines.extend(f"    {diagnostic.render()}" for diagnostic in reported)
            if count > len(reported):
                lines.append(f"    ... {count - len(reported)} more in the uploaded clang-tidy log")
            errors.append("\n".join(lines))
        elif count < limit:
            errors.append(
                f"{path} [{check}]: budget {limit} is stale, only {count} diagnostic(s) remain; lower the budget "
                "in this change so the improvement is locked in (Tools/clang_tidy_budget.py update)"
            )
    return errors


def render_summary(budget: dict, diagnostics: set[Diagnostic]) -> str:
    allowed = budget["checks"]
    measured = count_by_check(diagnostics)
    rows = ["| Check | Budget | Measured | Delta |", "|---|---:|---:|---:|"]
    for check in sorted(set(allowed) | set(measured)):
        limit = allowed.get(check, 0)
        count = measured.get(check, 0)
        rows.append(f"| `{check}` | {limit} | {count} | {count - limit:+d} |")
    measured_total = sum(measured.values())
    rows.append(f"| **total** | {budget['total']} | {measured_total} | {measured_total - budget['total']:+d} |")
    return "\n".join(rows)


def build_budget(diagnostics: set[Diagnostic], major_version: int, measured_at: str) -> dict:
    checks = count_by_check(diagnostics)
    return {
        "schemaVersion": SCHEMA_VERSION,
        "description": (
            "CI-110 clang-tidy diagnostic budget: distinct diagnostics per file and check across every "
            "shipped-product translation unit, measured with the clang-tidy CI lane configuration. The checks and "
            "total fields summarize the files map. Lower entries as diagnostics are fixed; raising one is a "
            "reviewed exception. Regenerate with Tools/clang_tidy_budget.py update."
        ),
        "clangTidyMajorVersion": major_version,
        "measuredAt": measured_at,
        "total": sum(checks.values()),
        "checks": checks,
        "files": count_by_file(diagnostics),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    subcommands = parser.add_subparsers(dest="command", required=True)
    for name, help_text in (
        ("check", "fail when the measured diagnostics differ from the budget"),
        ("update", "rewrite the budget from a clang-tidy log"),
    ):
        sub = subcommands.add_parser(name, help=help_text)
        sub.add_argument("--log", type=Path, required=True, help="concatenated clang-tidy output")
        sub.add_argument("--budget", type=Path, default=DEFAULT_BUDGET)
        sub.add_argument("--repo-root", type=Path, default=REPO_ROOT, help="checkout root the log's paths use")
        sub.add_argument(
            "--clang-tidy-version-file",
            type=Path,
            required=True,
            help="file holding `clang-tidy --version` output from the run that produced the log",
        )
    subcommands.choices["check"].add_argument(
        "--summary", type=Path, help="append a Markdown budget table (for $GITHUB_STEP_SUMMARY)"
    )
    subcommands.choices["update"].add_argument(
        "--measured-at", required=True, help="provenance of the log, e.g. a commit SHA or CI run reference"
    )
    args = parser.parse_args(argv)

    try:
        major_version = parse_major_version(read_text(args.clang_tidy_version_file, "clang-tidy version file"))
        # Resolving a POSIX runner root on Windows would graft it onto the current
        # drive (D:\home\runner\...), so such a root is taken as written there.
        repo_root = args.repo_root
        if not (os.name == "nt" and is_posix_root(repo_root)):
            repo_root = repo_root.resolve()
        log_text = read_text(args.log, "clang-tidy log")
        if not log_text.strip():
            # clang-tidy prints at least its warning-suppression summary per
            # translation unit, so an empty log means the analysis never ran.
            raise BudgetInputError(f"clang-tidy log {args.log} is empty")
        diagnostics = parse_diagnostics(log_text, repo_root)
        if args.command == "update":
            budget = build_budget(diagnostics, major_version, args.measured_at)
            args.budget.write_text(json.dumps(budget, indent=2) + "\n", encoding="utf-8")
            print(f"clang-tidy budget written: {budget['total']} diagnostic(s) across {len(budget['checks'])} check(s)")
            return 0
        budget = load_budget(args.budget)
    except BudgetInputError as error:
        print(f"::error::{error}", file=sys.stderr)
        return 2

    summary = render_summary(budget, diagnostics)
    print(summary)
    if args.summary is not None:
        with args.summary.open("a", encoding="utf-8") as handle:
            handle.write("## clang-tidy diagnostic budget\n\n" + summary + "\n")

    errors = evaluate(budget, diagnostics, major_version)
    for error in errors:
        print(f"::error::{error}", file=sys.stderr)
    if errors:
        print(f"clang-tidy budget: {len(errors)} violation(s)", file=sys.stderr)
        return 1
    print(f"clang-tidy budget holds: {budget['total']} diagnostic(s) across {len(budget['checks'])} check(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
