"""In-memory API contract tests for verify-publication-evidence.py."""
from __future__ import annotations

import copy
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import unittest
from unittest.mock import patch

MODULE_PATH = Path(__file__).with_name("verify-publication-evidence.py")
SPEC = importlib.util.spec_from_file_location("verify_publication_evidence", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)

SOURCE = "a" * 40
PUBLICATION = "b" * 40


class Response:
    def __init__(self, payload):
        self.payload = payload

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def read(self, _limit):
        return json.dumps(self.payload).encode("utf-8")


def contract(reference: str | None = None) -> dict:
    reference = reference or f"ci:release.yml/42@{PUBLICATION}"
    return {
        "readiness": {"releaseProfiles": [{"publicationFinalization": {"workItemIds": ["REL-200"]}}]},
        "workItems": [{"id": "REL-200", "acceptanceStatus": [{"state": "evidenced", "evidence": [reference]}]}],
    }


def payloads() -> dict[str, dict]:
    job = {
        "id": 700, "name": "verify-stable-publication", "run_id": 42, "run_attempt": 1,
        "status": "completed", "conclusion": "success", "head_sha": PUBLICATION,
    }
    return {
        "/repos/Krilliac/SparkEngine/actions/runs/42": {
            "id": 42, "status": "completed", "conclusion": "success", "head_sha": PUBLICATION,
            "run_attempt": 1, "path": ".github/workflows/release.yml",
        },
        "/repos/Krilliac/SparkEngine/actions/runs/42/attempts/1/jobs?per_page=100&page=1": {
            "total_count": 1, "jobs": [job],
        },
        "/repos/Krilliac/SparkEngine/releases/latest": {
            "tag_name": "v1.0.0", "draft": False, "prerelease": False, "immutable": True,
        },
        "/repos/Krilliac/SparkEngine/git/ref/tags/v1.0.0": {
            "ref": "refs/tags/v1.0.0", "object": {"type": "commit", "sha": PUBLICATION},
        },
        f"/repos/Krilliac/SparkEngine/compare/{PUBLICATION}...{SOURCE}": {
            "status": "ahead", "head_commit": {"sha": SOURCE}, "merge_base_commit": {"sha": PUBLICATION},
        },
    }


class PublicationEvidenceTests(unittest.TestCase):
    def api(self, changes: dict[str, dict] | None = None):
        routes = payloads()
        for path, mutation in (changes or {}).items():
            routes[path] = {**routes[path], **mutation}

        def opener(request, timeout):
            self.assertEqual(timeout, 30)
            path = request.full_url.removeprefix("https://api.github.test")
            if path not in routes:
                raise AssertionError(f"unexpected API path: {path}")
            return Response(copy.deepcopy(routes[path]))

        return MODULE.GithubApi("Krilliac/SparkEngine", "token", opener=opener, api_root="https://api.github.test")

    def test_valid_descendant_publication_is_accepted(self):
        MODULE.verify(contract(), SOURCE, self.api())

    def test_publication_sha_must_be_ancestor_of_source(self):
        route = f"/repos/Krilliac/SparkEngine/compare/{PUBLICATION}...{SOURCE}"
        changes = {route: {"status": "behind", "head_commit": {"sha": SOURCE}, "merge_base_commit": {"sha": "c" * 40}}}
        with self.assertRaises(MODULE.PublicationEvidenceError):
            MODULE.verify(contract(), SOURCE, self.api(changes))

    def test_wrong_run_workflow_or_sha_is_rejected(self):
        route = "/repos/Krilliac/SparkEngine/actions/runs/42"
        for mutation, fragment in (({"path": ".github/workflows/build.yml"}, "identity"), ({"head_sha": "c" * 40}, "cited SHA"), ({"id": 99}, "run")):
            with self.subTest(mutation=mutation), self.assertRaisesRegex(MODULE.PublicationEvidenceError, fragment):
                MODULE.verify(contract(), SOURCE, self.api({route: mutation}))

    def test_missing_failed_skipped_and_duplicate_jobs_are_rejected(self):
        route = "/repos/Krilliac/SparkEngine/actions/runs/42/attempts/1/jobs?per_page=100&page=1"
        valid = payloads()[route]["jobs"][0]
        for jobs in ([], [{**valid, "conclusion": "failure"}], [{**valid, "conclusion": "skipped"}],
                     [{**valid, "status": "in_progress"}], [{**valid, "name": "other"}],
                     [valid, valid], [valid, {**valid, "id": 701}]):
            with self.subTest(jobs=jobs), self.assertRaises(MODULE.PublicationEvidenceError):
                MODULE.verify(contract(), SOURCE, self.api({route: {"total_count": len(jobs), "jobs": jobs}}))

    def test_paginated_jobs_must_be_complete(self):
        route = "/repos/Krilliac/SparkEngine/actions/runs/42/attempts/1/jobs?per_page=100&page=1"
        other = {**payloads()[route]["jobs"][0], "id": 800, "name": "other"}
        first = {"total_count": 101, "jobs": [{**other, "id": 800 + index} for index in range(100)]}
        second = {"total_count": 101, "jobs": [payloads()[route]["jobs"][0]]}
        routes = payloads()

        def opener(request, timeout):
            path = request.full_url.removeprefix("https://api.github.test")
            if path == route:
                return Response(first)
            if path.endswith("/jobs?per_page=100&page=2"):
                return Response(second)
            return Response(routes[path])

        api = MODULE.GithubApi("Krilliac/SparkEngine", "token", opener=opener, api_root="https://api.github.test")
        MODULE.verify(contract(), SOURCE, api)
        second["jobs"] = []
        with self.assertRaisesRegex(MODULE.PublicationEvidenceError, "pagination"):
            MODULE.verify(contract(), SOURCE, api)

    def test_attempt_endpoint_does_not_require_undocumented_job_field(self):
        routes = payloads()
        route = "/repos/Krilliac/SparkEngine/actions/runs/42/attempts/1/jobs?per_page=100&page=1"
        routes[route]["jobs"][0].pop("run_attempt")
        api = MODULE.GithubApi("Krilliac/SparkEngine", "token", api_root="https://api.github.test",
                               opener=lambda request, timeout: Response(routes[request.full_url.removeprefix("https://api.github.test")]))
        MODULE.verify(contract(), SOURCE, api)
        routes[route]["jobs"][0]["run_attempt"] = 2
        with self.assertRaisesRegex(MODULE.PublicationEvidenceError, "attempt"):
            MODULE.verify(contract(), SOURCE, api)

    def test_cli_uses_complete_contract_from_its_repository_checkout(self):
        fixture = contract()
        api = object()
        with patch.object(MODULE, "load_repository_contract", return_value=fixture) as load, \
                patch.object(MODULE, "GithubApi", return_value=api), \
                patch.object(MODULE, "verify") as verify, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(MODULE.main(["--repository", "Krilliac/SparkEngine", "--source-sha", SOURCE]), 0)
        load.assert_called_once_with()
        verify.assert_called_once_with(fixture, SOURCE, api)

    def test_api_rejects_duplicate_keys_oversized_and_non_object_responses(self):
        for body in (b'{"id":1,"id":2}', b'[]', b' ' * (MODULE.MAX_RESPONSE_BYTES + 1)):
            class RawResponse(Response):
                def read(self, limit):
                    return body
            api = MODULE.GithubApi("Krilliac/SparkEngine", "token", opener=lambda request, timeout: RawResponse(None))
            with self.subTest(size=len(body)), self.assertRaises(MODULE.PublicationEvidenceError):
                api.get("/repos/Krilliac/SparkEngine/actions/runs/42")

    def test_malformed_and_unpublished_payloads_fail_closed(self):
        route = "/repos/Krilliac/SparkEngine/actions/runs/42"
        for mutation in ({"status": "in_progress"}, {"run_attempt": 2}, {"path": None}):
            with self.subTest(mutation=mutation), self.assertRaises(MODULE.PublicationEvidenceError):
                MODULE.verify(contract(), SOURCE, self.api({route: mutation}))
        release = "/repos/Krilliac/SparkEngine/releases/latest"
        for mutation in ({"immutable": False}, {"draft": True}, {"prerelease": True}, {"tag_name": "nightly"}):
            with self.subTest(mutation=mutation), self.assertRaises(MODULE.PublicationEvidenceError):
                MODULE.verify(contract(), SOURCE, self.api({release: mutation}))

    def test_missing_finalizer_reference_is_rejected_without_network(self):
        with self.assertRaisesRegex(MODULE.PublicationEvidenceError, "no release.yml"):
            MODULE.verify(contract("Tests/readiness.json"), SOURCE, self.api())


if __name__ == "__main__":
    unittest.main()
