#!/usr/bin/env python3
"""Tests for the REL-110 protected stable-release approval recorder and its release.yml wiring.

Registered as the CTest ReleaseApproval_Record.
"""
from __future__ import annotations

import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import urllib.error

import yaml

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

import record_release_approval as recorder  # noqa: E402

RELEASE_WORKFLOW = SCRIPT_DIR.parents[0] / "workflows" / "release.yml"
REPOSITORY = "Krilliac/SparkEngine"
RUN_ID = 777
ATTEMPT = 2
SHA = "a" * 40
OWNER = {"login": "Krilliac", "id": 4242}
ENVIRONMENT_ID = 9001
ROOT = "https://api.github.com"


class FakeResponse:
    def __init__(self, body, *, status=200, headers=None):
        self.status = status
        self._stream = io.BytesIO(body)
        self.headers = {"Content-Length": str(len(body)), **(headers or {})}

    def read(self, limit=-1):
        return self._stream.read(limit)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def approval(*, state="approved", user=None, environments=None, comment="ship v1"):
    return {
        "state": state,
        "comment": comment,
        "user": dict(user or OWNER),
        "environments": environments if environments is not None else [
            {"id": ENVIRONMENT_ID, "name": "stable-release", "url": "https://example.invalid"}],
    }


class Server:
    """Injected urlopen that serves raw bytes per exact URL and records requests."""

    def __init__(self, routes):
        self.routes = routes
        self.requests = []

    def __call__(self, request, timeout):
        self.requests.append(request)
        route = self.routes.get(request.full_url)
        if route is None:
            raise urllib.error.HTTPError(request.full_url, 404, "Not Found", {}, None)
        if isinstance(route, BaseException):
            raise route
        return route() if callable(route) else FakeResponse(route)


def encode(document):
    return json.dumps(document).encode("utf-8")


def default_routes(reviews):
    return {
        f"{ROOT}/repos/{REPOSITORY}": encode({"full_name": REPOSITORY, "owner": OWNER}),
        f"{ROOT}/repos/{REPOSITORY}/environments/stable-release": encode(
            {"id": ENVIRONMENT_ID, "name": "stable-release"}),
        f"{ROOT}/repos/{REPOSITORY}/actions/runs/{RUN_ID}/attempts/{ATTEMPT}": encode({
            "id": RUN_ID, "run_attempt": ATTEMPT, "head_sha": SHA, "path": ".github/workflows/release.yml",
            "repository": {"full_name": REPOSITORY}, "run_started_at": "2026-09-24T10:00:00Z"}),
        f"{ROOT}/repos/{REPOSITORY}/actions/runs/{RUN_ID}/approvals?per_page=100": encode(reviews),
    }


def collect(routes):
    server = Server(routes)
    api = recorder.GitHubApi("token", urlopen=server)
    return recorder.collect(api, repository=REPOSITORY, run_id=RUN_ID, run_attempt=ATTEMPT, source_commit=SHA), server


class RecorderTests(unittest.TestCase):
    def assert_refused(self, routes, message):
        with self.assertRaisesRegex(recorder.ApprovalError, message):
            collect(routes)

    def test_owner_approval_is_recorded_as_a_closed_deterministic_record(self):
        record, server = collect(default_routes([approval()]))
        self.assertEqual(set(record), {"schemaVersion", "kind", "repository", "sourceCommit", "workflow", "run",
                                       "environment", "approver", "approvals", "limitations"})
        self.assertEqual(record["run"], {"id": RUN_ID, "attempt": ATTEMPT, "startedAt": "2026-09-24T10:00:00Z"})
        self.assertEqual(record["environment"], {"name": "stable-release", "id": ENVIRONMENT_ID})
        self.assertEqual(record["approver"], OWNER)
        self.assertEqual(record["approvals"], [{"state": "approved", "login": "Krilliac", "id": 4242,
                                                "commentSha256": hashlib.sha256(b"ship v1").hexdigest()}])
        self.assertNotIn("ship v1", json.dumps(record))
        again, _ = collect(default_routes([approval()]))
        self.assertEqual(recorder.canonical_bytes(record), recorder.canonical_bytes(again))
        for request in server.requests:
            self.assertEqual(request.get_header("Authorization"), "Bearer token")
            self.assertEqual(request.get_header("X-github-api-version"), "2022-11-28")

    def test_each_protected_job_approval_by_the_owner_is_retained(self):
        record, _ = collect(default_routes([approval(comment="windows"), approval(comment="publish")]))
        self.assertEqual(len(record["approvals"]), 2)

    def test_non_owner_approval_is_refused(self):
        for user in ({"login": "mallory", "id": 5}, {"login": "Krilliac", "id": 5}, {"login": "krilliac", "id": 4242}):
            with self.subTest(user=user):
                self.assert_refused(default_routes([approval(), approval(user=user)]), "someone other than")

    def test_rejected_review_is_refused_even_after_an_owner_approval(self):
        self.assert_refused(default_routes([approval(), approval(state="rejected")]), "rejected")
        self.assert_refused(default_routes([approval(state="pending")]), "unexpected")

    def test_missing_approval_is_refused(self):
        self.assert_refused(default_routes([]), "no protected stable-release approval")

    def test_multiple_or_foreign_environments_are_refused(self):
        stable = {"id": ENVIRONMENT_ID, "name": "stable-release"}
        for environments in ([stable, {"id": 3, "name": "nightly-release"}], [],
                             [{"id": 3, "name": "nightly-release"}], [{"id": 3, "name": "stable-release"}]):
            with self.subTest(environments=environments):
                self.assert_refused(default_routes([approval(environments=environments)]), "environment")

    def test_wrong_run_attempt_commit_workflow_or_repository_is_refused(self):
        run_url = f"{ROOT}/repos/{REPOSITORY}/actions/runs/{RUN_ID}/attempts/{ATTEMPT}"
        run = json.loads(default_routes([])[run_url])
        for field, value, message in (("id", 778, "different workflow run"),
                                      ("run_attempt", 1, "different run attempt"),
                                      ("head_sha", "b" * 40, "head SHA"),
                                      ("path", ".github/workflows/build.yml", "release workflow"),
                                      ("repository", {"full_name": "other/repo"}, "another repository"),
                                      ("run_started_at", None, "start time")):
            with self.subTest(field=field):
                routes = default_routes([approval()])
                routes[run_url] = encode({**run, field: value})
                self.assert_refused(routes, message)

    def test_wrong_repository_owner_is_refused(self):
        routes = default_routes([approval()])
        routes[f"{ROOT}/repos/{REPOSITORY}"] = encode({"full_name": REPOSITORY, "owner": {"login": "x", "id": 1}})
        self.assert_refused(routes, "repository owner")

    def test_duplicate_json_keys_are_refused(self):
        routes = default_routes([])
        routes[f"{ROOT}/repos/{REPOSITORY}/actions/runs/{RUN_ID}/approvals?per_page=100"] = (
            b'[{"state": "rejected", "state": "approved", "comment": "", "user": {"login": "Krilliac", "id": 4242},'
            b' "environments": [{"id": 9001, "name": "stable-release"}]}]')
        self.assert_refused(routes, "duplicate")

    def test_truncated_body_or_oversized_body_is_refused(self):
        url = f"{ROOT}/repos/{REPOSITORY}/actions/runs/{RUN_ID}/approvals?per_page=100"
        body = encode([approval()])
        routes = default_routes([])
        routes[url] = lambda: FakeResponse(body[:-5], headers={"Content-Length": str(len(body))})
        self.assert_refused(routes, "truncated")
        routes[url] = b"[" + b" " * recorder.MAX_RESPONSE_BYTES + b"]"
        self.assert_refused(routes, "size limit")

    def test_pagination_is_followed_and_truncated_pagination_is_refused(self):
        url = f"{ROOT}/repos/{REPOSITORY}/actions/runs/{RUN_ID}/approvals"
        first = f"{url}?per_page=100"
        page = lambda number: f"{url}?per_page=100&page={number}"
        routes = default_routes([])
        routes[first] = lambda: FakeResponse(encode([approval()]), headers={"Link": f'<{page(2)}>; rel="next"'})
        routes[page(2)] = encode([approval(comment="second")])
        record, _ = collect(routes)
        self.assertEqual(len(record["approvals"]), 2)
        # The next page promised by GitHub is unreadable: the history is incomplete.
        del routes[page(2)]
        self.assert_refused(routes, "cannot read")
        # A next link that never ends exceeds the bounded page count.
        for number in range(2, recorder.MAX_PAGES + 2):
            routes[page(number)] = (lambda n: lambda: FakeResponse(
                encode([approval()]), headers={"Link": f'<{page(n + 1)}>; rel="next"'}))(number)
        self.assert_refused(routes, "bounded page count")
        # A next link that leaves the endpoint is never followed.
        routes[first] = lambda: FakeResponse(encode([approval()]),
                                             headers={"Link": '<https://evil.example/x?page=2>; rel="next"'})
        self.assert_refused(routes, "left the requested endpoint")

    def test_http_errors_and_non_200_status_are_refused(self):
        routes = default_routes([approval()])
        routes[f"{ROOT}/repos/{REPOSITORY}/actions/runs/{RUN_ID}/approvals?per_page=100"] = (
            urllib.error.HTTPError("x", 403, "Forbidden", {}, None))
        self.assert_refused(routes, "cannot read")
        routes[f"{ROOT}/repos/{REPOSITORY}/actions/runs/{RUN_ID}/approvals?per_page=100"] = (
            lambda: FakeResponse(b"[]", status=202))
        self.assert_refused(routes, "HTTP 202")

    def test_invalid_inputs_and_missing_token_are_refused(self):
        with self.assertRaisesRegex(recorder.ApprovalError, "GH_TOKEN"):
            recorder.GitHubApi("")
        with self.assertRaisesRegex(recorder.ApprovalError, "https origin"):
            recorder.GitHubApi("t", api_root="http://api.github.com")
        api = recorder.GitHubApi("t", urlopen=Server({}))
        for arguments in ({"repository": "bad repo"}, {"run_id": 0}, {"run_attempt": True}, {"source_commit": "abc"}):
            identity = {"repository": REPOSITORY, "run_id": RUN_ID, "run_attempt": ATTEMPT, "source_commit": SHA,
                        **arguments}
            with self.subTest(arguments=arguments), self.assertRaises(recorder.ApprovalError):
                recorder.collect(api, **identity)

    def test_cli_publishes_once_and_never_overwrites(self):
        server = Server(default_routes([approval()]))
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.dict(os.environ, {"GH_TOKEN": "token", "GITHUB_API_URL": ROOT}), \
                mock.patch.object(recorder.urllib.request, "urlopen", server):
            output = Path(temporary) / "approval.json"
            arguments = ["--repository", REPOSITORY, "--run-id", str(RUN_ID), "--run-attempt", str(ATTEMPT),
                         "--source-commit", SHA, "--output", str(output)]
            with mock.patch("sys.stdout", new=io.StringIO()):
                self.assertEqual(recorder.main(arguments), 0)
            payload = output.read_bytes()
            record = json.loads(payload)
            self.assertEqual(payload, recorder.canonical_bytes(record))
            with mock.patch("sys.stderr", new=io.StringIO()) as error:
                self.assertEqual(recorder.main(arguments), 1)
            self.assertIn("already exists", error.getvalue())
            self.assertEqual(output.read_bytes(), payload)

    def test_cli_failure_leaves_no_record(self):
        server = Server(default_routes([approval(state="rejected")]))
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.dict(os.environ, {"GH_TOKEN": "token"}), \
                mock.patch.object(recorder.urllib.request, "urlopen", server), \
                mock.patch("sys.stderr", new=io.StringIO()):
            output = Path(temporary) / "approval.json"
            self.assertEqual(recorder.main(["--repository", REPOSITORY, "--run-id", str(RUN_ID), "--run-attempt",
                                            str(ATTEMPT), "--source-commit", SHA, "--output", str(output)]), 1)
            self.assertFalse(output.exists())


class WorkflowWiringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.safe_load(RELEASE_WORKFLOW.read_text(encoding="utf-8"))

    def test_release_job_records_approval_right_after_the_environment_gate(self):
        release = self.workflow["jobs"]["release"]
        steps = release["steps"]
        names = [step.get("name") for step in steps]
        gate = names.index("Verify stable publication environment protection")
        record = steps[gate + 1]
        retain = steps[gate + 2]
        self.assertEqual(record["name"], "Record protected stable-release approval event")
        self.assertEqual(record["id"], "release-approval")
        self.assertEqual(record["if"], "needs.prepare.outputs.is_versioned == 'true'")
        self.assertEqual(record["env"]["GH_TOKEN"], "${{ github.token }}")
        self.assertIn("record_release_approval.py", record["run"])
        for argument in ("--run-id", "--run-attempt", "--source-commit", "--output", "GITHUB_OUTPUT"):
            self.assertIn(argument, record["run"])
        self.assertTrue(retain["uses"].startswith("actions/upload-artifact@"))
        self.assertEqual(retain["if"], "needs.prepare.outputs.is_versioned == 'true'")
        self.assertEqual(retain["with"]["if-no-files-found"], "error")
        self.assertGreaterEqual(retain["with"]["retention-days"], 90)
        self.assertEqual(release["permissions"]["actions"], "read")
        self.assertEqual(release["outputs"]["approval_record_sha256"],
                         "${{ steps.release-approval.outputs.sha256 }}")
        self.assertEqual(release["outputs"]["approval_run_attempt"],
                         "${{ steps.release-approval.outputs.run_attempt }}")
        first_mutation = next(index for index, step in enumerate(steps)
                              if "guard_release_mutation.py" in step.get("run", "") or "git push" in step.get("run", ""))
        self.assertLess(gate + 1, first_mutation)

    def test_consumer_requires_the_publisher_approval_digest(self):
        consumer = self.workflow["jobs"]["verify-stable-publication"]
        step = next(step for step in consumer["steps"]
                    if "verify_published_stable_release.py" in step.get("run", ""))
        self.assertEqual(step["env"]["APPROVAL_RECORD_SHA256"], "${{ needs.release.outputs.approval_record_sha256 }}")
        self.assertEqual(step["env"]["APPROVAL_RUN_ATTEMPT"], "${{ needs.release.outputs.approval_run_attempt }}")
        self.assertIn('--approval-record-sha256 "$APPROVAL_RECORD_SHA256"', step["run"])
        self.assertIn('--approval-run-attempt "$APPROVAL_RUN_ATTEMPT"', step["run"])
        self.assertEqual(step["env"]["GH_TOKEN"], "${{ github.token }}")
        self.assertEqual(consumer["permissions"]["actions"], "read")


if __name__ == "__main__":
    unittest.main()
