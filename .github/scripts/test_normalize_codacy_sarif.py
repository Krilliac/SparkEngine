#!/usr/bin/env python3
"""Regression contract for filtered Codacy SARIF and stable GitHub categories."""

from __future__ import annotations

import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("normalize-codacy-sarif.py")
SPEC = importlib.util.spec_from_file_location("normalize_codacy_sarif", SCRIPT)
assert SPEC and SPEC.loader
normalizer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(normalizer)

TOOL_NAME = "Cppcheck (reported by Codacy)"


def result(
    index: int,
    *,
    level: str = "warning",
    rule_id: str | None = None,
    language: str = "cpp",
    with_location: bool = True,
) -> dict[str, object]:
    suffix = "c" if language == "c" else "cpp"
    item: dict[str, object] = {
        "ruleId": rule_id or f"rule-{index % 2}",
        "level": level,
        "message": {"text": f"finding {index}"},
    }
    if with_location:
        item["locations"] = [
            {
                "physicalLocation": {
                    "artifactLocation": {"uri": f"src/file-{index}.{suffix}"},
                    "region": {"startLine": index + 1},
                }
            }
        ]
    return item


def run(language: str, results: list[dict[str, object]]) -> dict[str, object]:
    suffix = "c" if language == "c" else "cpp"
    return {
        "tool": {"driver": {"name": TOOL_NAME}},
        "artifacts": [{"location": {"uri": f"src/all.{suffix}"}}],
        "results": results,
    }


def payload(*runs: dict[str, object]) -> dict[str, object]:
    return {"version": "2.1.0", "runs": list(runs)}


def category_results(document: dict[str, object]) -> dict[str, list[dict[str, object]]]:
    return {
        item["automationDetails"]["id"]: item["results"]
        for item in document["runs"]
    }


class NormalizeCodacySarifTests(unittest.TestCase):
    def test_oversized_run_is_split_without_dropping_results(self) -> None:
        document = payload(run("cpp", [result(index) for index in range(7)]))
        count, run_count = normalizer.split_oversized_runs(document, max_results=3)

        self.assertEqual(count, 7)
        self.assertEqual(run_count, 3)
        self.assertEqual([len(item["results"]) for item in document["runs"]], [3, 3, 1])
        self.assertEqual(
            sorted(item["message"]["text"] for shard in document["runs"] for item in shard["results"]),
            [f"finding {index}" for index in range(7)],
        )

    def test_input_order_does_not_change_normalized_output_or_digests(self) -> None:
        forward = payload(
            run("c", [result(100, language="c")]),
            run("cpp", [result(index) for index in range(4)]),
        )
        reverse = payload(
            run("cpp", [result(index) for index in reversed(range(4))]),
            run("c", [result(100, language="c")]),
        )

        forward_audit = normalizer.normalize_for_github(forward, max_results=3)
        reverse_audit = normalizer.normalize_for_github(reverse, max_results=3)

        self.assertEqual(forward, reverse)
        self.assertEqual(forward_audit["input_digest"], reverse_audit["input_digest"])
        self.assertEqual(forward_audit["decision_digest"], reverse_audit["decision_digest"])

    def test_exact_github_boundary_is_partitioned(self) -> None:
        document = payload(
            run(
                "cpp",
                [result(index, with_location=False) for index in range(normalizer.MAX_RESULTS_PER_RUN + 1)],
            )
        )
        count, run_count = normalizer.split_oversized_runs(document)
        self.assertEqual(count, normalizer.MAX_RESULTS_PER_RUN + 1)
        self.assertEqual(run_count, 2)
        self.assertEqual(
            [len(item["results"]) for item in document["runs"]],
            [normalizer.MAX_RESULTS_PER_RUN, 1],
        )

    def test_only_low_levels_and_exact_config_rule_are_dropped(self) -> None:
        findings = [
            result(0, level="warning", rule_id="cppcheck_noValidConfiguration"),
            result(1, level="error", rule_id="actionable-error"),
            result(2, level="note", rule_id="ordinary-note"),
            result(3, level="none", rule_id="ordinary-none"),
            result(4, level="warning", rule_id="cppcheck_misra-config"),
            result(5, level="note", rule_id="cppcheck_misra-config"),
            result(6, level="warning", rule_id="cppcheck_misra-config-extra"),
        ]
        document = payload(run("c", []), run("cpp", findings))

        audit = normalizer.normalize_for_github(document)
        retained = [item for shard in document["runs"] for item in shard["results"]]

        self.assertEqual(audit["input_results"], 7)
        self.assertEqual(audit["output_results"], 3)
        self.assertEqual(audit["dropped_results"], 4)
        self.assertEqual(
            audit["dropped_reasons"],
            {"level:none": 1, "level:note": 1, "rule:cppcheck_misra-config": 2},
        )
        self.assertEqual(
            sorted(item["ruleId"] for item in retained),
            ["actionable-error", "cppcheck_misra-config-extra", "cppcheck_noValidConfiguration"],
        )
        self.assertEqual(audit["dropped_rules"]["cppcheck_misra-config"], 2)

    def test_exact_head_level_distribution_has_deterministic_audit(self) -> None:
        findings: list[dict[str, object]] = []
        index = 0
        for level, count in (("error", 5), ("none", 22_312), ("note", 3_314), ("warning", 1_271)):
            for _ in range(count):
                findings.append(result(index, level=level, with_location=False))
                index += 1
        findings[-1]["ruleId"] = "cppcheck_misra-config"
        document = payload(run("c", []), run("cpp", findings))

        audit = normalizer.normalize_for_github(document)

        self.assertEqual(audit["input_results"], 26_902)
        self.assertEqual(
            audit["input_levels"], {"error": 5, "none": 22_312, "note": 3_314, "warning": 1_271}
        )
        self.assertEqual(audit["output_results"], 1_275)
        self.assertEqual(audit["output_levels"], {"error": 5, "warning": 1_270})
        self.assertEqual(audit["dropped_results"], 25_627)
        self.assertEqual(
            audit["dropped_reasons"],
            {"level:none": 22_312, "level:note": 3_314, "rule:cppcheck_misra-config": 1},
        )
        self.assertEqual(audit["categories"], list(normalizer.CATEGORY_ROSTER))
        self.assertTrue(all(len(audit[key]) == 64 for key in (
            "input_digest", "output_digest", "dropped_digest", "decision_digest"
        )))

    def test_lower_volume_migration_keeps_full_category_roster(self) -> None:
        document = payload(
            run("c", [result(10, language="c")]),
            run("cpp", [result(20)]),
        )

        normalizer.normalize_for_github(document)
        by_category = category_results(document)

        self.assertEqual(list(by_category), list(normalizer.CATEGORY_ROSTER))
        self.assertEqual(len(by_category[normalizer.CATEGORY_ROSTER[0]]), 1)
        self.assertEqual(len(by_category[normalizer.CATEGORY_ROSTER[1]]), 1)
        self.assertEqual(by_category[normalizer.CATEGORY_ROSTER[2]], [])

    def test_missing_language_still_emits_empty_closure_category(self) -> None:
        document = payload(run("cpp", [result(1)]))

        normalizer.normalize_for_github(document)
        by_category = category_results(document)

        self.assertEqual(list(by_category), list(normalizer.CATEGORY_ROSTER))
        self.assertEqual(by_category[normalizer.CATEGORY_ROSTER[0]], [])

    def test_normalization_is_idempotent(self) -> None:
        document = payload(
            run("c", [result(10, language="c", level="note")]),
            run("cpp", [result(index) for index in range(4)]),
        )
        normalizer.normalize_for_github(document, max_results=3)
        first = copy.deepcopy(document)

        normalizer.normalize_for_github(document, max_results=3)

        self.assertEqual(document, first)

    def test_existing_roster_preserves_shard_identity_when_reordered(self) -> None:
        document = payload(
            run("c", [result(10, language="c")]),
            run("cpp", [result(index) for index in range(4)]),
        )
        normalizer.normalize_for_github(document, max_results=3)
        before = {
            category: [item["message"]["text"] for item in findings]
            for category, findings in category_results(document).items()
        }
        document["runs"].reverse()

        normalizer.normalize_for_github(document, max_results=3)
        after = {
            category: [item["message"]["text"] for item in findings]
            for category, findings in category_results(document).items()
        }

        self.assertEqual(after, before)
        self.assertEqual(list(category_results(document)), list(normalizer.CATEGORY_ROSTER))

    def test_more_shards_than_stable_roster_fails_closed(self) -> None:
        document = payload(run("cpp", [result(index) for index in range(7)]))
        with self.assertRaisesRegex(ValueError, "stable roster has 2"):
            normalizer.normalize_for_github(document, max_results=3)

    def test_invalid_results_array_is_rejected(self) -> None:
        document = payload(run("cpp", []))
        document["runs"][0]["results"] = {}
        with self.assertRaisesRegex(ValueError, "run.results"):
            normalizer.normalize_for_github(document)

    def test_non_object_result_is_rejected(self) -> None:
        document = payload(run("cpp", []))
        document["runs"][0]["results"] = ["not-an-object"]
        with self.assertRaisesRegex(ValueError, "entries must be objects"):
            normalizer.normalize_for_github(document)

    def test_missing_or_unknown_level_is_rejected(self) -> None:
        for value in (None, "critical", 1):
            with self.subTest(value=value):
                finding = result(0)
                if value is None:
                    finding.pop("level")
                else:
                    finding["level"] = value
                with self.assertRaisesRegex(ValueError, "result.level"):
                    normalizer.normalize_for_github(payload(run("cpp", [finding])))

    def test_missing_or_non_string_rule_id_is_rejected(self) -> None:
        for value in (None, "", 1):
            with self.subTest(value=value):
                finding = result(0)
                if value is None:
                    finding.pop("ruleId")
                else:
                    finding["ruleId"] = value
                with self.assertRaisesRegex(ValueError, "result.ruleId"):
                    normalizer.normalize_for_github(payload(run("cpp", [finding])))

    def test_malformed_nested_location_is_rejected(self) -> None:
        finding = result(0)
        finding["locations"][0]["physicalLocation"]["artifactLocation"] = []
        with self.assertRaisesRegex(ValueError, "artifactLocation must be an object"):
            normalizer.normalize_for_github(payload(run("cpp", [finding])))

    def test_mixed_or_drifted_categories_are_rejected(self) -> None:
        uncategorized = run("cpp", [result(0)])
        categorized = run("c", [result(1, language="c")])
        categorized["automationDetails"] = {"id": normalizer.CATEGORY_ROSTER[0]}
        with self.assertRaisesRegex(ValueError, "mixes categorized and uncategorized"):
            normalizer.normalize_for_github(payload(categorized, uncategorized))

        drifted = run("cpp", [result(2)])
        drifted["automationDetails"] = {"id": "codacy/unexpected/"}
        with self.assertRaisesRegex(ValueError, "category roster is unexpected"):
            normalizer.normalize_for_github(payload(drifted))

    def test_duplicate_json_keys_and_invalid_json_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            duplicate = Path(directory, "duplicate.sarif")
            duplicate.write_text('{"version":"2.1.0","runs":[],"runs":[]}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate JSON property"):
                normalizer._load_payload(duplicate)

            malformed = Path(directory, "malformed.sarif")
            malformed.write_text('{"version":', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "invalid SARIF JSON"):
                normalizer._load_payload(malformed)

    def test_cli_failure_does_not_rewrite_input(self) -> None:
        document = payload(run("cpp", [result(0)]))
        document["runs"][0]["results"][0].pop("level")
        original = json.dumps(document, separators=(",", ":"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, "invalid.sarif")
            path.write_text(original, encoding="utf-8")

            completed = subprocess.run(
                [sys.executable, "-I", str(SCRIPT), str(path)],
                capture_output=True,
                check=False,
                text=True,
            )

            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("result.level", completed.stderr)
            self.assertEqual(path.read_text(encoding="utf-8"), original)

    def test_cli_success_is_idempotent_and_writes_audit_summary(self) -> None:
        document = payload(
            run("c", []),
            run(
                "cpp",
                [result(0), result(1, level="note", rule_id="quiet-rule")],
            ),
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, "results.sarif")
            summary_path = Path(directory, "summary.md")
            path.write_text(json.dumps(document), encoding="utf-8")
            environment = dict(os.environ)
            environment["GITHUB_STEP_SUMMARY"] = str(summary_path)

            first = subprocess.run(
                [sys.executable, "-I", str(SCRIPT), str(path)],
                capture_output=True,
                check=False,
                env=environment,
                text=True,
            )
            first_output = path.read_bytes()
            second = subprocess.run(
                [sys.executable, "-I", str(SCRIPT), str(path)],
                capture_output=True,
                check=False,
                env=environment,
                text=True,
            )

            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertIn("level:note=1", first.stdout)
            self.assertIn("quiet-rule=1", first.stdout)
            self.assertEqual(path.read_bytes(), first_output)
            normalized = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(
                [item["automationDetails"]["id"] for item in normalized["runs"]],
                list(normalizer.CATEGORY_ROSTER),
            )
            summary_text = summary_path.read_text(encoding="utf-8")
            self.assertIn("#### Audit digests", summary_text)
            self.assertIn("quiet-rule", summary_text)

    def test_step_summary_lists_reasons_rules_categories_and_digests(self) -> None:
        document = payload(
            run("c", []),
            run("cpp", [result(0), result(1, level="note", rule_id="quiet-rule")]),
        )
        audit = normalizer.normalize_for_github(document)
        summary = normalizer._summary_line(audit)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, "summary.md")
            normalizer._write_step_summary(path, audit, summary)
            text = path.read_text(encoding="utf-8")

        self.assertIn("#### Audit digests", text)
        self.assertIn("#### Dropped by reason", text)
        self.assertIn("level:note", text)
        self.assertIn("#### Dropped by rule", text)
        self.assertIn("quiet-rule", text)
        for category in normalizer.CATEGORY_ROSTER:
            self.assertIn(category, text)


if __name__ == "__main__":
    unittest.main()
