#!/usr/bin/env python3
"""Regression tests for the release acceptance gate.

Covers TOCTOU, replay, tag-drift, asset-tampering, and permission attacks
identified in the adversarial audit of the release pipeline.
"""

from __future__ import annotations

import base64
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch, MagicMock

SCRIPT = Path(__file__).with_name("release-acceptance-gate.py")
SPEC = importlib.util.spec_from_file_location("release_acceptance_gate", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

SHA = "a" * 40
OTHER_SHA = "b" * 40
REPOSITORY = "Krilliac/SparkEngine"
REPOSITORY_ID = 1001
API_URL = "https://api.github.com"
TOKEN = "test-token"
RELEASE_ID = 100
RELEASE_TAG = "nightly"
DIGEST_A = "sha256:" + "c" * 64
DIGEST_B = "sha256:" + "d" * 64
ASSET_NAMES = ["SparkEngine-Linux.tar.gz", "SparkEngine-Windows.zip", "SHA256SUMS"]


def _write_assets_file(path: Path, names: list[str] | None = None) -> None:
    path.write_text("\n".join(names or ASSET_NAMES) + "\n", encoding="utf-8")


def _write_digests_file(path: Path, digests: dict[str, str] | None = None) -> None:
    if digests is None:
        digests = {
            "SparkEngine-Linux.tar.gz": DIGEST_A,
            "SparkEngine-Windows.zip": DIGEST_B,
            "SHA256SUMS": "sha256:" + "e" * 64,
        }
    lines = []
    for name, digest in digests.items():
        hex_part = digest.removeprefix("sha256:")
        lines.append(f"{hex_part}  {name}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _release(release_id: int = RELEASE_ID, tag: str = RELEASE_TAG, **overrides):
    value = {
        "id": release_id,
        "tag_name": tag,
        "draft": True,
        "prerelease": True,
        "immutable": False,
    }
    value.update(overrides)
    return value


def _asset(name: str, digest: str, **overrides):
    value = {
        "name": name,
        "state": "uploaded",
        "digest": digest,
        "id": hash(name) % 10000 + 1,
    }
    value.update(overrides)
    return value


def _default_assets():
    return [
        _asset("SparkEngine-Linux.tar.gz", DIGEST_A),
        _asset("SparkEngine-Windows.zip", DIGEST_B),
        _asset("SHA256SUMS", "sha256:" + "e" * 64),
    ]


def _ci_run(run_id=42, sha=SHA, **overrides):
    value = {
        "id": run_id,
        "workflow_id": 202,
        "run_number": run_id,
        "run_attempt": 1,
        "name": "Build SparkEngine",
        "path": ".github/workflows/build.yml@refs/heads/Working",
        "head_sha": sha,
        "head_branch": "Working",
        "event": "push",
        "status": "completed",
        "conclusion": "success",
        "html_url": f"https://github.com/{REPOSITORY}/actions/runs/{run_id}",
        "repository": {"id": REPOSITORY_ID, "full_name": REPOSITORY},
        "head_repository": {"id": REPOSITORY_ID, "full_name": REPOSITORY},
    }
    value.update(overrides)
    return value


def _ci_runs_payload(runs):
    return {"total_count": len(runs), "workflow_runs": runs}


def _ci_jobs_payload(jobs):
    return {"total_count": len(jobs), "jobs": jobs}


def _gate_job(**overrides):
    value = {"name": "Required CI Gate", "status": "completed", "conclusion": "success"}
    value.update(overrides)
    return value


def _published_release(is_versioned=False, **overrides):
    value = {
        "id": RELEASE_ID,
        "tag_name": RELEASE_TAG,
        "draft": False,
        "prerelease": not is_versioned,
    }
    value.update(overrides)
    return value


class FakeApi:
    """Routes API calls to fixture data."""

    def __init__(
        self,
        release=None,
        assets=None,
        runs=None,
        jobs_by_run=None,
        working_sha=SHA,
        published=None,
        patch_should_fail=False,
    ):
        self.release = release or _release()
        self.assets = assets if assets is not None else _default_assets()
        self.runs = runs if runs is not None else [_ci_run()]
        self.jobs_by_run = jobs_by_run if jobs_by_run is not None else {42: [_gate_job()]}
        self.working_sha = working_sha
        self.published = published or _published_release()
        self.patch_should_fail = patch_should_fail
        self.fetch_calls: list[str] = []
        self.patch_calls: list[tuple[str, dict]] = []

    def fetch(self, url: str, token: str) -> Any:
        self.fetch_calls.append(url)
        if f"/releases/{RELEASE_ID}/assets" in url:
            return self.assets
        if f"/releases/{RELEASE_ID}" in url:
            return self.release
        if url.endswith("/commits/Working"):
            return {"sha": self.working_sha}
        if "/workflows/build.yml/runs" in url:
            page = int(url.rsplit("page=", 1)[1])
            start = (page - 1) * MODULE.CI_PAGE_SIZE
            return _ci_runs_payload(self.runs[start : start + MODULE.CI_PAGE_SIZE])
        if "/jobs?" in url:
            run_id = int(url.split("/runs/")[1].split("/")[0])
            page = int(url.rsplit("page=", 1)[1])
            jobs = self.jobs_by_run.get(run_id, [])
            start = (page - 1) * MODULE.CI_PAGE_SIZE
            return _ci_jobs_payload(jobs[start : start + MODULE.CI_PAGE_SIZE])
        raise MODULE.GateError(f"unexpected API path: {url}")

    def patch(self, url: str, token: str, body: dict, **_kwargs: Any) -> Any:
        self.patch_calls.append((url, body))
        if self.patch_should_fail:
            raise MODULE.GateError("PATCH failed")
        return self.published


class PostPatchTamperApi(FakeApi):
    """Expose an asset mutation between publication and post-PATCH recheck."""

    def __init__(self):
        super().__init__()
        self.tampered_assets = _default_assets()
        self.tampered_assets[0]["digest"] = "sha256:" + "f" * 64

    def fetch(self, url: str, token: str) -> Any:
        if f"/releases/{RELEASE_ID}/assets" in url and self.patch_calls:
            return self.tampered_assets
        return super().fetch(url, token)

    def patch(self, url: str, token: str, body: dict, **_kwargs: Any) -> Any:
        self.patch_calls.append((url, body))
        response = dict(self.published)
        response["draft"] = body.get("draft")
        if "prerelease" in body:
            response["prerelease"] = body["prerelease"]
        return response


class TestVerifyDraftRelease(unittest.TestCase):
    """Test 2: Asset digest mismatch at publication time."""

    def _patch_fetch(self, release=None, assets=None):
        original = MODULE._fetch_json
        rel = release if release is not None else _release()
        ast = assets if assets is not None else _default_assets()
        def fake(url, token):
            if f"/releases/{RELEASE_ID}/assets" in url:
                return ast
            if f"/releases/{RELEASE_ID}" in url:
                return rel
            return original(url, token)
        MODULE._fetch_json = fake
        return original

    def setUp(self):
        self._original_fetch = None

    def tearDown(self):
        if self._original_fetch is not None:
            MODULE._fetch_json = self._original_fetch

    def test_accepts_matching_draft(self):
        self._original_fetch = self._patch_fetch()
        MODULE.verify_draft_release(
            API_URL, TOKEN, REPOSITORY, RELEASE_ID, RELEASE_TAG, False,
            ASSET_NAMES, {n: a["digest"] for n, a in zip(ASSET_NAMES, _default_assets())},
        )

    def test_rejects_non_draft_release(self):
        self._original_fetch = self._patch_fetch(release=_release(draft=False))
        with self.assertRaisesRegex(MODULE.GateError, "not a draft"):
            MODULE.verify_draft_release(
                API_URL, TOKEN, REPOSITORY, RELEASE_ID, RELEASE_TAG, False,
                ASSET_NAMES, {},
            )

    def test_rejects_draft_with_wrong_release_channel(self):
        self._original_fetch = self._patch_fetch(release=_release(prerelease=False))
        with self.assertRaisesRegex(MODULE.GateError, "prerelease"):
            MODULE.verify_draft_release(
                API_URL, TOKEN, REPOSITORY, RELEASE_ID, RELEASE_TAG, False,
                ASSET_NAMES, {n: a["digest"] for n, a in zip(ASSET_NAMES, _default_assets())},
            )

    def test_rejects_immutable_draft(self):
        self._original_fetch = self._patch_fetch(release=_release(immutable=True))
        with self.assertRaisesRegex(MODULE.GateError, "immutable"):
            MODULE.verify_draft_release(
                API_URL, TOKEN, REPOSITORY, RELEASE_ID, RELEASE_TAG, False,
                ASSET_NAMES, {n: a["digest"] for n, a in zip(ASSET_NAMES, _default_assets())},
            )

    def test_rejects_wrong_release_id(self):
        original_fetch = MODULE._fetch_json
        MODULE._fetch_json = lambda url, token: {"id": 999, "tag_name": RELEASE_TAG, "draft": True}
        try:
            with self.assertRaisesRegex(MODULE.GateError, "release ID mismatch"):
                MODULE.verify_draft_release(
                    API_URL, TOKEN, REPOSITORY, RELEASE_ID, RELEASE_TAG, False,
                    ASSET_NAMES, {},
                )
        finally:
            MODULE._fetch_json = original_fetch

    def test_rejects_digest_mismatch(self):
        """Core TOCTOU test: asset re-uploaded with different content."""
        tampered = _default_assets()
        tampered[0]["digest"] = "sha256:" + "f" * 64
        original_fetch = MODULE._fetch_json

        call_count = [0]
        def fake_fetch(url, token):
            call_count[0] += 1
            if f"/releases/{RELEASE_ID}/assets" in url:
                return tampered
            if f"/releases/{RELEASE_ID}" in url:
                return _release()
            return original_fetch(url, token)

        MODULE._fetch_json = fake_fetch
        try:
            with self.assertRaisesRegex(MODULE.GateError, "digest mismatch"):
                expected_digests = {n: a["digest"] for n, a in zip(ASSET_NAMES, _default_assets())}
                MODULE.verify_draft_release(
                    API_URL, TOKEN, REPOSITORY, RELEASE_ID, RELEASE_TAG, False,
                    ASSET_NAMES, expected_digests,
                )
        finally:
            MODULE._fetch_json = original_fetch

    def test_rejects_missing_asset(self):
        original_fetch = MODULE._fetch_json
        def fake_fetch(url, token):
            if f"/releases/{RELEASE_ID}/assets" in url:
                return _default_assets()[:2]
            if f"/releases/{RELEASE_ID}" in url:
                return _release()
            return original_fetch(url, token)

        MODULE._fetch_json = fake_fetch
        try:
            with self.assertRaisesRegex(MODULE.GateError, "asset mismatch"):
                MODULE.verify_draft_release(
                    API_URL, TOKEN, REPOSITORY, RELEASE_ID, RELEASE_TAG, False,
                    ASSET_NAMES, {n: a["digest"] for n, a in zip(ASSET_NAMES, _default_assets())},
                )
        finally:
            MODULE._fetch_json = original_fetch

    def test_rejects_extra_asset(self):
        original_fetch = MODULE._fetch_json
        extra = _default_assets() + [_asset("EXTRA.txt", "sha256:" + "0" * 64)]
        def fake_fetch(url, token):
            if f"/releases/{RELEASE_ID}/assets" in url:
                return extra
            if f"/releases/{RELEASE_ID}" in url:
                return _release()
            return original_fetch(url, token)

        MODULE._fetch_json = fake_fetch
        try:
            with self.assertRaisesRegex(MODULE.GateError, "asset mismatch"):
                MODULE.verify_draft_release(
                    API_URL, TOKEN, REPOSITORY, RELEASE_ID, RELEASE_TAG, False,
                    ASSET_NAMES, {n: a["digest"] for n, a in zip(ASSET_NAMES, _default_assets())},
                )
        finally:
            MODULE._fetch_json = original_fetch

    def test_rejects_non_uploaded_asset(self):
        original_fetch = MODULE._fetch_json
        assets = _default_assets()
        assets[1]["state"] = "starter"
        def fake_fetch(url, token):
            if f"/releases/{RELEASE_ID}/assets" in url:
                return assets
            if f"/releases/{RELEASE_ID}" in url:
                return _release()
            return original_fetch(url, token)

        MODULE._fetch_json = fake_fetch
        try:
            with self.assertRaisesRegex(MODULE.GateError, "not in 'uploaded' state"):
                MODULE.verify_draft_release(
                    API_URL, TOKEN, REPOSITORY, RELEASE_ID, RELEASE_TAG, False,
                    ASSET_NAMES, {n: a["digest"] for n, a in zip(ASSET_NAMES, _default_assets())},
                )
        finally:
            MODULE._fetch_json = original_fetch

    def test_fetches_all_asset_pages_before_comparing_inventory(self):
        expected_names = [f"asset-{index:03d}.zip" for index in range(101)]
        expected_digests = {name: DIGEST_A for name in expected_names}
        assets = [
            _asset(name, DIGEST_A, id=index + 1)
            for index, name in enumerate(expected_names)
        ]
        original_fetch = MODULE._fetch_json
        asset_urls: list[str] = []

        def fake_fetch(url, token):
            if f"/releases/{RELEASE_ID}/assets?" in url:
                asset_urls.append(url)
                if url.endswith("per_page=100&page=1"):
                    return assets[:100]
                if url.endswith("per_page=100&page=2"):
                    return assets[100:]
                self.fail(f"unexpected asset page request: {url}")
            if f"/releases/{RELEASE_ID}" in url:
                return _release()
            return original_fetch(url, token)

        MODULE._fetch_json = fake_fetch
        try:
            MODULE.verify_draft_release(
                API_URL, TOKEN, REPOSITORY, RELEASE_ID, RELEASE_TAG, False,
                expected_names, expected_digests,
            )
        finally:
            MODULE._fetch_json = original_fetch

        self.assertEqual(
            asset_urls,
            [
                f"{API_URL}/repos/{REPOSITORY}/releases/{RELEASE_ID}/assets?per_page=100&page=1",
                f"{API_URL}/repos/{REPOSITORY}/releases/{RELEASE_ID}/assets?per_page=100&page=2",
            ],
        )

    def test_rejects_duplicate_asset_name_across_pages(self):
        expected_names = [f"asset-{index:03d}.zip" for index in range(100)]
        expected_digests = {name: DIGEST_A for name in expected_names}
        first_page = [
            _asset(name, DIGEST_A, id=index + 1)
            for index, name in enumerate(expected_names)
        ]
        duplicate = _asset(expected_names[0], DIGEST_A, id=101)
        original_fetch = MODULE._fetch_json

        def fake_fetch(url, token):
            if f"/releases/{RELEASE_ID}/assets?" in url:
                if url.endswith("per_page=100&page=1"):
                    return first_page
                if url.endswith("per_page=100&page=2"):
                    return [duplicate]
                self.fail(f"unexpected asset page request: {url}")
            if f"/releases/{RELEASE_ID}" in url:
                return _release()
            return original_fetch(url, token)

        MODULE._fetch_json = fake_fetch
        try:
            with self.assertRaisesRegex(MODULE.GateError, "duplicate asset name"):
                MODULE.verify_draft_release(
                    API_URL, TOKEN, REPOSITORY, RELEASE_ID, RELEASE_TAG, False,
                    expected_names, expected_digests,
                )
        finally:
            MODULE._fetch_json = original_fetch


class TestVerifyTag(unittest.TestCase):
    """Test 3: Tag drift at publication time."""

    @patch("subprocess.run")
    def test_accepts_matching_tag(self, mock_run):
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=f"{SHA}\trefs/tags/{RELEASE_TAG}\n",
        )
        MODULE.verify_tag(TOKEN, RELEASE_TAG, SHA)

    @patch("subprocess.run")
    def test_uses_authenticated_noninteractive_tag_lookup(self, mock_run):
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=f"{SHA}\trefs/tags/{RELEASE_TAG}\n",
        )
        MODULE.verify_tag(TOKEN, RELEASE_TAG, SHA)
        encoded = base64.b64encode(f"x-access-token:{TOKEN}".encode("utf-8")).decode("ascii")
        self.assertEqual(
            mock_run.call_args.args[0],
            [
                "git",
                "-c",
                f"http.https://github.com/.extraheader=AUTHORIZATION: basic {encoded}",
                "ls-remote",
                "origin",
                f"refs/tags/{RELEASE_TAG}",
            ],
        )
        self.assertEqual(mock_run.call_args.kwargs["env"]["GIT_TERMINAL_PROMPT"], "0")

    @patch("subprocess.run")
    def test_accepts_annotated_tag_that_peels_to_target(self, mock_run):
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=(
                f"{OTHER_SHA}\trefs/tags/v1.0.0\n"
                f"{SHA}\trefs/tags/v1.0.0^{{}}\n"
            ),
        )
        try:
            MODULE.verify_tag(TOKEN, "v1.0.0", SHA)
        except MODULE.GateError as error:
            self.fail(f"valid annotated tag rejected: {error}")

    @patch("subprocess.run")
    def test_rejects_tag_pointing_elsewhere(self, mock_run):
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=f"{OTHER_SHA}\trefs/tags/{RELEASE_TAG}\n",
        )
        with self.assertRaisesRegex(MODULE.GateError, f"points to {OTHER_SHA}"):
            MODULE.verify_tag(TOKEN, RELEASE_TAG, SHA)

    @patch("subprocess.run")
    def test_rejects_missing_tag(self, mock_run):
        mock_run.return_value = MagicMock(returncode=0, stdout="")
        with self.assertRaisesRegex(MODULE.GateError, "missing tag ref"):
            MODULE.verify_tag(TOKEN, RELEASE_TAG, SHA)

    @patch("subprocess.run")
    def test_rejects_unexpected_or_duplicate_tag_refs(self, mock_run):
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=(
                f"{SHA}\trefs/tags/{RELEASE_TAG}\n"
                f"{SHA}\trefs/tags/{RELEASE_TAG}\n"
            ),
        )
        with self.assertRaisesRegex(MODULE.GateError, "malformed tag ref response"):
            MODULE.verify_tag(TOKEN, RELEASE_TAG, SHA)

    @patch("subprocess.run")
    def test_rejects_git_failure(self, mock_run):
        mock_run.return_value = MagicMock(returncode=1, stderr="network error")
        with self.assertRaisesRegex(MODULE.GateError, "git ls-remote failed"):
            MODULE.verify_tag(TOKEN, RELEASE_TAG, SHA)

    @patch("subprocess.run", side_effect=subprocess.TimeoutExpired("git", 30))
    def test_rejects_git_timeout(self, _mock_run):
        with self.assertRaisesRegex(MODULE.GateError, "git ls-remote failed"):
            MODULE.verify_tag(TOKEN, RELEASE_TAG, SHA)


class TestPatchAttemptMarker(unittest.TestCase):
    """A recovery marker must appear only once a PATCH can be dispatched."""

    @patch.object(MODULE, "urlopen")
    def test_marks_patch_started_before_network_dispatch(self, mock_urlopen):
        with tempfile.TemporaryDirectory() as tmpdir:
            marker = Path(tmpdir) / "patch-started"
            observed_marker_states: list[bool] = []
            response = MagicMock()
            response.__enter__.return_value = response
            response.__exit__.return_value = False
            response.read.return_value = b'{"ok": true}'

            def capture_request(_request, timeout):
                observed_marker_states.append(marker.is_file())
                return response

            mock_urlopen.side_effect = capture_request
            with patch.dict(
                os.environ,
                {"RELEASE_ACCEPTANCE_PATCH_STARTED_FILE": str(marker)},
                clear=False,
            ):
                self.assertEqual(
                    MODULE._patch_json(
                        "https://api.github.com/repos/example/release", TOKEN, {"draft": False}
                    ),
                    {"ok": True},
                )
            self.assertEqual(observed_marker_states, [True])

    @patch.object(MODULE, "urlopen")
    def test_rejects_stale_marker_before_network_dispatch(self, mock_urlopen):
        with tempfile.TemporaryDirectory() as tmpdir:
            marker = Path(tmpdir) / "patch-started"
            marker.write_text("stale\n", encoding="utf-8")
            response = MagicMock()
            response.__enter__.return_value = response
            response.__exit__.return_value = False
            response.read.return_value = b'{"ok": true}'
            mock_urlopen.return_value = response
            with patch.dict(
                os.environ,
                {"RELEASE_ACCEPTANCE_PATCH_STARTED_FILE": str(marker)},
                clear=False,
            ):
                with self.assertRaisesRegex(MODULE.GateError, "cannot record release PATCH attempt"):
                    MODULE._patch_json(
                        "https://api.github.com/repos/example/release", TOKEN, {"draft": False}
                    )
            mock_urlopen.assert_not_called()


class TestVerifyCIGate(unittest.TestCase):
    """Tests 4-5: Build conclusion flip and status replay."""

    def test_accepts_successful_gate(self):
        original_fetch = MODULE._fetch_json
        job_urls: list[str] = []

        def fake_fetch(url, token):
            if "/workflows/build.yml/runs" in url:
                return _ci_runs_payload([_ci_run()])
            if "/jobs?" in url:
                job_urls.append(url)
                return _ci_jobs_payload([_gate_job()])
            return original_fetch(url, token)

        MODULE._fetch_json = fake_fetch
        try:
            MODULE.verify_ci_gate(API_URL, TOKEN, REPOSITORY, SHA)
        finally:
            MODULE._fetch_json = original_fetch
        self.assertEqual(
            job_urls,
            [
                f"{API_URL}/repos/{REPOSITORY}/actions/runs/42/attempts/1/jobs"
                "?per_page=100&page=1",
            ],
        )

    def test_rejects_no_matching_runs(self):
        original_fetch = MODULE._fetch_json
        MODULE._fetch_json = lambda url, token: _ci_runs_payload([])
        try:
            with self.assertRaisesRegex(MODULE.GateError, "no completed successful"):
                MODULE.verify_ci_gate(API_URL, TOKEN, REPOSITORY, SHA)
        finally:
            MODULE._fetch_json = original_fetch

    def test_rejects_build_run_with_wrong_conclusion(self):
        """Test 4: Build run conclusion flip — success is the only accepted conclusion."""
        original_fetch = MODULE._fetch_json
        def fake_fetch(url, token):
            if "/workflows/build.yml/runs" in url:
                return _ci_runs_payload([_ci_run(conclusion="failure")])
            return original_fetch(url, token)

        MODULE._fetch_json = fake_fetch
        try:
            with self.assertRaisesRegex(MODULE.GateError, "newest eligible Build run"):
                MODULE.verify_ci_gate(API_URL, TOKEN, REPOSITORY, SHA)
        finally:
            MODULE._fetch_json = original_fetch

    def test_rejects_gate_job_failure(self):
        original_fetch = MODULE._fetch_json
        def fake_fetch(url, token):
            if "/workflows/build.yml/runs" in url:
                return _ci_runs_payload([_ci_run()])
            if "/jobs?" in url:
                return _ci_jobs_payload([_gate_job(conclusion="failure")])
            return original_fetch(url, token)

        MODULE._fetch_json = fake_fetch
        try:
            with self.assertRaisesRegex(MODULE.GateError, "Required CI Gate"):
                MODULE.verify_ci_gate(API_URL, TOKEN, REPOSITORY, SHA)
        finally:
            MODULE._fetch_json = original_fetch

    def test_rejects_newer_failed_run_instead_of_accepting_old_success(self):
        old_success = _ci_run(run_id=42, run_number=10, run_attempt=1)
        newer_failure = _ci_run(
            run_id=43,
            run_number=11,
            run_attempt=1,
            conclusion="failure",
        )
        original_fetch = MODULE._fetch_json

        def fake_fetch(url, token):
            if "/workflows/build.yml/runs" in url:
                return _ci_runs_payload([old_success, newer_failure])
            if "/jobs?" in url:
                return _ci_jobs_payload([_gate_job()])
            return original_fetch(url, token)

        MODULE._fetch_json = fake_fetch
        try:
            with self.assertRaisesRegex(MODULE.GateError, "newest eligible Build run"):
                MODULE.verify_ci_gate(API_URL, TOKEN, REPOSITORY, SHA)
        finally:
            MODULE._fetch_json = original_fetch

    def test_rejects_eligible_run_without_exact_working_provenance(self):
        mutations = (
            {"head_branch": "feature/untrusted"},
            {"path": ".github/workflows/other.yml@refs/heads/Working"},
            {"head_repository": {"id": 2002, "full_name": "fork/SparkEngine"}},
        )
        for overrides in mutations:
            with self.subTest(overrides=overrides):
                original_fetch = MODULE._fetch_json

                def fake_fetch(url, token):
                    if "/workflows/build.yml/runs" in url:
                        return _ci_runs_payload([_ci_run(**overrides)])
                    if "/jobs?" in url:
                        return _ci_jobs_payload([_gate_job()])
                    return original_fetch(url, token)

                MODULE._fetch_json = fake_fetch
                try:
                    with self.assertRaisesRegex(
                        MODULE.GateError, "exact base-repository Working workflow"
                    ):
                        MODULE.verify_ci_gate(API_URL, TOKEN, REPOSITORY, SHA)
                finally:
                    MODULE._fetch_json = original_fetch

    def test_loads_later_ci_run_pages_before_selecting_newest_run(self):
        first_page = [
            _ci_run(run_id=index + 1, run_number=index + 1)
            for index in range(100)
        ]
        newest_failure = _ci_run(
            run_id=101,
            run_number=101,
            run_attempt=1,
            conclusion="failure",
        )
        original_fetch = MODULE._fetch_json
        run_urls: list[str] = []

        def fake_fetch(url, token):
            if "/workflows/build.yml/runs" in url:
                run_urls.append(url)
                if url.endswith("per_page=100&page=1"):
                    return {"total_count": 101, "workflow_runs": first_page}
                if url.endswith("per_page=100&page=2"):
                    return {"total_count": 101, "workflow_runs": [newest_failure]}
                self.fail(f"unexpected CI run page request: {url}")
            if "/jobs?" in url:
                return _ci_jobs_payload([_gate_job()])
            return original_fetch(url, token)

        MODULE._fetch_json = fake_fetch
        try:
            with self.assertRaisesRegex(MODULE.GateError, "newest eligible Build run"):
                MODULE.verify_ci_gate(API_URL, TOKEN, REPOSITORY, SHA)
        finally:
            MODULE._fetch_json = original_fetch

        self.assertEqual(
            run_urls,
            [
                f"{API_URL}/repos/{REPOSITORY}/actions/workflows/build.yml/runs"
                f"?head_sha={SHA}&per_page=100&page=1",
                f"{API_URL}/repos/{REPOSITORY}/actions/workflows/build.yml/runs"
                f"?head_sha={SHA}&per_page=100&page=2",
            ],
        )

    def test_rejects_run_for_different_sha(self):
        """Test 5: Status target pointing to different source commit."""
        original_fetch = MODULE._fetch_json
        def fake_fetch(url, token):
            if "/workflows/build.yml/runs" in url:
                return _ci_runs_payload([_ci_run(sha=OTHER_SHA)])
            return original_fetch(url, token)

        MODULE._fetch_json = fake_fetch
        try:
            with self.assertRaisesRegex(MODULE.GateError, "different commit"):
                MODULE.verify_ci_gate(API_URL, TOKEN, REPOSITORY, SHA)
        finally:
            MODULE._fetch_json = original_fetch

    def test_rejects_pull_request_event(self):
        original_fetch = MODULE._fetch_json
        def fake_fetch(url, token):
            if "/workflows/build.yml/runs" in url:
                return _ci_runs_payload([_ci_run(event="pull_request")])
            return original_fetch(url, token)

        MODULE._fetch_json = fake_fetch
        try:
            with self.assertRaisesRegex(MODULE.GateError, "no completed successful"):
                MODULE.verify_ci_gate(API_URL, TOKEN, REPOSITORY, SHA)
        finally:
            MODULE._fetch_json = original_fetch

    def test_api_error_fails_closed(self):
        original_fetch = MODULE._fetch_json
        MODULE._fetch_json = lambda url, token: (_ for _ in ()).throw(
            MODULE.GateError("network down")
        )
        try:
            with self.assertRaises(MODULE.GateError):
                MODULE.verify_ci_gate(API_URL, TOKEN, REPOSITORY, SHA)
        finally:
            MODULE._fetch_json = original_fetch


class TestAcceptanceGateIntegration(unittest.TestCase):
    """End-to-end integration: verify + PATCH in one step."""

    def _run_gate(self, api, is_versioned=False, tag=RELEASE_TAG):
        with tempfile.TemporaryDirectory() as tmpdir:
            assets_file = Path(tmpdir) / "expected-assets.txt"
            digests_file = Path(tmpdir) / "expected-digests.txt"
            _write_assets_file(assets_file)
            _write_digests_file(digests_file)

            original_fetch = MODULE._fetch_json
            original_patch = MODULE._patch_json
            MODULE._fetch_json = api.fetch
            MODULE._patch_json = api.patch
            try:
                return MODULE.acceptance_gate(
                    api_url=API_URL,
                    token=TOKEN,
                    repository=REPOSITORY,
                    release_id=RELEASE_ID,
                    release_tag=tag,
                    target_sha=SHA,
                    is_versioned=is_versioned,
                    expected_assets_file=assets_file,
                    expected_digests_file=digests_file,
                )
            finally:
                MODULE._fetch_json = original_fetch
                MODULE._patch_json = original_patch

    @patch("subprocess.run")
    def test_full_nightly_publication(self, mock_run):
        mock_run.return_value = MagicMock(
            returncode=0, stdout=f"{SHA}\trefs/tags/{RELEASE_TAG}\n"
        )
        api = FakeApi()
        result = self._run_gate(api)
        self.assertEqual(result["draft"], False)
        self.assertEqual(result["prerelease"], True)
        self.assertEqual(len(api.patch_calls), 1)
        _, body = api.patch_calls[0]
        self.assertFalse(body["draft"])
        self.assertEqual(body["make_latest"], "false")

    @patch("subprocess.run")
    def test_blocks_when_working_advanced_before_publication(self, mock_run):
        mock_run.return_value = MagicMock(
            returncode=0, stdout=f"{SHA}\trefs/tags/{RELEASE_TAG}\n"
        )
        api = FakeApi(working_sha=OTHER_SHA)
        with self.assertRaisesRegex(MODULE.GateError, "Working advanced"):
            self._run_gate(api)
        self.assertEqual(api.patch_calls, [])

    @patch("subprocess.run")
    def test_full_stable_publication(self, mock_run):
        mock_run.return_value = MagicMock(
            returncode=0, stdout=f"{SHA}\trefs/tags/v1.0.0\n"
        )
        api = FakeApi(
            release=_release(tag="v1.0.0", prerelease=False),
            published=_published_release(is_versioned=True, tag_name="v1.0.0"),
        )
        result = self._run_gate(api, is_versioned=True, tag="v1.0.0")
        self.assertEqual(result["draft"], False)
        self.assertEqual(result["prerelease"], False)
        _, body = api.patch_calls[0]
        self.assertEqual(body["make_latest"], "true")

    @patch("subprocess.run")
    def test_blocks_versioned_publication_with_rolling_tag(self, mock_run):
        """A stable publication must not accept the nightly tag identity."""
        mock_run.return_value = MagicMock(
            returncode=0, stdout=f"{SHA}\trefs/tags/nightly\n"
        )
        api = FakeApi(
            release=_release(tag="nightly", prerelease=False),
            published=_published_release(is_versioned=True, tag_name="nightly"),
        )
        with self.assertRaisesRegex(MODULE.GateError, "versioned release tag"):
            self._run_gate(api, is_versioned=True, tag="nightly")
        self.assertEqual(api.patch_calls, [])

    @patch("subprocess.run")
    def test_blocks_on_tag_drift(self, mock_run):
        """Test 3: Tag moved between verify and PATCH."""
        mock_run.return_value = MagicMock(
            returncode=0, stdout=f"{OTHER_SHA}\trefs/tags/{RELEASE_TAG}\n"
        )
        api = FakeApi()
        with self.assertRaisesRegex(MODULE.GateError, "points to"):
            self._run_gate(api)
        self.assertEqual(len(api.patch_calls), 0)

    @patch("subprocess.run")
    def test_blocks_on_ci_failure(self, mock_run):
        mock_run.return_value = MagicMock(
            returncode=0, stdout=f"{SHA}\trefs/tags/{RELEASE_TAG}\n"
        )
        api = FakeApi(runs=[])
        with self.assertRaisesRegex(MODULE.GateError, "no completed successful"):
            self._run_gate(api)
        self.assertEqual(len(api.patch_calls), 0)

    @patch("subprocess.run")
    def test_blocks_on_asset_tampering(self, mock_run):
        """Test 2: Asset digest changed between staging and acceptance gate."""
        mock_run.return_value = MagicMock(
            returncode=0, stdout=f"{SHA}\trefs/tags/{RELEASE_TAG}\n"
        )
        tampered_assets = _default_assets()
        tampered_assets[0]["digest"] = "sha256:" + "f" * 64
        api = FakeApi(assets=tampered_assets)
        with self.assertRaisesRegex(MODULE.GateError, "digest mismatch"):
            self._run_gate(api)
        self.assertEqual(len(api.patch_calls), 0)

    @patch("subprocess.run")
    def test_blocks_on_non_draft(self, mock_run):
        mock_run.return_value = MagicMock(
            returncode=0, stdout=f"{SHA}\trefs/tags/{RELEASE_TAG}\n"
        )
        api = FakeApi(release=_release(draft=False))
        with self.assertRaisesRegex(MODULE.GateError, "not a draft"):
            self._run_gate(api)
        self.assertEqual(len(api.patch_calls), 0)

    @patch("subprocess.run")
    def test_patch_failure_propagates(self, mock_run):
        mock_run.return_value = MagicMock(
            returncode=0, stdout=f"{SHA}\trefs/tags/{RELEASE_TAG}\n"
        )
        api = FakeApi(patch_should_fail=True)
        with self.assertRaisesRegex(MODULE.GateError, "PATCH failed"):
            self._run_gate(api)

    @patch("subprocess.run")
    def test_redrafts_when_assets_change_after_publication(self, mock_run):
        """A post-PATCH asset race must not leave a public tampered release."""
        mock_run.return_value = MagicMock(
            returncode=0, stdout=f"{SHA}\trefs/tags/{RELEASE_TAG}\n"
        )
        api = PostPatchTamperApi()
        with self.assertRaisesRegex(MODULE.GateError, "post-PATCH"):
            self._run_gate(api)
        self.assertEqual(
            [body["draft"] for _url, body in api.patch_calls],
            [False, True],
        )


class TestInputValidation(unittest.TestCase):
    """Ensure the gate fails closed on malformed inputs."""

    def test_main_rejects_empty_token(self):
        env = {"GITHUB_REPOSITORY": REPOSITORY, "RELEASE_ID": "1",
               "RELEASE_TAG": "v1", "TARGET_SHA": SHA, "IS_VERSIONED": "false",
               "EXPECTED_ASSETS_FILE": "/x", "EXPECTED_DIGESTS_FILE": "/x"}
        with patch.dict(os.environ, env, clear=True):
            self.assertNotEqual(MODULE.main(), 0)

    def test_main_rejects_bad_repository(self):
        env = {"GH_TOKEN": TOKEN, "GITHUB_REPOSITORY": "bad",
               "RELEASE_ID": "1", "RELEASE_TAG": "v1", "TARGET_SHA": SHA,
               "IS_VERSIONED": "false", "EXPECTED_ASSETS_FILE": "/x",
               "EXPECTED_DIGESTS_FILE": "/x"}
        with patch.dict(os.environ, env, clear=True):
            self.assertNotEqual(MODULE.main(), 0)

    def test_main_rejects_bad_sha(self):
        env = {"GH_TOKEN": TOKEN, "GITHUB_REPOSITORY": REPOSITORY,
               "RELEASE_ID": "1", "RELEASE_TAG": "v1", "TARGET_SHA": "abc",
               "IS_VERSIONED": "false", "EXPECTED_ASSETS_FILE": "/x",
               "EXPECTED_DIGESTS_FILE": "/x"}
        with patch.dict(os.environ, env, clear=True):
            self.assertNotEqual(MODULE.main(), 0)

    def test_main_rejects_missing_env(self):
        env = {
            "GH_TOKEN": TOKEN,
            "GITHUB_REPOSITORY": REPOSITORY,
            "RELEASE_ID": str(RELEASE_ID),
            "RELEASE_TAG": RELEASE_TAG,
            "TARGET_SHA": SHA,
            "IS_VERSIONED": "false",
            "EXPECTED_ASSETS_FILE": "/nonexistent/assets",
            "EXPECTED_DIGESTS_FILE": "/nonexistent/digests",
        }
        for key in ("GH_TOKEN", "RELEASE_ID", "RELEASE_TAG", "TARGET_SHA", "IS_VERSIONED"):
            with self.subTest(missing=key):
                partial = {k: v for k, v in env.items() if k != key}
                with patch.dict(os.environ, partial, clear=True):
                    self.assertNotEqual(MODULE.main(), 0)


class TestExpectedFileParsing(unittest.TestCase):
    """Test 7 analog: explicit errors, not assert."""

    def test_rejects_empty_assets_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            p = Path(tmpdir) / "empty.txt"
            p.write_text("", encoding="utf-8")
            with self.assertRaisesRegex(MODULE.GateError, "invalid entries"):
                MODULE._read_expected_assets(p)

    def test_rejects_duplicate_assets(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            p = Path(tmpdir) / "dupes.txt"
            p.write_text("a.zip\na.zip\n", encoding="utf-8")
            with self.assertRaisesRegex(MODULE.GateError, "duplicates"):
                MODULE._read_expected_assets(p)

    def test_rejects_mismatched_digests(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            assets = Path(tmpdir) / "assets.txt"
            digests = Path(tmpdir) / "digests.txt"
            assets.write_text("a.zip\nb.zip\n")
            digests.write_text(f"{'c' * 64}  a.zip\n")
            names = MODULE._read_expected_assets(assets)
            with self.assertRaisesRegex(MODULE.GateError, "do not match"):
                MODULE._read_expected_digests(digests, names)

    def test_rejects_nonexistent_file(self):
        with self.assertRaisesRegex(MODULE.GateError, "not found"):
            MODULE._read_expected_assets(Path("/nonexistent/file.txt"))


if __name__ == "__main__":
    unittest.main()
