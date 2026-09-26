#!/usr/bin/env python3
"""Fixture and mutation tests for verify-working-ruleset.py (fixture mode only, no network)."""

from __future__ import annotations

import contextlib
import copy
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("verify-working-ruleset.py")
FIXTURE = Path(__file__).parent / "fixtures" / "working-ruleset-2026-09-24.json"
SPEC = importlib.util.spec_from_file_location("verify_working_ruleset", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def captured() -> dict:
    return json.loads(FIXTURE.read_text(encoding="utf-8"))


def branch_rule(document: dict, rule_type: str) -> dict:
    return next(rule for rule in document["rulesForBranch"] if rule["type"] == rule_type)


def ruleset_rule(document: dict, rule_type: str) -> dict:
    return next(rule for rule in document["ruleset"]["rules"] if rule["type"] == rule_type)


class VerifyWorkingRulesetTests(unittest.TestCase):
    def assert_rejected(self, document: dict, message: str) -> None:
        result = MODULE.verdict(document["rulesForBranch"], document["ruleset"], "test")
        self.assertFalse(result["ok"])
        self.assertTrue(any(message in error for error in result["errors"]), result["errors"])

    def test_captured_fixture_is_the_unedited_api_response_pair(self) -> None:
        document = captured()
        self.assertEqual(document["repository"], MODULE.REPOSITORY)
        self.assertEqual(document["ruleset"]["id"], MODULE.RULESET_ID)
        self.assertEqual(document["ruleset"]["name"], "Working integrity")
        self.assertEqual(
            document["ruleset"]["_links"]["self"]["href"],
            f"https://api.github.com/repos/{MODULE.REPOSITORY}/rulesets/{MODULE.RULESET_ID}",
        )

    def test_captured_ruleset_is_accepted(self) -> None:
        document = captured()
        result = MODULE.verdict(document["rulesForBranch"], document["ruleset"], "test")
        self.assertEqual(result["errors"], [])
        self.assertTrue(result["ok"])

    def test_fixture_mode_prints_verdict_and_exits_zero(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = MODULE.main(["--fixture", str(FIXTURE)])
        self.assertEqual(code, 0)
        self.assertTrue(json.loads(output.getvalue())["ok"])

    def test_fixture_mode_exits_nonzero_on_violation(self) -> None:
        document = captured()
        document["ruleset"]["enforcement"] = "evaluate"
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "mutated.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                code = MODULE.main(["--fixture", str(path)])
        self.assertEqual(code, 1)
        self.assertFalse(json.loads(output.getvalue())["ok"])

    def test_unreadable_fixture_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "broken.json"
            path.write_text("[not json", encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                code = MODULE.main(["--fixture", str(path)])
        self.assertEqual(code, 1)

    def test_missing_branch_rules_are_rejected(self) -> None:
        for rule_type in sorted(MODULE.REQUIRED_RULE_TYPES):
            with self.subTest(rule=rule_type):
                document = captured()
                document["rulesForBranch"].remove(branch_rule(document, rule_type))
                self.assert_rejected(document, f"does not supply the '{rule_type}' rule")

    def test_missing_ruleset_rules_are_rejected(self) -> None:
        for rule_type in sorted(MODULE.REQUIRED_RULE_TYPES):
            with self.subTest(rule=rule_type):
                document = captured()
                document["ruleset"]["rules"].remove(ruleset_rule(document, rule_type))
                self.assert_rejected(document, f"ruleset does not define the '{rule_type}' rule")

    def test_wrong_required_check_is_rejected(self) -> None:
        mutations = (
            ("context", "Build SparkEngine"),
            ("integration_id", 1),
            ("integration_id", None),
        )
        for field, value in mutations:
            for where in ("branch", "ruleset"):
                with self.subTest(field=field, value=value, where=where):
                    document = captured()
                    rule = (
                        branch_rule(document, "required_status_checks")
                        if where == "branch"
                        else ruleset_rule(document, "required_status_checks")
                    )
                    rule["parameters"]["required_status_checks"][0][field] = value
                    self.assert_rejected(document, f"{where} required_status_checks must require exactly")

    def test_extra_or_missing_required_checks_are_rejected(self) -> None:
        document = captured()
        branch_rule(document, "required_status_checks")["parameters"]["required_status_checks"].append(
            {"context": "check-format", "integration_id": MODULE.GITHUB_ACTIONS_INTEGRATION_ID}
        )
        self.assert_rejected(document, "branch required_status_checks must require exactly")
        document = captured()
        ruleset_rule(document, "required_status_checks")["parameters"]["required_status_checks"] = []
        self.assert_rejected(document, "ruleset required_status_checks must require exactly")

    def test_rules_from_a_different_ruleset_or_source_are_rejected(self) -> None:
        document = captured()
        for rule in document["rulesForBranch"]:
            rule["ruleset_id"] = 1
        self.assert_rejected(document, f"ruleset {MODULE.RULESET_ID} does not supply")
        document = captured()
        branch_rule(document, "deletion")["ruleset_source"] = "someone/else"
        self.assert_rejected(document, "has an unexpected source")
        document = captured()
        branch_rule(document, "deletion")["ruleset_source_type"] = "Organization"
        self.assert_rejected(document, "has an unexpected source")

    def test_second_ruleset_adding_required_checks_is_rejected(self) -> None:
        document = captured()
        extra = copy.deepcopy(branch_rule(document, "required_status_checks"))
        extra["ruleset_id"] = 999
        document["rulesForBranch"].append(extra)
        self.assert_rejected(document, "another ruleset (999) adds required status checks")

    def test_ruleset_definition_weakening_is_rejected(self) -> None:
        cases = (
            (lambda ruleset: ruleset.update({"enforcement": "disabled"}), "enforcement is 'disabled'"),
            (lambda ruleset: ruleset.update({"enforcement": "evaluate"}), "enforcement is 'evaluate'"),
            (
                lambda ruleset: ruleset.update(
                    {"bypass_actors": [{"actor_id": 5, "actor_type": "RepositoryRole", "bypass_mode": "always"}]}
                ),
                "declares bypass actors",
            ),
            (lambda ruleset: ruleset.update({"id": 1}), "ruleset id is 1"),
            (lambda ruleset: ruleset.update({"source": "someone/else"}), "not owned by the repository"),
            (lambda ruleset: ruleset.update({"target": "tag"}), "does not target branches"),
            (
                lambda ruleset: ruleset["conditions"]["ref_name"].update({"include": ["refs/heads/main"]}),
                "does not include refs/heads/Working",
            ),
            (
                lambda ruleset: ruleset["conditions"]["ref_name"].update({"exclude": ["refs/heads/Working"]}),
                "ruleset excludes refs",
            ),
        )
        for change, message in cases:
            with self.subTest(message=message):
                document = captured()
                change(document["ruleset"])
                self.assert_rejected(document, message)

    def test_malformed_responses_are_rejected(self) -> None:
        self.assertFalse(MODULE.verdict({}, captured()["ruleset"], "test")["ok"])
        self.assertFalse(MODULE.verdict(captured()["rulesForBranch"], [], "test")["ok"])
        self.assertFalse(MODULE.verdict(["deletion"], captured()["ruleset"], "test")["ok"])


if __name__ == "__main__":
    unittest.main()
