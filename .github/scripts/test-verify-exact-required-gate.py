#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("verify-exact-required-gate.py")
SPEC = importlib.util.spec_from_file_location("verify_exact_required_gate", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

SHA = "a" * 40
REPOSITORY = "Krilliac/SparkEngine"


def run(run_id: int = 42, **overrides):
    value = {
        "id": run_id,
        "head_sha": SHA,
        "event": "push",
        "status": "completed",
        "conclusion": "success",
        "html_url": f"https://example.invalid/runs/{run_id}",
    }
    value.update(overrides)
    return value


def gate(**overrides):
    value = {"name": "Required CI Gate", "status": "completed", "conclusion": "success"}
    value.update(overrides)
    return value


class FakeApi:
    def __init__(self, runs, jobs_by_run):
        self.runs = runs
        self.jobs_by_run = jobs_by_run

    def __call__(self, path):
        if "/workflows/build.yml/runs?" in path:
            return {"workflow_runs": self.runs}
        run_id = int(path.split("/runs/", 1)[1].split("/", 1)[0])
        return {"jobs": self.jobs_by_run[run_id]}


class VerifyExactRequiredGateTests(unittest.TestCase):
    def test_accepts_exact_successful_push_gate(self):
        evidence = MODULE.verify_exact_gate(FakeApi([run()], {42: [gate()]}), REPOSITORY, SHA)
        self.assertEqual(evidence.run_id, 42)
        self.assertEqual(evidence.event, "push")

    def test_accepts_workflow_dispatch(self):
        evidence = MODULE.verify_exact_gate(
            FakeApi([run(event="workflow_dispatch")], {42: [gate()]}), REPOSITORY, SHA
        )
        self.assertEqual(evidence.event, "workflow_dispatch")

    def test_rejects_pull_request_and_different_sha(self):
        api = FakeApi(
            [run(event="pull_request"), run(43, head_sha="b" * 40)],
            {},
        )
        with self.assertRaisesRegex(ValueError, "no completed successful"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_missing_failed_skipped_or_cancelled_gate(self):
        for job in (
            {"name": "some other job", "status": "completed", "conclusion": "success"},
            gate(conclusion="failure"),
            gate(conclusion="skipped"),
            gate(status="queued", conclusion=None),
        ):
            with self.subTest(job=job):
                with self.assertRaisesRegex(ValueError, "no completed successful"):
                    MODULE.verify_exact_gate(FakeApi([run()], {42: [job]}), REPOSITORY, SHA)

    def test_rejects_unsuccessful_workflow_even_if_job_payload_would_pass(self):
        api = FakeApi([run(conclusion="cancelled")], {42: [gate()]})
        with self.assertRaisesRegex(ValueError, "no completed successful"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_fails_closed_on_api_error(self):
        def broken(_path):
            raise RuntimeError("network unavailable")

        with self.assertRaisesRegex(RuntimeError, "network unavailable"):
            MODULE.verify_exact_gate(broken, REPOSITORY, SHA)

    def test_rejects_malformed_input_and_payload(self):
        with self.assertRaisesRegex(ValueError, "owner/name"):
            MODULE.verify_exact_gate(FakeApi([], {}), "bad", SHA)
        with self.assertRaisesRegex(ValueError, "40-character"):
            MODULE.verify_exact_gate(FakeApi([], {}), REPOSITORY, "abc")
        with self.assertRaisesRegex(ValueError, "workflow_runs"):
            MODULE.verify_exact_gate(lambda _path: {}, REPOSITORY, SHA)


if __name__ == "__main__":
    unittest.main()
