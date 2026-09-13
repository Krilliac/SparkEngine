#!/usr/bin/env python3
"""Fail-closed acceptance gate immediately before a release PATCH.

Re-reads the draft release, its assets, the release tag, and the Required
CI Gate status from the GitHub API.  Only if every check passes does it
execute the PATCH that sets draft=false.  This narrows the final verification
window and records the instant at which recovery may be needed.
"""

from __future__ import annotations

import base64
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


SHA_PATTERN = re.compile(r"[0-9a-fA-F]{40}")
ASSET_PAGE_SIZE = 100
MAX_ASSET_PAGES = 100
CI_PAGE_SIZE = 100
MAX_CI_RUNS = 200
MAX_CI_JOBS = 200
ALLOWED_CI_EVENTS = frozenset({"push", "workflow_dispatch"})
BUILD_WORKFLOW_NAME = "Build SparkEngine"
BUILD_WORKFLOW_PATH = ".github/workflows/build.yml"
WORKING_BRANCH = "Working"
VERSION_TAG_PATTERN = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+")


class GateError(Exception):
    pass


def _fetch_json(url: str, token: str) -> Any:
    request = Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "SparkEngine-release-acceptance-gate",
        },
    )
    try:
        with urlopen(request, timeout=30) as response:  # noqa: S310
            return json.load(response)
    except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as error:
        raise GateError(f"GitHub API request failed: {error}") from error


def _mark_patch_started() -> None:
    """Persist an optional workflow recovery marker immediately before PATCH."""

    marker_raw = os.environ.get("RELEASE_ACCEPTANCE_PATCH_STARTED_FILE", "")
    if not marker_raw:
        return
    try:
        descriptor = os.open(
            marker_raw,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL,
            0o600,
        )
        with os.fdopen(descriptor, "w", encoding="utf-8") as marker:
            marker.write("PATCH dispatch started\n")
    except OSError as error:
        raise GateError(f"cannot record release PATCH attempt: {error}") from error


def _patch_json(
    url: str,
    token: str,
    body: dict[str, Any],
    *,
    mark_attempt: bool = True,
) -> Any:
    data = json.dumps(body).encode("utf-8")
    request = Request(
        url,
        data=data,
        method="PATCH",
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "SparkEngine-release-acceptance-gate",
        },
    )
    if mark_attempt:
        _mark_patch_started()
    try:
        with urlopen(request, timeout=30) as response:  # noqa: S310
            return json.load(response)
    except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as error:
        raise GateError(f"GitHub API PATCH failed: {error}") from error


def _require_str(value: str | None, name: str) -> str:
    if not value:
        raise GateError(f"{name} is required")
    return value


def _require_positive_int(raw: str | None, name: str) -> int:
    value = _require_str(raw, name)
    if not value.isdigit() or int(value) < 1:
        raise GateError(f"{name} must be a positive integer")
    return int(value)


def _read_expected_assets(path: Path) -> list[str]:
    if not path.is_file():
        raise GateError(f"expected assets file not found: {path}")
    names = path.read_text(encoding="utf-8").splitlines()
    if not names or any(not n or n != Path(n).name for n in names):
        raise GateError("expected assets file contains invalid entries")
    if len(names) != len(set(names)):
        raise GateError("expected assets file contains duplicates")
    return names


def _read_expected_digests(path: Path, expected_names: list[str]) -> dict[str, str]:
    if not path.is_file():
        raise GateError(f"expected digests file not found: {path}")
    digests: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{64})  (.+)", line)
        if not match or match.group(2) in digests:
            raise GateError("expected digests file is malformed or contains duplicates")
        digests[match.group(2)] = "sha256:" + match.group(1).lower()
    if set(digests) != set(expected_names):
        raise GateError("expected digests do not match expected asset names")
    return digests


def _validate_release_tag(release_tag: str, is_versioned: bool) -> None:
    """Require the tag identity to match the publication channel."""
    if is_versioned:
        if VERSION_TAG_PATTERN.fullmatch(release_tag) is None:
            raise GateError("versioned release tag must have the form vMAJOR.MINOR.PATCH")
    elif release_tag != "nightly":
        raise GateError("nightly publication must use the nightly release tag")


def _fetch_release_assets(
    api_url: str,
    token: str,
    repository: str,
    release_id: int,
) -> list[dict[str, Any]]:
    """Fetch an exact, bounded release-asset inventory across API pages."""

    assets: list[dict[str, Any]] = []
    asset_ids: set[int] = set()
    asset_names: set[str] = set()
    for page in range(1, MAX_ASSET_PAGES + 1):
        assets_url = (
            f"{api_url}/repos/{repository}/releases/{release_id}/assets"
            f"?per_page={ASSET_PAGE_SIZE}&page={page}"
        )
        page_assets = _fetch_json(assets_url, token)
        if not isinstance(page_assets, list) or len(page_assets) > ASSET_PAGE_SIZE:
            raise GateError("release assets response is not a bounded list")
        for asset in page_assets:
            if not isinstance(asset, dict):
                raise GateError("release asset is not an object")
            asset_id = asset.get("id")
            name = asset.get("name")
            if type(asset_id) is not int or asset_id < 1:
                raise GateError("release asset has no valid ID")
            if not isinstance(name, str) or not name:
                raise GateError("release asset has no valid name")
            if asset_id in asset_ids:
                raise GateError(f"duplicate asset ID in release inventory: {asset_id}")
            if name in asset_names:
                raise GateError(f"duplicate asset name in release inventory: {name}")
            asset_ids.add(asset_id)
            asset_names.add(name)
            assets.append(asset)
        if len(page_assets) < ASSET_PAGE_SIZE:
            return assets
    raise GateError(f"release assets exceed the {MAX_ASSET_PAGES}-page acceptance limit")


def _verify_release_assets(
    assets: list[dict[str, Any]],
    expected_names: list[str],
    expected_digests: dict[str, str],
) -> None:
    """Verify the complete asset inventory against the frozen manifest."""
    asset_names = {asset["name"] for asset in assets}
    if len(assets) != len(expected_names) or asset_names != set(expected_names):
        missing = set(expected_names) - asset_names
        extra = asset_names - set(expected_names)
        raise GateError(f"asset mismatch — missing: {missing}, extra: {extra}")

    for asset in assets:
        name = asset["name"]
        if asset.get("state") != "uploaded":
            raise GateError(f"asset '{name}' is not in 'uploaded' state")
        expected_digest = expected_digests[name]
        actual_digest = asset.get("digest")
        if not isinstance(actual_digest, str) or actual_digest.lower() != expected_digest:
            raise GateError(
                f"asset '{name}' digest mismatch: expected {expected_digest}, got {actual_digest}"
            )


def verify_draft_release(
    api_url: str,
    token: str,
    repository: str,
    release_id: int,
    release_tag: str,
    is_versioned: bool,
    expected_names: list[str],
    expected_digests: dict[str, str],
) -> None:
    release = _fetch_json(f"{api_url}/repos/{repository}/releases/{release_id}", token)
    if not isinstance(release, dict):
        raise GateError("release API response is not an object")
    if release.get("id") != release_id:
        raise GateError(f"release ID mismatch: expected {release_id}, got {release.get('id')}")
    if release.get("tag_name") != release_tag:
        raise GateError(f"release tag mismatch: expected {release_tag}")
    if release.get("draft") is not True:
        raise GateError("release is not a draft — refusing to publish a non-draft release")
    if release.get("prerelease") is not (not is_versioned):
        raise GateError("release prerelease channel does not match the publication channel")
    if release.get("immutable") is not False:
        raise GateError("release is immutable at the publication boundary")

    assets = _fetch_release_assets(api_url, token, repository, release_id)

    _verify_release_assets(assets, expected_names, expected_digests)


def verify_published_release(
    api_url: str,
    token: str,
    repository: str,
    release_id: int,
    release_tag: str,
    is_versioned: bool,
    published: Any,
    expected_names: list[str],
    expected_digests: dict[str, str],
) -> None:
    """Re-check publication response and assets before reporting success."""
    if not isinstance(published, dict):
        raise GateError("publication PATCH response is not an object")
    if published.get("id") != release_id:
        raise GateError("published release ID mismatch")
    if published.get("tag_name") != release_tag:
        raise GateError("published release tag mismatch")
    if published.get("draft") is not False:
        raise GateError("publication PATCH did not clear draft flag")
    expected_prerelease = not is_versioned
    if published.get("prerelease") is not expected_prerelease:
        raise GateError("publication PATCH returned the wrong release channel")

    assets = _fetch_release_assets(api_url, token, repository, release_id)
    _verify_release_assets(assets, expected_names, expected_digests)


def verify_tag(
    token: str,
    release_tag: str,
    target_sha: str,
) -> None:
    auth = base64.b64encode(f"x-access-token:{token}".encode("utf-8")).decode("ascii")
    try:
        result = subprocess.run(
            [
                "git",
                "-c",
                f"http.https://github.com/.extraheader=AUTHORIZATION: basic {auth}",
                "ls-remote",
                "origin",
                f"refs/tags/{release_tag}",
            ],
            capture_output=True,
            text=True,
            timeout=30,
            env={
                **os.environ,
                "GIT_TERMINAL_PROMPT": "0",
            },
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise GateError(f"git ls-remote failed for tag {release_tag}: {error}") from error
    if result.returncode != 0:
        raise GateError(f"git ls-remote failed for tag {release_tag}: {result.stderr.strip()}")
    direct_ref = f"refs/tags/{release_tag}"
    peeled_ref = f"{direct_ref}^{{}}"
    refs: dict[str, str] = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) != 2 or fields[1] not in {direct_ref, peeled_ref}:
            raise GateError(f"malformed tag ref response for {release_tag}")
        sha, ref = fields
        if not SHA_PATTERN.fullmatch(sha) or ref in refs:
            raise GateError(f"malformed tag ref response for {release_tag}")
        refs[ref] = sha.lower()
    if direct_ref not in refs:
        raise GateError(f"missing tag ref for {release_tag}")
    remote_sha = refs.get(peeled_ref, refs[direct_ref])
    if remote_sha != target_sha.lower():
        raise GateError(
            f"tag {release_tag} points to {remote_sha}, expected {target_sha.lower()}"
        )


def verify_working(
    api_url: str,
    token: str,
    repository: str,
    target_sha: str,
) -> None:
    payload = _fetch_json(f"{api_url}/repos/{repository}/commits/Working", token)
    if not isinstance(payload, dict):
        raise GateError("Working commit response is not an object")
    working_sha = str(payload.get("sha", "")).lower()
    if not SHA_PATTERN.fullmatch(working_sha):
        raise GateError("Working commit response has no valid SHA")
    if working_sha != target_sha.lower():
        raise GateError(
            f"Working advanced to {working_sha}, expected {target_sha.lower()}"
        )


def _fetch_bounded_collection(
    url_without_page: str,
    token: str,
    item_key: str,
    label: str,
    max_items: int,
) -> list[dict[str, Any]]:
    """Read every item from a stable, bounded GitHub API collection."""

    items: list[dict[str, Any]] = []
    expected_total: int | None = None
    for page in range(1, (max_items // CI_PAGE_SIZE) + 1):
        payload = _fetch_json(f"{url_without_page}&page={page}", token)
        if not isinstance(payload, dict):
            raise GateError(f"{label} response is not an object")
        page_items = payload.get(item_key)
        total_count = payload.get("total_count")
        if (
            not isinstance(page_items, list)
            or any(not isinstance(item, dict) for item in page_items)
            or type(total_count) is not int
            or total_count < 0
            or total_count > max_items
            or len(page_items) > CI_PAGE_SIZE
        ):
            raise GateError(f"{label} response is malformed or exceeds {max_items} items")
        if expected_total is None:
            expected_total = total_count
        elif total_count != expected_total:
            raise GateError(f"{label} total_count changed during pagination")
        if len(items) + len(page_items) > total_count:
            raise GateError(f"{label} pages exceed the declared inventory")
        items.extend(page_items)
        if len(items) == total_count:
            return items
        if len(page_items) != CI_PAGE_SIZE:
            raise GateError(f"{label} inventory is incomplete: page ended before total_count")
    raise GateError(f"{label} inventory is incomplete or exceeds {max_items} items")


def _ci_run_identity(run: dict[str, Any]) -> tuple[int, int, int]:
    identity: list[int] = []
    for key, label in (
        ("id", "CI run ID"),
        ("run_number", "CI run number"),
        ("run_attempt", "CI run attempt"),
    ):
        value = run.get(key)
        if type(value) is not int or value < 1:
            raise GateError(f"{label} is not a positive integer")
        identity.append(value)
    return identity[0], identity[1], identity[2]


def _normalize_workflow_path(value: Any) -> str:
    text = str(value or "").split("@", 1)[0].replace("\\", "/")
    marker = "/.github/workflows/"
    offset = text.find(marker)
    return text[offset + 1 :] if offset >= 0 else text.removeprefix("./")


def _exact_repository(candidate: Any, repository_id: int, repository: str) -> bool:
    return (
        isinstance(candidate, dict)
        and candidate.get("id") == repository_id
        and candidate.get("full_name") == repository
    )


def _validate_ci_run_provenance(
    run: dict[str, Any],
    repository: str,
    repository_id: int | None,
) -> int:
    candidate_repository = run.get("repository")
    candidate_id = candidate_repository.get("id") if isinstance(candidate_repository, dict) else None
    if type(candidate_id) is not int or candidate_id < 1:
        raise GateError("eligible Build run identity is not the exact base-repository Working workflow")
    if repository_id is None:
        repository_id = candidate_id
    run_id, _, _ = _ci_run_identity(run)
    workflow_id = run.get("workflow_id")
    if (
        type(workflow_id) is not int
        or workflow_id < 1
        or run.get("name") != BUILD_WORKFLOW_NAME
        or _normalize_workflow_path(run.get("path")) != BUILD_WORKFLOW_PATH
        or run.get("head_branch") != WORKING_BRANCH
        or not _exact_repository(candidate_repository, repository_id, repository)
        or not _exact_repository(run.get("head_repository"), repository_id, repository)
        or run.get("html_url") != f"https://github.com/{repository}/actions/runs/{run_id}"
    ):
        raise GateError("eligible Build run identity is not the exact base-repository Working workflow")
    return repository_id


def verify_ci_gate(
    api_url: str,
    token: str,
    repository: str,
    target_sha: str,
) -> None:
    runs = _fetch_bounded_collection(
        f"{api_url}/repos/{repository}/actions/workflows/build.yml/runs"
        f"?head_sha={target_sha.lower()}&per_page={CI_PAGE_SIZE}",
        token,
        "workflow_runs",
        "Build workflow run inventory",
        MAX_CI_RUNS,
    )
    run_ids: set[int] = set()
    execution_keys: set[tuple[int, int]] = set()
    candidates: list[tuple[tuple[int, int, int], dict[str, Any]]] = []
    repository_id: int | None = None
    for run in runs:
        if str(run.get("head_sha", "")).lower() != target_sha.lower():
            raise GateError("same-commit Build inventory contains a different commit")
        run_id, run_number, run_attempt = _ci_run_identity(run)
        if run_id in run_ids or (run_number, run_attempt) in execution_keys:
            raise GateError("same-commit Build inventory contains a duplicate run identity")
        run_ids.add(run_id)
        execution_keys.add((run_number, run_attempt))
        if run.get("event") not in ALLOWED_CI_EVENTS:
            continue
        repository_id = _validate_ci_run_provenance(run, repository, repository_id)
        candidates.append(((run_number, run_attempt, run_id), run))

    if not candidates:
        raise GateError(
            f"no completed successful Required CI Gate from push/workflow_dispatch certifies {target_sha}"
        )
    _, newest = max(candidates, key=lambda item: item[0])
    if newest.get("status") != "completed" or newest.get("conclusion") != "success":
        raise GateError("newest eligible Build run is not a completed successful run")
    run_id, _, run_attempt = _ci_run_identity(newest)
    jobs = _fetch_bounded_collection(
        f"{api_url}/repos/{repository}/actions/runs/{run_id}/attempts/{run_attempt}/jobs"
        f"?per_page={CI_PAGE_SIZE}",
        token,
        "jobs",
        "Required CI Gate job inventory",
        MAX_CI_JOBS,
    )
    required_jobs = [job for job in jobs if job.get("name") == "Required CI Gate"]
    if len(required_jobs) != 1:
        raise GateError("newest eligible Build run does not have exactly one Required CI Gate job")
    required_job = required_jobs[0]
    if (
        required_job.get("status") != "completed"
        or required_job.get("conclusion") != "success"
    ):
        raise GateError("newest eligible Build run's Required CI Gate is not completed successful")



def acceptance_gate(
    api_url: str,
    token: str,
    repository: str,
    release_id: int,
    release_tag: str,
    target_sha: str,
    is_versioned: bool,
    expected_assets_file: Path,
    expected_digests_file: Path,
) -> dict[str, Any]:
    """Run every pre-publication check, then PATCH draft=false in one step."""

    _validate_release_tag(release_tag, is_versioned)
    expected_names = _read_expected_assets(expected_assets_file)
    expected_digests = _read_expected_digests(expected_digests_file, expected_names)

    verify_draft_release(
        api_url, token, repository, release_id, release_tag, is_versioned,
        expected_names, expected_digests,
    )
    verify_tag(token, release_tag, target_sha)
    verify_ci_gate(api_url, token, repository, target_sha)
    verify_working(api_url, token, repository, target_sha)

    patch_body: dict[str, Any] = {"draft": False}
    if is_versioned:
        patch_body["make_latest"] = "true"
        patch_body["prerelease"] = False
    else:
        patch_body["make_latest"] = "false"
        patch_body["prerelease"] = True

    published = _patch_json(
        f"{api_url}/repos/{repository}/releases/{release_id}",
        token,
        patch_body,
    )

    try:
        verify_published_release(
            api_url,
            token,
            repository,
            release_id,
            release_tag,
            is_versioned,
            published,
            expected_names,
            expected_digests,
        )
    except GateError as publication_error:
        try:
            redrafted = _patch_json(
                f"{api_url}/repos/{repository}/releases/{release_id}",
                token,
                {"draft": True, "make_latest": "false"},
                mark_attempt=False,
            )
            if (
                not isinstance(redrafted, dict)
                or redrafted.get("id") != release_id
                or redrafted.get("tag_name") != release_tag
                or redrafted.get("draft") is not True
            ):
                raise GateError("redraft PATCH did not prove a mutable draft release")
        except GateError as redraft_error:
            raise GateError(
                f"post-PATCH publication validation failed ({publication_error}); "
                f"redraft failed ({redraft_error})"
            ) from redraft_error
        raise GateError(
            f"post-PATCH publication validation failed; release was redrafted: "
            f"{publication_error}"
        ) from publication_error

    return published


def main() -> int:
    api_url = os.environ.get("GITHUB_API_URL", "https://api.github.com").rstrip("/")
    token = os.environ.get("GH_TOKEN", "")
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    release_id_raw = os.environ.get("RELEASE_ID", "")
    release_tag = os.environ.get("RELEASE_TAG", "")
    target_sha = os.environ.get("TARGET_SHA", "")
    is_versioned_raw = os.environ.get("IS_VERSIONED", "")
    expected_assets_file = os.environ.get("EXPECTED_ASSETS_FILE", "")
    expected_digests_file = os.environ.get("EXPECTED_DIGESTS_FILE", "")

    try:
        if not token:
            raise GateError("GH_TOKEN is required")
        if not repository or repository.count("/") != 1:
            raise GateError("GITHUB_REPOSITORY must be owner/name")
        if not SHA_PATTERN.fullmatch(target_sha):
            raise GateError("TARGET_SHA must be a 40-character hex commit ID")
        release_id = _require_positive_int(release_id_raw, "RELEASE_ID")
        if not release_tag:
            raise GateError("RELEASE_TAG is required")
        if is_versioned_raw not in ("true", "false"):
            raise GateError("IS_VERSIONED must be 'true' or 'false'")
        is_versioned = is_versioned_raw == "true"

        published = acceptance_gate(
            api_url=api_url,
            token=token,
            repository=repository,
            release_id=release_id,
            release_tag=release_tag,
            target_sha=target_sha,
            is_versioned=is_versioned,
            expected_assets_file=Path(expected_assets_file),
            expected_digests_file=Path(expected_digests_file),
        )
    except GateError as error:
        print(f"error: release acceptance gate FAILED: {error}", file=sys.stderr)
        return 1
    except (RuntimeError, OSError) as error:
        print(f"error: release acceptance gate infrastructure failure: {error}", file=sys.stderr)
        return 1

    print(
        f"Acceptance gate passed — published release {release_id} "
        f"(tag={release_tag}, sha={target_sha[:12]})."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
