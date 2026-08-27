#!/usr/bin/env python3
"""Unit contract for deterministic Codacy SARIF sharding and categories."""

from __future__ import annotations

import copy
import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).with_name("normalize-codacy-sarif.py")
SPEC = importlib.util.spec_from_file_location("normalize_codacy_sarif", SCRIPT)
assert SPEC and SPEC.loader
normalizer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(normalizer)


def result(index: int) -> dict[str, object]:
    return {
        "ruleId": f"rule-{index % 2}",
        "level": "warning" if index % 2 else "error",
        "message": {"text": f"finding {index}"},
        "locations": [
            {
                "physicalLocation": {
                    "artifactLocation": {"uri": f"src/file-{index}.cpp"},
                    "region": {"startLine": index + 1},
                }
            }
        ],
    }


def payload(results: list[dict[str, object]]) -> dict[str, object]:
    return {
        "version": "2.1.0",
        "runs": [
            {
                "tool": {"driver": {"name": "Cppcheck"}},
                "artifacts": [{"location": {"uri": "src/all.cpp"}}],
                "results": results,
            }
        ],
    }


class NormalizeCodacySarifTests(unittest.TestCase):
    def test_oversized_run_is_split_without_dropping_results(self) -> None:
        document = payload([result(index) for index in range(7)])
        count, run_count = normalizer.split_oversized_runs(document, max_results=3)
        categories = normalizer.normalize(document)

        self.assertEqual(count, 7)
        self.assertEqual(run_count, 3)
        self.assertEqual([len(run["results"]) for run in document["runs"]], [3, 3, 1])
        self.assertEqual(len(categories), 3)
        self.assertEqual(len(categories), len(set(categories)))
        self.assertEqual(
            sorted(item["message"]["text"] for run in document["runs"] for item in run["results"]),
            [f"finding {index}" for index in range(7)],
        )

    def test_input_order_does_not_change_normalized_output(self) -> None:
        forward = payload([result(index) for index in range(7)])
        reverse = payload([result(index) for index in reversed(range(7))])
        normalizer.split_oversized_runs(forward, max_results=3)
        normalizer.split_oversized_runs(reverse, max_results=3)
        normalizer.normalize(forward)
        normalizer.normalize(reverse)
        self.assertEqual(forward, reverse)

    def test_exact_github_boundary_is_partitioned(self) -> None:
        document = payload([result(index) for index in range(normalizer.MAX_RESULTS_PER_RUN + 1)])
        count, run_count = normalizer.split_oversized_runs(document)
        self.assertEqual(count, normalizer.MAX_RESULTS_PER_RUN + 1)
        self.assertEqual(run_count, 2)
        self.assertEqual(
            [len(run["results"]) for run in document["runs"]],
            [normalizer.MAX_RESULTS_PER_RUN, 1],
        )

    def test_normalization_is_idempotent(self) -> None:
        document = payload([result(index) for index in range(7)])
        normalizer.split_oversized_runs(document, max_results=3)
        normalizer.normalize(document)
        first = copy.deepcopy(document)
        normalizer.split_oversized_runs(document, max_results=3)
        normalizer.normalize(document)
        self.assertEqual(document, first)

    def test_existing_payload_is_not_mutated_on_copy(self) -> None:
        original = payload([result(0)])
        document = copy.deepcopy(original)
        normalizer.split_oversized_runs(document, max_results=3)
        self.assertEqual(original, payload([result(0)]))

    def test_invalid_results_shape_is_rejected(self) -> None:
        document = payload([])
        document["runs"][0]["results"] = {}
        with self.assertRaisesRegex(ValueError, "run.results"):
            normalizer.split_oversized_runs(document)


if __name__ == "__main__":
    unittest.main()
