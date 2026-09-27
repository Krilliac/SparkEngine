#!/usr/bin/env python3
"""Adversarial tests for the CI-110 clang-tidy diagnostic budget ratchet."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL = REPO_ROOT / "Tools" / "clang_tidy_budget.py"
COMMITTED_BUDGET = REPO_ROOT / "Tools" / "clang-tidy-budget.json"
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"

sys.path.insert(0, str(REPO_ROOT / "Tools"))

import clang_tidy_budget  # noqa: E402

# Checkout root the fixture logs pretend clang-tidy ran in (GitHub's layout).
RUNNER_ROOT = "/home/runner/work/SparkEngine/SparkEngine"
VERSION_18 = "Ubuntu LLVM version 18.1.3\n  Optimized build.\n"

HEADER_DIAGNOSTIC = (
    f"{RUNNER_ROOT}/SparkEngine/Source/Engine/Tween/TweenSystem.h:190:21: warning: the parameter 'tweens' is "
    "copied for each invocation but only used as a const reference; consider making it a const reference "
    "[performance-unnecessary-value-param]"
)
SOURCE_DIAGNOSTIC = (
    f"{RUNNER_ROOT}/SparkEngine/Source/Engine/Tween/TweenSystem.cpp:42:5: warning: use range-based for loop "
    "instead [modernize-loop-convert]"
)
PROMOTED_DIAGNOSTIC = (
    f"{RUNNER_ROOT}/SparkEditor/Source/Panels/Panel.cpp:7:1: error: statement should be inside braces "
    "[readability-braces-around-statements,-warnings-as-errors]"
)
COMPILER_ERROR = f"{RUNNER_ROOT}/SparkEngine/Source/Broken.cpp:3:1: error: unknown type name 'Foo' [clang-diagnostic-error]"
NOTE_LINE = f"{RUNNER_ROOT}/SparkEngine/Source/Engine/Tween/TweenSystem.h:12:5: note: previous declaration is here"


TWEEN_H = "SparkEngine/Source/Engine/Tween/TweenSystem.h"
TWEEN_CPP = "SparkEngine/Source/Engine/Tween/TweenSystem.cpp"
PANEL_CPP = "SparkEditor/Source/Panels/Panel.cpp"


def budget_document(files: dict[str, dict[str, int]], major: int = 18) -> dict:
    checks: dict[str, int] = {}
    for file_checks in files.values():
        for check, count in file_checks.items():
            checks[check] = checks.get(check, 0) + count
    return {
        "schemaVersion": 1,
        "description": "fixture",
        "clangTidyMajorVersion": major,
        "measuredAt": "fixture",
        "total": sum(checks.values()),
        "checks": dict(sorted(checks.items())),
        "files": files,
    }


class ClangTidyBudgetCliTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        self.version = self.root / "version.txt"
        self.version.write_text(VERSION_18, encoding="utf-8")

    def tearDown(self) -> None:
        self._temp.cleanup()

    def run_tool(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(TOOL), *args],
            capture_output=True,
            text=True,
            check=False,
        )

    def check(self, log_lines: list[str], files: dict[str, dict[str, int]] | None, *, major: int = 18, raw_budget: str | None = None):
        log = self.root / "clang-tidy-output.log"
        log.write_text("\n".join(log_lines) + "\n", encoding="utf-8")
        budget = self.root / "budget.json"
        if raw_budget is not None:
            budget.write_text(raw_budget, encoding="utf-8")
        elif files is not None:
            budget.write_text(json.dumps(budget_document(files, major)), encoding="utf-8")
        return self.run_tool(
            "check",
            "--log", str(log),
            "--budget", str(budget),
            "--repo-root", RUNNER_ROOT,
            "--clang-tidy-version-file", str(self.version),
        )

    def test_exact_budget_passes(self) -> None:
        result = self.check(
            [HEADER_DIAGNOSTIC, SOURCE_DIAGNOSTIC, "2 warnings generated."],
            {TWEEN_CPP: {"modernize-loop-convert": 1}, TWEEN_H: {"performance-unnecessary-value-param": 1}},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("clang-tidy budget holds: 2 diagnostic(s)", result.stdout)

    def test_header_repeated_across_translation_units_counts_once(self) -> None:
        # Two TUs include the same header: the ratchet must not double count.
        result = self.check(
            [HEADER_DIAGNOSTIC, "1 warning generated.", HEADER_DIAGNOSTIC, "1 warning generated.", NOTE_LINE],
            {TWEEN_H: {"performance-unnecessary-value-param": 1}},
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_regression_over_budget_fails_and_lists_diagnostics(self) -> None:
        second = SOURCE_DIAGNOSTIC.replace(":42:5:", ":77:9:")
        result = self.check([SOURCE_DIAGNOSTIC, second], {TWEEN_CPP: {"modernize-loop-convert": 1}})
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"{TWEEN_CPP} [modernize-loop-convert]: 2 diagnostic(s), over its budget of 1 (+1)", result.stderr)
        self.assertIn("SparkEngine/Source/Engine/Tween/TweenSystem.cpp:77:9", result.stderr)

    def test_unbudgeted_check_fails(self) -> None:
        result = self.check([SOURCE_DIAGNOSTIC, HEADER_DIAGNOSTIC], {TWEEN_CPP: {"modernize-loop-convert": 1}})
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"{TWEEN_H} [performance-unnecessary-value-param]: 1 diagnostic(s), not in the budget", result.stderr)

    def test_stale_budget_after_cleanup_fails(self) -> None:
        result = self.check([SOURCE_DIAGNOSTIC], {TWEEN_CPP: {"modernize-loop-convert": 3}})
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"{TWEEN_CPP} [modernize-loop-convert]: budget 3 is stale, only 1 diagnostic(s) remain", result.stderr)

    def test_budgeted_check_that_disappears_entirely_is_stale(self) -> None:
        result = self.check(["Suppressed 10 warnings (10 in non-user code)."], {TWEEN_CPP: {"modernize-loop-convert": 1}})
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"{TWEEN_CPP} [modernize-loop-convert]: budget 1 is stale, only 0", result.stderr)

    def test_diagnostic_moved_to_another_file_fails_in_both_files(self) -> None:
        # Same check total, different file: the per-file ratchet still reports
        # exactly where the new diagnostic landed.
        moved = SOURCE_DIAGNOSTIC.replace("Tween/TweenSystem.cpp", "Tween/TweenEasing.cpp")
        result = self.check([moved], {TWEEN_CPP: {"modernize-loop-convert": 1}})
        self.assertEqual(result.returncode, 1)
        self.assertIn("SparkEngine/Source/Engine/Tween/TweenEasing.cpp [modernize-loop-convert]: 1 diagnostic(s), not in the budget", result.stderr)
        self.assertIn(f"{TWEEN_CPP} [modernize-loop-convert]: budget 1 is stale", result.stderr)

    def test_warnings_as_errors_promotion_keeps_the_check_key(self) -> None:
        result = self.check([PROMOTED_DIAGNOSTIC], {PANEL_CPP: {"readability-braces-around-statements": 1}})
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_compiler_diagnostics_are_budgeted_like_checks(self) -> None:
        # clang-diagnostic-* are compiler diagnostics reported through clang-tidy.
        # A compile error already fails the run step through clang-tidy's exit
        # status; the ratchet still treats it as a named, unbudgeted check.
        result = self.check([COMPILER_ERROR], {})
        self.assertEqual(result.returncode, 1)
        self.assertIn("SparkEngine/Source/Broken.cpp [clang-diagnostic-error]: 1 diagnostic(s), not in the budget", result.stderr)

    def test_toolchain_major_version_mismatch_fails(self) -> None:
        result = self.check([SOURCE_DIAGNOSTIC], {TWEEN_CPP: {"modernize-loop-convert": 1}}, major=17)
        self.assertEqual(result.returncode, 1)
        self.assertIn("clang-tidy 18 ran but the budget was measured with clang-tidy 17", result.stderr)

    def test_empty_log_is_an_input_error(self) -> None:
        log = self.root / "empty.log"
        log.write_text("", encoding="utf-8")
        budget = self.root / "budget.json"
        budget.write_text(json.dumps(budget_document({})), encoding="utf-8")
        result = self.run_tool(
            "check", "--log", str(log), "--budget", str(budget), "--repo-root", RUNNER_ROOT,
            "--clang-tidy-version-file", str(self.version),
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("is empty", result.stderr)

    def test_missing_log_is_an_input_error(self) -> None:
        result = self.run_tool(
            "check", "--log", str(self.root / "absent.log"), "--budget", str(COMMITTED_BUDGET),
            "--repo-root", RUNNER_ROOT, "--clang-tidy-version-file", str(self.version),
        )
        self.assertEqual(result.returncode, 2)

    def test_unreadable_version_is_an_input_error(self) -> None:
        self.version.write_text("clang-tidy (unknown)\n", encoding="utf-8")
        result = self.check([SOURCE_DIAGNOSTIC], {TWEEN_CPP: {"modernize-loop-convert": 1}})
        self.assertEqual(result.returncode, 2)
        self.assertIn("cannot read an LLVM major version", result.stderr)

    def test_malformed_budgets_are_input_errors(self) -> None:
        good = budget_document({TWEEN_CPP: {"a": 1}})
        cases = {
            "not json": "{",
            "not an object": "[]",
            "wrong schema": json.dumps({**good, "schemaVersion": 2}),
            "zero entry": json.dumps(budget_document({TWEEN_CPP: {"a": 0}})),
            "negative entry": json.dumps(budget_document({TWEEN_CPP: {"a": -1}})),
            "boolean entry": json.dumps({**good, "files": {TWEEN_CPP: {"a": True}}}),
            "empty file entry": json.dumps({**good, "files": {TWEEN_CPP: {"a": 1}, TWEEN_H: {}}}),
            "files missing": json.dumps({k: v for k, v in good.items() if k != "files"}),
            "checks summary mismatch": json.dumps({**good, "checks": {"a": 2}}),
            "total mismatch": json.dumps({**good, "total": 5}),
            "missing version": json.dumps({k: v for k, v in good.items() if k != "clangTidyMajorVersion"}),
        }
        for label, raw in cases.items():
            with self.subTest(label):
                result = self.check([SOURCE_DIAGNOSTIC], None, raw_budget=raw)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    def test_update_then_check_round_trips(self) -> None:
        log = self.root / "clang-tidy-output.log"
        log.write_text("\n".join([HEADER_DIAGNOSTIC, SOURCE_DIAGNOSTIC, HEADER_DIAGNOSTIC]) + "\n", encoding="utf-8")
        budget = self.root / "budget.json"
        common = ["--log", str(log), "--budget", str(budget), "--repo-root", RUNNER_ROOT,
                  "--clang-tidy-version-file", str(self.version)]
        updated = self.run_tool("update", *common, "--measured-at", "fixture")
        self.assertEqual(updated.returncode, 0, updated.stderr)
        document = json.loads(budget.read_text(encoding="utf-8"))
        self.assertEqual(document["checks"], {"modernize-loop-convert": 1, "performance-unnecessary-value-param": 1})
        self.assertEqual(
            document["files"],
            {TWEEN_CPP: {"modernize-loop-convert": 1}, TWEEN_H: {"performance-unnecessary-value-param": 1}},
        )
        self.assertEqual(document["total"], 2)
        self.assertEqual(document["clangTidyMajorVersion"], 18)
        checked = self.run_tool("check", *common)
        self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_summary_table_is_appended(self) -> None:
        summary = self.root / "summary.md"
        log = self.root / "clang-tidy-output.log"
        log.write_text(SOURCE_DIAGNOSTIC + "\n", encoding="utf-8")
        budget = self.root / "budget.json"
        budget.write_text(json.dumps(budget_document({TWEEN_CPP: {"modernize-loop-convert": 1}})), encoding="utf-8")
        result = self.run_tool(
            "check", "--log", str(log), "--budget", str(budget), "--repo-root", RUNNER_ROOT,
            "--clang-tidy-version-file", str(self.version), "--summary", str(summary),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("| `modernize-loop-convert` | 1 | 1 | +0 |", summary.read_text(encoding="utf-8"))


class ClangTidyBudgetParsingTests(unittest.TestCase):
    def test_paths_are_repository_relative_whatever_the_checkout_root(self) -> None:
        local = HEADER_DIAGNOSTIC.replace(RUNNER_ROOT, "/home/user/SparkEngine")
        runner = clang_tidy_budget.parse_diagnostics(HEADER_DIAGNOSTIC, Path(RUNNER_ROOT))
        workstation = clang_tidy_budget.parse_diagnostics(local, Path("/home/user/SparkEngine"))
        self.assertEqual(runner, workstation)
        self.assertEqual(next(iter(runner)).path, "SparkEngine/Source/Engine/Tween/TweenSystem.h")

    def test_dot_dot_segments_are_normalized_before_deduplication(self) -> None:
        dotted = HEADER_DIAGNOSTIC.replace("/Engine/Tween/", "/Engine/Animation/../Tween/")
        parsed = clang_tidy_budget.parse_diagnostics(HEADER_DIAGNOSTIC + "\n" + dotted, Path(RUNNER_ROOT))
        self.assertEqual(len(parsed), 1)

    def test_notes_and_summaries_are_not_diagnostics(self) -> None:
        text = "\n".join([NOTE_LINE, "3 warnings generated.", "Suppressed 3 warnings (3 in non-user code)."])
        self.assertEqual(clang_tidy_budget.parse_diagnostics(text, Path(RUNNER_ROOT)), set())


class CommittedBudgetTests(unittest.TestCase):
    def test_committed_budget_is_well_formed_for_the_ci_toolchain(self) -> None:
        budget = clang_tidy_budget.load_budget(COMMITTED_BUDGET)
        # The clang-tidy lane runs on ubuntu-24.04, whose clang-tidy package is LLVM 18.
        self.assertEqual(budget["clangTidyMajorVersion"], 18)
        self.assertTrue(budget["measuredAt"].strip())
        self.assertEqual(list(budget["checks"]), sorted(budget["checks"]))
        self.assertEqual(list(budget["files"]), sorted(budget["files"]))
        # Every budgeted path is repository-relative: an absolute path would
        # only ever match the checkout it was measured in.
        for path in budget["files"]:
            self.assertFalse(path.startswith("/"), path)
            self.assertTrue((REPO_ROOT / path).is_file(), path)

    def test_committed_budget_names_only_tracked_files(self) -> None:
        # CI analyzes a clean checkout. A budget measured over a working tree
        # that holds uncommitted sources names files CI never sees, and the
        # ratchet then reports every one of their entries as stale.
        listing = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "ls-files", "-z"], capture_output=True, check=False
        )
        if listing.returncode != 0:
            self.skipTest("not a git checkout")
        tracked = set(listing.stdout.decode("utf-8").split("\0"))
        budget = clang_tidy_budget.load_budget(COMMITTED_BUDGET)
        untracked = sorted(path for path in budget["files"] if path not in tracked)
        self.assertEqual(untracked, [])

    def test_workflow_enforces_the_budget_after_the_analysis(self) -> None:
        workflow = BUILD_WORKFLOW.read_text(encoding="utf-8")
        block = workflow[workflow.index("\n  clang-tidy:\n"):]
        block = block[:block.index("\n  # ===========================================================================", 1)]
        run_index = block.index("- name: Run clang-tidy\n")
        budget_index = block.index("- name: Enforce clang-tidy diagnostic budget\n")
        self.assertLess(run_index, budget_index)
        # A generated header missing at analysis time is a parse error, not a
        # diagnostic, and fails the lane before the ratchet runs.
        generate_index = block.index("run: cmake --build build --target SparkServerBuildIdentity\n")
        self.assertLess(generate_index, run_index)
        budget_step = block[budget_index:block.index("\n      - name:", budget_index + 1)]
        self.assertIn("python3 Tools/clang_tidy_budget.py check", budget_step)
        self.assertIn("--log clang-tidy-output.log", budget_step)
        self.assertIn("--budget Tools/clang-tidy-budget.json", budget_step)
        self.assertIn("--clang-tidy-version-file clang-tidy-version.txt", budget_step)
        self.assertNotIn("continue-on-error", budget_step)
        self.assertNotIn("|| true", budget_step)
        self.assertNotIn("if: always()", budget_step)
        run_step = block[run_index:budget_index]
        # One log per translation unit: parallel workers must never interleave
        # partial diagnostic lines in a shared stream the ratchet parses.
        self.assertIn('> "$0/${1//\\//__}.log" 2>&1', run_step)
        self.assertIn("clang-tidy --version > clang-tidy-version.txt", run_step)


if __name__ == "__main__":
    unittest.main()
