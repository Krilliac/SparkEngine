#!/usr/bin/env python3
"""Regression tests for the trusted direct Codacy SARIF validator."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name("validate-codacy-sarif.py")
SPEC = importlib.util.spec_from_file_location("validate_codacy_sarif", SCRIPT)
assert SPEC and SPEC.loader
validator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validator)


def payload() -> dict[str, object]:
    return {
        "version": "2.1.0",
        "runs": [
            {
                "tool": {"driver": {"name": validator.EXPECTED_TOOL_NAME}},
                "automationDetails": {"id": category},
                "results": [
                    {
                        "ruleId": f"cppcheck_test_{index}",
                        "level": "warning",
                        "message": {"text": "finding"},
                    }
                ],
            }
            for index, category in enumerate(validator.EXPECTED_CATEGORIES)
        ],
    }


def write_artifact(root: Path, document: object) -> str:
    artifact = root / validator.ARTIFACT_NAME
    artifact.write_text(json.dumps(document), encoding="utf-8")
    return f"sha256:{hashlib.sha256(artifact.read_bytes()).hexdigest()}"


class ValidateCodacySarifTests(unittest.TestCase):
    def test_rejects_results_the_normalizer_must_have_dropped(self) -> None:
        for rule_id, message, uri in (
            ("cppcheck_y2038-unsafe-call", "time is Y2038-unsafe", "SparkEngine/Source/Core/Clock.cpp"),
            ("cppcheck_misra-config", "finding", "SparkEngine/Source/Core/Clock.cpp"),
            ("cppcheck_syntaxError", "Code 'namespaceSpark{' is invalid C code.", "SparkEngine/Source/Core/Engine.h"),
        ):
            document = payload()
            document["runs"][1]["results"][0] = {
                "ruleId": rule_id,
                "level": "warning",
                "message": {"text": message},
                "locations": [{"physicalLocation": {"artifactLocation": {"uri": uri}}}],
            }
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                digest = write_artifact(root, document)
                with self.assertRaises(ValueError, msg=rule_id):
                    validator.validate_artifact(root, digest)

    def test_accepts_syntax_errors_that_are_not_header_language_guesses(self) -> None:
        document = payload()
        document["runs"][1]["results"][0] = {
            "ruleId": "cppcheck_syntaxError",
            "level": "warning",
            "message": {"text": "Code 'namespaceSpark{' is invalid C code."},
            "locations": [{"physicalLocation": {"artifactLocation": {"uri": "SparkLauncher/src/main.cpp"}}}],
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            digest = write_artifact(root, document)
            summary = validator.validate_artifact(root, digest)
        self.assertEqual(summary["results"], 3)

    def test_accepts_exact_direct_tool_and_category_roster(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            digest = write_artifact(root, payload())

            summary = validator.validate_artifact(root, digest)

        self.assertEqual(summary["runs"], 3)
        self.assertEqual(summary["results"], 3)

    def test_rejects_digest_tool_category_and_result_drift(self) -> None:
        mutations = {
            "digest": lambda document: None,
            "tool": lambda document: document["runs"][0]["tool"]["driver"].update(
                {"name": "Cppcheck"}
            ),
            "category": lambda document: document["runs"][1]["automationDetails"].update(
                {"id": "codacy/unexpected/"}
            ),
            "level": lambda document: document["runs"][2]["results"][0].update(
                {"level": "note"}
            ),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory() as directory:
                document = payload()
                mutate(document)
                root = Path(directory)
                digest = write_artifact(root, document)
                if label == "digest":
                    digest = f"sha256:{'f' * 64}"
                with self.assertRaises(ValueError):
                    validator.validate_artifact(root, digest)

    def test_rejects_duplicate_properties_and_extra_entries(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact = root / validator.ARTIFACT_NAME
            artifact.write_text(
                '{"version":"2.1.0","runs":[],"runs":[]}', encoding="utf-8"
            )
            digest = f"sha256:{hashlib.sha256(artifact.read_bytes()).hexdigest()}"
            with self.assertRaisesRegex(ValueError, "duplicate JSON property"):
                validator.validate_artifact(root, digest)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            digest = write_artifact(root, payload())
            (root / "unexpected.txt").write_text("hostile", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "contain only"):
                validator.validate_artifact(root, digest)

    def test_pull_and_default_branch_upload_provenance_are_exact(self) -> None:
        base = {
            "EXPECTED_DEFAULT_BRANCH": "Working",
            "EXPECTED_SOURCE_SHA": "1" * 40,
            "EXPECTED_UPLOAD_SHA": "2" * 40,
        }
        with mock.patch.dict(
            os.environ,
            {
                **base,
                "EXPECTED_SOURCE_EVENT": "pull_request",
                "EXPECTED_PR_NUMBER": "42",
                "EXPECTED_UPLOAD_REF": "refs/pull/42/head",
                "EXPECTED_UPLOAD_SHA": "1" * 40,
            },
            clear=True,
        ):
            validator._validate_upload_provenance()

        with mock.patch.dict(
            os.environ,
            {
                **base,
                "EXPECTED_SOURCE_EVENT": "push",
                "EXPECTED_PR_NUMBER": "",
                "EXPECTED_UPLOAD_REF": "refs/heads/Working",
                "EXPECTED_UPLOAD_SHA": "1" * 40,
            },
            clear=True,
        ):
            validator._validate_upload_provenance()

        with mock.patch.dict(
            os.environ,
            {
                **base,
                "EXPECTED_SOURCE_EVENT": "pull_request",
                "EXPECTED_PR_NUMBER": "42",
                "EXPECTED_UPLOAD_REF": "refs/pull/41/head",
                "EXPECTED_UPLOAD_SHA": "1" * 40,
            },
            clear=True,
        ):
            with self.assertRaisesRegex(ValueError, "exact head ref"):
                validator._validate_upload_provenance()


if __name__ == "__main__":
    unittest.main()
