#!/usr/bin/env python3
"""Best-effort, ID-bound recovery after an ambiguous release publication."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import sys
import time
from typing import Any, Protocol
from urllib import error, request


API_VERSION = "2026-03-10"
MAX_RESPONSE_BYTES = 4 * 1024 * 1024
REPOSITORY_RE = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
VERSION_TAG_RE = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+")


class RecoveryError(RuntimeError):
    """Raised when a known release cannot be proven private again."""


@dataclass(frozen=True)
class DurableAssetSnapshot:
    asset_id: int
    name: str
    download_count: int
    digest: str | None
    state: str


@dataclass(frozen=True)
class DurableRecoveryTarget:
    release_id: int
    release_tag: str
    expected_prerelease: bool
    action: str = "redraft"
    expected_assets: tuple[DurableAssetSnapshot, ...] = ()


def resolve_durable_recovery_target(
    pending: Any,
    *,
    assets: Any = None,
    run_id: int,
    run_attempt: int,
    source_sha: str,
) -> DurableRecoveryTarget | None:
    """Bind recovery to one exact staged ledger operation.

    A completed workflow for an older run must never hide a newer run's release,
    so operation/source mismatches are deliberate no-ops.  Once the durable
    operation matches, malformed release identity is a hard failure.
    """
    if run_id < 1 or run_attempt < 1 or re.fullmatch(r"[0-9a-f]{40}", source_sha) is None:
        raise RecoveryError("failed workflow identity is malformed")
    if pending is None:
        return None
    if (
        getattr(pending, "operation_id", None) != f"{run_id}:{run_attempt}"
        or getattr(pending, "source_sha", None) != source_sha
    ):
        return None
    phase = getattr(pending, "phase", None)
    if phase not in {"prepared", "staged"}:
        return None

    release_id = getattr(pending, "target_release_id", None)
    release_tag = getattr(pending, "target_tag", None)
    is_versioned = getattr(pending, "is_versioned", None)
    if not isinstance(release_id, int) or isinstance(release_id, bool) or release_id < 1:
        raise RecoveryError("matching staged operation has no durable release ID")
    if not isinstance(is_versioned, bool) or not isinstance(release_tag, str):
        raise RecoveryError("matching staged operation has malformed channel identity")
    if is_versioned:
        if VERSION_TAG_RE.fullmatch(release_tag) is None:
            raise RecoveryError("matching versioned operation has a malformed release tag")
    elif release_tag != "nightly":
        raise RecoveryError("matching nightly operation does not target the nightly tag")
    prepared_public = getattr(pending, "target_draft_at_prepare", None) is False
    if phase == "prepared":
        if is_versioned or not prepared_public:
            return None
        if not isinstance(assets, dict):
            raise RecoveryError("prepared nightly recovery has no durable asset inventory")
        prepared_ids = getattr(pending, "prepared_asset_ids", None)
        if not isinstance(prepared_ids, frozenset) or not prepared_ids:
            raise RecoveryError("prepared nightly recovery has no durable asset IDs")
        snapshots: list[DurableAssetSnapshot] = []
        for asset_id in sorted(prepared_ids):
            asset = assets.get(asset_id)
            if (
                asset is None
                or getattr(asset, "asset_id", None) != asset_id
                or getattr(asset, "release_id", None) != release_id
                or not isinstance(getattr(asset, "name", None), str)
                or not getattr(asset, "name", "")
                or type(getattr(asset, "download_count", None)) is not int
                or getattr(asset, "download_count", -1) < 0
                or not isinstance(getattr(asset, "digest", None), str)
                or re.fullmatch(r"sha256:[0-9a-f]{64}", asset.digest) is None
                or getattr(asset, "state", None) != "uploaded"
            ):
                raise RecoveryError("prepared nightly recovery asset snapshot is malformed")
            snapshots.append(
                DurableAssetSnapshot(
                    asset_id,
                    asset.name,
                    asset.download_count,
                    asset.digest,
                    asset.state,
                )
            )
        return DurableRecoveryTarget(
            release_id,
            release_tag,
            True,
            "restore-public-nightly",
            tuple(snapshots),
        )
    if is_versioned and prepared_public:
        return None
    return DurableRecoveryTarget(release_id, release_tag, not is_versioned)


class ReleaseApi(Protocol):
    def get_release(self, release_id: int) -> dict[str, Any]: ...

    def hide_release(self, release_id: int) -> None: ...

    def get_release_assets(self, release_id: int) -> list[dict[str, Any]]: ...

    def publish_release(self, release_id: int, *, prerelease: bool) -> None: ...


@dataclass(frozen=True)
class RecoveryResult:
    mutated: bool
    tag_matches: bool
    channel_matches: bool


def _verify_asset_snapshot(
    records: Any, expected: tuple[DurableAssetSnapshot, ...]
) -> None:
    if not isinstance(records, list) or len(records) != len(expected):
        raise RecoveryError("nightly release assets differ from the durable pre-hide snapshot")
    actual: dict[int, tuple[str, int, str, str]] = {}
    for record in records:
        if not isinstance(record, dict):
            raise RecoveryError("nightly release asset response is malformed")
        asset_id = record.get("id")
        name = record.get("name")
        count = record.get("download_count")
        digest = record.get("digest")
        state = record.get("state")
        if (
            type(asset_id) is not int
            or asset_id < 1
            or asset_id in actual
            or not isinstance(name, str)
            or not name
            or type(count) is not int
            or count < 0
            or not isinstance(digest, str)
            or re.fullmatch(r"sha256:[0-9a-f]{64}", digest) is None
            or state != "uploaded"
        ):
            raise RecoveryError("nightly release asset response is malformed")
        actual[asset_id] = (name, count, digest, state)
    durable = {
        asset.asset_id: (
            asset.name,
            asset.download_count,
            asset.digest,
            asset.state,
        )
        for asset in expected
    }
    if actual != durable:
        raise RecoveryError("nightly release assets differ from the durable pre-hide snapshot")


def recover_durable_publication(
    api: ReleaseApi,
    *,
    release_id: int,
    release_tag: str,
    expected_prerelease: bool,
    action: str,
    expected_assets: tuple[DurableAssetSnapshot, ...] = (),
    attempts: int = 3,
    sleep_seconds: float = 1.0,
) -> RecoveryResult:
    if action == "redraft":
        return recover_release_publication(
            api,
            release_id=release_id,
            release_tag=release_tag,
            expected_prerelease=expected_prerelease,
            attempts=attempts,
            sleep_seconds=sleep_seconds,
        )
    if (
        action != "restore-public-nightly"
        or release_tag != "nightly"
        or expected_prerelease is not True
        or not expected_assets
    ):
        raise RecoveryError("durable recovery action is malformed")
    if attempts < 1 or attempts > 10:
        raise RecoveryError("recovery attempts must be between 1 and 10")

    last_error = "nightly visibility recovery did not run"
    mutated = False
    for attempt in range(1, attempts + 1):
        try:
            current = _release_state(api.get_release(release_id), release_id, "current")
            if (
                current["tag_name"] != release_tag
                or current["prerelease"] is not expected_prerelease
                or current["immutable"]
            ):
                raise RecoveryError(
                    "nightly release identity changed after the durable pre-hide snapshot"
                )
            _verify_asset_snapshot(api.get_release_assets(release_id), expected_assets)
            if not current["draft"]:
                return RecoveryResult(mutated, True, True)
            mutated = True
            api.publish_release(release_id, prerelease=True)
            recovered = _release_state(
                api.get_release(release_id), release_id, "restored"
            )
            if (
                recovered["tag_name"] != release_tag
                or recovered["draft"]
                or recovered["prerelease"] is not True
                or recovered["immutable"]
            ):
                raise RecoveryError(
                    "fresh release GET did not prove the exact public nightly"
                )
            _verify_asset_snapshot(api.get_release_assets(release_id), expected_assets)
            return RecoveryResult(True, True, True)
        except RecoveryError as exc:
            last_error = str(exc)
            if "changed" in last_error or "differ" in last_error or "malformed" in last_error:
                break
        except Exception as exc:
            last_error = f"release API recovery failed: {exc}"
        if attempt < attempts and sleep_seconds > 0:
            time.sleep(sleep_seconds)
    raise RecoveryError(last_error)


def _release_state(record: Any, release_id: int, label: str) -> dict[str, Any]:
    if not isinstance(record, dict):
        raise RecoveryError(f"{label} release response is not an object")
    if type(record.get("id")) is not int or record["id"] != release_id:
        raise RecoveryError(f"{label} release ID is not the durable target")
    if not isinstance(record.get("tag_name"), str) or not record["tag_name"]:
        raise RecoveryError(f"{label} release tag is invalid")
    if type(record.get("draft")) is not bool:
        raise RecoveryError(f"{label} release draft state is invalid")
    if type(record.get("prerelease")) is not bool:
        raise RecoveryError(f"{label} release channel is invalid")
    if type(record.get("immutable")) is not bool:
        raise RecoveryError(f"{label} release immutability state is invalid")
    return record


def recover_release_publication(
    api: ReleaseApi,
    *,
    release_id: int,
    release_tag: str,
    expected_prerelease: bool,
    attempts: int = 3,
    sleep_seconds: float = 1.0,
) -> RecoveryResult:
    """Hide the durable release ID and prove the result with a fresh GET.

    The compensating mutation changes only visibility and latest-release status.
    Tag or channel drift is reported to the caller but is never overwritten.
    """

    if release_id < 1 or not release_tag:
        raise RecoveryError("recovery identity is malformed")
    if attempts < 1 or attempts > 10:
        raise RecoveryError("recovery attempts must be between 1 and 10")

    last_error = "recovery did not run"
    mutated = False
    for attempt in range(1, attempts + 1):
        try:
            current = _release_state(
                api.get_release(release_id), release_id, "current"
            )
            if current["draft"]:
                if current["immutable"]:
                    raise RecoveryError("a draft release unexpectedly reports immutable")
                return RecoveryResult(
                    mutated=mutated,
                    tag_matches=current["tag_name"] == release_tag,
                    channel_matches=current["prerelease"] is expected_prerelease,
                )
            if current["immutable"]:
                raise RecoveryError("the published release is immutable and cannot be hidden")

            # RELEASE_ID is durably checkpointed and cannot be reassigned. Hide
            # that exact object without overwriting a concurrent tag/channel edit.
            api.hide_release(release_id)
            mutated = True
            recovered = _release_state(
                api.get_release(release_id), release_id, "recovered"
            )
            if recovered["draft"] and not recovered["immutable"]:
                return RecoveryResult(
                    mutated=True,
                    tag_matches=recovered["tag_name"] == release_tag,
                    channel_matches=recovered["prerelease"] is expected_prerelease,
                )
            last_error = "fresh release GET did not prove a mutable draft"
        except RecoveryError as exc:
            last_error = str(exc)
            if "immutable" in last_error or "durable target" in last_error:
                break
        except Exception as exc:  # Network/client failures are retried, then reported.
            last_error = f"release API recovery failed: {exc}"
        if attempt < attempts and sleep_seconds > 0:
            time.sleep(sleep_seconds)
    raise RecoveryError(last_error)


class GitHubReleaseApi:
    def __init__(self, repository: str, token: str, *, timeout: float = 30.0):
        if REPOSITORY_RE.fullmatch(repository) is None:
            raise RecoveryError("GITHUB_REPOSITORY is malformed")
        if not token:
            raise RecoveryError("GH_TOKEN is missing")
        self._base = f"https://api.github.com/repos/{repository}/releases"
        self._token = token
        self._timeout = timeout

    def _request(
        self, method: str, release_id: int, payload: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        body = None if payload is None else json.dumps(payload).encode("utf-8")
        headers = {
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {self._token}",
            "User-Agent": "SparkEngine-release-recovery",
            "X-GitHub-Api-Version": API_VERSION,
        }
        if body is not None:
            headers["Content-Type"] = "application/json"
        api_request = request.Request(
            f"{self._base}/{release_id}", data=body, headers=headers, method=method
        )
        try:
            with request.urlopen(api_request, timeout=self._timeout) as response:
                raw = response.read(MAX_RESPONSE_BYTES + 1)
        except error.HTTPError as exc:
            detail = exc.read(4096).decode("utf-8", errors="replace")
            raise RecoveryError(
                f"GitHub release API returned HTTP {exc.code}: {detail}"
            ) from exc
        except error.URLError as exc:
            raise RecoveryError(f"GitHub release API request failed: {exc.reason}") from exc
        if len(raw) > MAX_RESPONSE_BYTES:
            raise RecoveryError("GitHub release API response is unexpectedly large")
        try:
            value = json.loads(raw)
        except (UnicodeError, json.JSONDecodeError) as exc:
            raise RecoveryError("GitHub release API returned invalid JSON") from exc
        if not isinstance(value, dict):
            raise RecoveryError("GitHub release API returned a non-object response")
        return value

    def get_release(self, release_id: int) -> dict[str, Any]:
        return self._request("GET", release_id)

    def hide_release(self, release_id: int) -> None:
        self._request(
            "PATCH", release_id, {"draft": True, "make_latest": "false"}
        )

    def get_release_assets(self, release_id: int) -> list[dict[str, Any]]:
        headers = {
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {self._token}",
            "User-Agent": "SparkEngine-release-recovery",
            "X-GitHub-Api-Version": API_VERSION,
        }
        api_request = request.Request(
            f"{self._base}/{release_id}/assets?per_page=100",
            headers=headers,
            method="GET",
        )
        try:
            with request.urlopen(api_request, timeout=self._timeout) as response:
                raw = response.read(MAX_RESPONSE_BYTES + 1)
        except error.HTTPError as exc:
            detail = exc.read(4096).decode("utf-8", errors="replace")
            raise RecoveryError(
                f"GitHub release asset API returned HTTP {exc.code}: {detail}"
            ) from exc
        except error.URLError as exc:
            raise RecoveryError(
                f"GitHub release asset API request failed: {exc.reason}"
            ) from exc
        if len(raw) > MAX_RESPONSE_BYTES:
            raise RecoveryError("GitHub release asset API response is unexpectedly large")
        try:
            value = json.loads(raw)
        except (UnicodeError, json.JSONDecodeError) as exc:
            raise RecoveryError("GitHub release asset API returned invalid JSON") from exc
        if not isinstance(value, list) or len(value) >= 100:
            raise RecoveryError("GitHub release asset API returned an unbounded inventory")
        return value

    def publish_release(self, release_id: int, *, prerelease: bool) -> None:
        self._request(
            "PATCH",
            release_id,
            {"draft": False, "prerelease": prerelease, "make_latest": "false"},
        )


def _parse_boolean(value: str) -> bool:
    if value == "true":
        return True
    if value == "false":
        return False
    raise argparse.ArgumentTypeError("expected true or false")


def _load_counter_state(path: Path) -> Any:
    try:
        metadata = os.lstat(path)
    except OSError as exc:
        raise RecoveryError(f"cannot inspect durable counter state: {exc}") from exc
    if not stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        raise RecoveryError("durable counter state must be a regular file, not a link")
    ledger_path = Path(__file__).resolve().with_name("download_counter_ledger.py")
    spec = importlib.util.spec_from_file_location("_spark_download_counter_ledger", ledger_path)
    if spec is None or spec.loader is None:
        raise RecoveryError("cannot load the trusted counter-ledger validator")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
        state = module.load_state(path)
    except Exception as exc:
        raise RecoveryError(f"durable counter state is invalid: {exc}") from exc
    if not isinstance(state, module.CounterState):
        raise RecoveryError("durable counter state must use the current schema")
    return state


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--release-id", type=int)
    parser.add_argument("--release-tag")
    parser.add_argument("--expected-prerelease", type=_parse_boolean)
    parser.add_argument("--state-file", type=Path)
    parser.add_argument("--run-id", type=int)
    parser.add_argument("--run-attempt", type=int)
    parser.add_argument("--source-sha")
    parser.add_argument("--attempts", type=int, default=3)
    args = parser.parse_args(argv)
    try:
        if args.state_file is not None:
            if any(value is not None for value in (args.release_id, args.release_tag, args.expected_prerelease)):
                raise RecoveryError("state-file recovery cannot also accept direct release identity")
            if args.run_id is None or args.run_attempt is None or args.source_sha is None:
                raise RecoveryError("state-file recovery requires exact run ID, attempt, and source SHA")
            state = _load_counter_state(args.state_file)
            target = resolve_durable_recovery_target(
                state.pending,
                assets=state.assets,
                run_id=args.run_id,
                run_attempt=args.run_attempt,
                source_sha=args.source_sha.lower(),
            )
            if target is None:
                print("No exact staged publication belongs to this failed workflow run.")
                return 0
            release_id = target.release_id
            release_tag = target.release_tag
            expected_prerelease = target.expected_prerelease
            recovery_action = target.action
            expected_assets = target.expected_assets
        else:
            if any(value is not None for value in (args.run_id, args.run_attempt, args.source_sha)):
                raise RecoveryError("direct recovery cannot accept workflow-run identity")
            if args.release_id is None or args.release_tag is None or args.expected_prerelease is None:
                raise RecoveryError("direct recovery requires release ID, tag, and channel")
            release_id = args.release_id
            release_tag = args.release_tag
            expected_prerelease = args.expected_prerelease
            recovery_action = "redraft"
            expected_assets = ()
        api = GitHubReleaseApi(args.repository, os.environ.get("GH_TOKEN", ""))
        result = recover_durable_publication(
            api,
            release_id=release_id,
            release_tag=release_tag,
            expected_prerelease=expected_prerelease,
            action=recovery_action,
            expected_assets=expected_assets,
            attempts=args.attempts,
        )
    except RecoveryError as exc:
        print(f"::error::Automatic release redraft failed: {exc}")
        return 1
    if not result.tag_matches or not result.channel_matches:
        print(
            f"::warning::Release {release_id} was hidden, but its tag or channel "
            "drifted and requires manual review."
        )
    if recovery_action == "restore-public-nightly":
        action = "restored" if result.mutated else "was already"
        print(f"Release {release_id} {action} in public nightly state.")
    else:
        action = "returned" if result.mutated else "was already"
        print(f"Release {release_id} {action} in draft state.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
