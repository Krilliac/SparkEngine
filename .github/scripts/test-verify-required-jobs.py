#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("verify-required-jobs.py")
SPEC = importlib.util.spec_from_file_location("verify_required_jobs", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

GATE_SHA = "0123456789abcdef0123456789abcdef01234567"
RUN_ENVIRONMENT = {
    "GITHUB_SHA": GATE_SHA,
    "GITHUB_RUN_ID": "987654321",
    "GITHUB_RUN_ATTEMPT": "2",
    "GITHUB_REPOSITORY": "example/SparkEngine",
    "GITHUB_EVENT_NAME": "push",
    "GITHUB_REF": "refs/heads/Working",
}


def run_verifier(
    needs_json: str,
    expected_jobs_json: str,
    *,
    json_out: Path | None = None,
    deferred_json: str = "{}",
    overrides: dict[str, str | None] | None = None,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment.update(RUN_ENVIRONMENT)
    environment.update(
        {
            "NEEDS_JSON": needs_json,
            "EXPECTED_REQUIRED_JOBS_JSON": expected_jobs_json,
            "DEFERRED_REQUIRED_FAILURES_JSON": deferred_json,
        }
    )
    environment.pop("GITHUB_STEP_SUMMARY", None)
    for key, value in (overrides or {}).items():
        if value is None:
            environment.pop(key, None)
        else:
            environment[key] = value
    command = [sys.executable, str(SCRIPT)]
    if json_out is not None:
        command.extend(["--json-out", str(json_out)])
    return subprocess.run(
        command,
        cwd=SCRIPT.parents[2],
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )


class RequiredGateRecordTests(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory()
        self.addCleanup(self._directory.cleanup)
        self.record_path = Path(self._directory.name) / "gate" / "required-ci-gate.json"

    def read_record(self) -> dict:
        text = self.record_path.read_text(encoding="utf-8")
        record = json.loads(text)
        self.assertEqual(text, json.dumps(record, indent=2, sort_keys=True) + "\n")
        return record

    def assert_identity(self, record: dict) -> None:
        self.assertEqual(record["schema"], MODULE.REQUIRED_GATE_RECORD_SCHEMA)
        self.assertEqual(record["sha"], GATE_SHA)
        self.assertEqual(record["run_id"], 987654321)
        self.assertEqual(record["run_attempt"], 2)
        self.assertEqual(record["repository"], "example/SparkEngine")
        self.assertEqual(record["event"], "push")
        self.assertEqual(record["ref"], "refs/heads/Working")

    def test_passing_gate_writes_pass_record_and_exits_zero(self) -> None:
        result = run_verifier(
            '{"tests":{"result":"success"},"build":{"result":"success"}}',
            '["build","tests"]',
            json_out=self.record_path,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        record = self.read_record()
        self.assert_identity(record)
        self.assertEqual(record["verdict"], "pass")
        self.assertEqual(record["expected_jobs"], ["build", "tests"])
        self.assertEqual(
            record["jobs"],
            [
                {"job": "build", "result": "success", "status": "success"},
                {"job": "tests", "result": "success", "status": "success"},
            ],
        )
        self.assertEqual(record["deferred"], [])
        self.assertEqual(record["failed"], [])
        self.assertEqual(record["counts"], {"success": 2, "deferred": 0, "failed": 0})

    def test_failed_skipped_and_missing_results_write_fail_record_and_exit_one(self) -> None:
        result = run_verifier(
            '{"ok":{"result":"success"},"bad":{"result":"failure"},'
            '"skipped":{"result":"skipped"},"absent":{}}',
            '["ok","bad","skipped","absent"]',
            json_out=self.record_path,
        )
        self.assertEqual(result.returncode, 1)
        record = self.read_record()
        self.assert_identity(record)
        self.assertEqual(record["verdict"], "fail")
        self.assertEqual(record["expected_jobs"], ["ok", "bad", "skipped", "absent"])
        self.assertEqual(
            record["jobs"],
            [
                {"job": "absent", "result": None, "status": "failed"},
                {"job": "bad", "result": "failure", "status": "failed"},
                {"job": "ok", "result": "success", "status": "success"},
                {"job": "skipped", "result": "skipped", "status": "failed"},
            ],
        )
        self.assertEqual(
            record["failed"],
            [
                {"job": "absent", "reason": "missing"},
                {"job": "bad", "reason": "failure"},
                {"job": "skipped", "reason": "skipped"},
            ],
        )
        self.assertEqual(record["counts"], {"success": 1, "deferred": 0, "failed": 3})

    def test_deferred_failure_is_recorded_separately(self) -> None:
        result = run_verifier(
            '{"ordinary":{"result":"success"},"build-windows-shipping":{"result":"failure"}}',
            '["ordinary","build-windows-shipping"]',
            json_out=self.record_path,
            deferred_json='{"build-windows-shipping":"failure"}',
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        record = self.read_record()
        self.assertEqual(record["verdict"], "pass")
        self.assertEqual(record["deferred"], [{"job": "build-windows-shipping", "result": "failure"}])
        self.assertIn(
            {"job": "build-windows-shipping", "result": "failure", "status": "deferred"},
            record["jobs"],
        )

    def test_invalid_evidence_writes_no_record_and_exits_two(self) -> None:
        cases = {
            "malformed needs": ("{not json", '["build"]'),
            "duplicate needs key": ('{"build":{"result":"failure"},"build":{"result":"success"}}', '["build"]'),
            "empty needs": ("{}", '["build"]'),
            "missing required job": ('{"build":{"result":"success"}}', '["build","tests"]'),
            "non-object job metadata": ('{"build":"success"}', '["build"]'),
        }
        for label, (needs_json, expected_json) in cases.items():
            with self.subTest(label=label):
                # A stale record from an earlier invocation must be removed too.
                self.record_path.parent.mkdir(parents=True, exist_ok=True)
                self.record_path.write_text('{"verdict":"pass"}\n', encoding="utf-8")
                result = run_verifier(needs_json, expected_json, json_out=self.record_path)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("invalid required-job evidence", result.stderr)
                self.assertFalse(self.record_path.exists())
                self.assertEqual(list(self.record_path.parent.iterdir()), [])

    def test_record_requires_exact_run_identity(self) -> None:
        cases = {
            "missing sha": {"GITHUB_SHA": None},
            "short sha": {"GITHUB_SHA": GATE_SHA[:12]},
            "uppercase sha": {"GITHUB_SHA": GATE_SHA.upper()},
            "missing run id": {"GITHUB_RUN_ID": None},
            "zero run attempt": {"GITHUB_RUN_ATTEMPT": "0"},
            "non-numeric run id": {"GITHUB_RUN_ID": "12a"},
            "missing event": {"GITHUB_EVENT_NAME": ""},
            "missing ref": {"GITHUB_REF": None},
            "missing repository": {"GITHUB_REPOSITORY": None},
        }
        for label, overrides in cases.items():
            with self.subTest(label=label):
                result = run_verifier(
                    '{"build":{"result":"success"}}',
                    '["build"]',
                    json_out=self.record_path,
                    overrides=overrides,
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertFalse(self.record_path.exists())

    def test_identity_is_only_required_when_a_record_is_requested(self) -> None:
        result = run_verifier(
            '{"build":{"result":"success"}}',
            '["build"]',
            overrides={"GITHUB_SHA": None, "GITHUB_RUN_ID": None},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.record_path.exists())


class VerifyRequiredJobsTests(unittest.TestCase):
    def test_accepts_only_successful_jobs(self) -> None:
        passed, failed = MODULE.verify(
            {"build": {"result": "success"}, "tests": {"result": "success"}}
        )
        self.assertEqual(passed, ["build", "tests"])
        self.assertEqual(failed, [])

    def test_reports_failure_cancelled_and_skipped(self) -> None:
        passed, failed = MODULE.verify(
            {
                "ok": {"result": "success"},
                "bad": {"result": "failure"},
                "cancelled": {"result": "cancelled"},
                "skipped": {"result": "skipped"},
            }
        )
        self.assertEqual(passed, ["ok"])
        self.assertEqual(
            failed,
            [("bad", "failure"), ("cancelled", "cancelled"), ("skipped", "skipped")],
        )

    def test_accepts_only_the_exact_deferred_failure(self) -> None:
        passed, deferred, failed = MODULE.verify_with_policy(
            {
                "ordinary": {"result": "success"},
                "build-windows-shipping": {"result": "failure"},
            },
            deferred_failures={"build-windows-shipping": "failure"},
            expected_jobs=["ordinary", "build-windows-shipping"],
        )
        self.assertEqual(passed, ["ordinary"])
        self.assertEqual(deferred, [("build-windows-shipping", "failure")])
        self.assertEqual(failed, [])

    def test_deferred_job_must_not_turn_green_skip_or_cancel(self) -> None:
        for result in ("success", "skipped", "cancelled", None):
            with self.subTest(result=result):
                passed, deferred, failed = MODULE.verify_with_policy(
                    {"build-windows-shipping": {"result": result}},
                    deferred_failures={"build-windows-shipping": "failure"},
                    expected_jobs=["build-windows-shipping"],
                )
                self.assertEqual(passed, [])
                self.assertEqual(deferred, [])
                self.assertEqual(len(failed), 1)
                self.assertIn("expected failure", failed[0][1])

    def test_required_job_inventory_is_exact(self) -> None:
        with self.assertRaisesRegex(ValueError, "inventory mismatch"):
            MODULE.verify_with_policy(
                {"one": {"result": "success"}, "extra": {"result": "success"}},
                expected_jobs=["one", "missing"],
            )

    def test_deferred_policy_is_narrow_and_present(self) -> None:
        with self.assertRaisesRegex(ValueError, "map exact job names"):
            MODULE.verify_with_policy(
                {"one": {"result": "failure"}},
                deferred_failures={"one": "success"},
            )
        with self.assertRaisesRegex(ValueError, "absent from needs"):
            MODULE.verify_with_policy(
                {"one": {"result": "success"}},
                deferred_failures={"missing": "failure"},
            )

    def test_rejects_empty_needs(self) -> None:
        with self.assertRaisesRegex(ValueError, "non-empty"):
            MODULE.verify({})

    def test_main_rejects_missing_expected_job_inventory(self) -> None:
        for expected_jobs_raw in (None, ""):
            with self.subTest(expected_jobs_raw=expected_jobs_raw):
                environment = os.environ.copy()
                environment["NEEDS_JSON"] = '{"build":{"result":"success"}}'
                environment["DEFERRED_REQUIRED_FAILURES_JSON"] = "{}"
                if expected_jobs_raw is None:
                    environment.pop("EXPECTED_REQUIRED_JOBS_JSON", None)
                else:
                    environment["EXPECTED_REQUIRED_JOBS_JSON"] = expected_jobs_raw
                environment.pop("GITHUB_STEP_SUMMARY", None)
                result = subprocess.run(
                    [sys.executable, str(SCRIPT)],
                    cwd=SCRIPT.parents[2],
                    env=environment,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn("EXPECTED_REQUIRED_JOBS_JSON is required", result.stderr)
                self.assertNotIn("All 1 required jobs succeeded", result.stdout)

    def test_rejects_duplicate_job_keys_in_raw_needs_evidence(self) -> None:
        environment = os.environ.copy()
        environment.update(
            {
                "NEEDS_JSON": '{"build":{"result":"failure"},"build":{"result":"success"}}',
                "EXPECTED_REQUIRED_JOBS_JSON": '["build"]',
                "DEFERRED_REQUIRED_FAILURES_JSON": "{}",
            }
        )
        environment.pop("GITHUB_STEP_SUMMARY", None)
        result = subprocess.run(
            [sys.executable, str(SCRIPT)],
            cwd=SCRIPT.parents[2],
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("duplicate", result.stderr)
        self.assertNotIn("All 1 required jobs succeeded", result.stdout)

    def test_markdown_distinguishes_failure(self) -> None:
        report = MODULE.markdown(["ok"], [("bad", "failure")])
        self.assertIn("did not succeed", report)
        self.assertIn("**failure**", report)

    def test_markdown_names_the_external_deferred_gate(self) -> None:
        report = MODULE.markdown(
            ["ordinary"], [], [("build-windows-shipping", "failure")]
        )
        self.assertIn("protected external status", report)
        self.assertIn("deferred to exact external gate", report)


if __name__ == "__main__":
    unittest.main()
