#!/usr/bin/env python3
"""Crash-consistent high-water accounting for GitHub release downloads."""

from __future__ import annotations

import argparse
import json
import os
import re
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, replace
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable


API_ROOT = "https://api.github.com"
STATE_SCHEMA_VERSION = 2
PAGE_SIZE = 100
MAX_RELEASE_PAGES = 100
MAX_RELEASES = 500
MAX_ASSET_PAGES = 100
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
VERSION_TAG_RE = re.compile(r"^v[0-9]+\.[0-9]+\.[0-9]+$")
SHA_RE = re.compile(r"^[0-9a-fA-F]{40}$")
OPERATION_RE = re.compile(r"^[0-9]+:[0-9]+$")
DIGEST_RE = re.compile(r"^sha256:[0-9a-fA-F]{64}$")
PENDING_PHASES = frozenset({"prepared", "staged"})
EXACT_CI_EVIDENCE_ASSET = "SparkEngine-Exact-CI-Evidence.json"


class CounterError(RuntimeError):
    """The counters cannot be proven complete and must not be published."""


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CounterError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _decode_json(payload: bytes, label: str) -> Any:
    try:
        return json.loads(
            payload.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys
        )
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as exc:
        raise CounterError(f"{label} is not valid UTF-8 JSON: {exc}") from exc


def _require_keys(
    value: dict[str, Any],
    *,
    allowed: set[str],
    required: set[str],
    label: str,
) -> None:
    missing = required - value.keys()
    unknown = value.keys() - allowed
    if missing:
        raise CounterError(f"{label} is missing keys: {', '.join(sorted(missing))}")
    if unknown:
        raise CounterError(f"{label} has unknown keys: {', '.join(sorted(unknown))}")


def _nonnegative_integer(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise CounterError(f"{label} must be a non-negative integer")
    return value


def _positive_integer(value: Any, label: str) -> int:
    result = _nonnegative_integer(value, label)
    if result == 0:
        raise CounterError(f"{label} must be positive")
    return result


def _decimal_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value or not value.isdigit():
        raise CounterError(f"{label} must contain only decimal digits")
    return value


def _timestamp(value: Any, label: str) -> str:
    if not isinstance(value, str):
        raise CounterError(f"{label} must be a UTC timestamp string")
    try:
        datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ")
    except ValueError as exc:
        raise CounterError(f"{label} must use YYYY-MM-DDTHH:MM:SSZ") from exc
    return value


def _utc_now(now: Callable[[], datetime]) -> str:
    value = now()
    if value.tzinfo is None:
        raise CounterError("the counter clock must return a timezone-aware datetime")
    return value.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _is_installer(name: str) -> bool:
    lowered = name.lower()
    return (
        name.startswith("SparkInstaller-")
        or lowered.endswith("-installer.exe")
        or lowered.endswith("-runtime.exe")
        or lowered.endswith(".msi")
    )


@dataclass(frozen=True)
class AssetRecord:
    asset_id: int
    release_id: int
    name: str
    download_count: int
    digest: str | None = None
    state: str = "uploaded"


@dataclass(frozen=True)
class ReleaseRecord:
    release_id: int
    tag_name: str
    assets: tuple[AssetRecord, ...]
    draft: bool = False
    prerelease: bool = False


@dataclass(frozen=True)
class Inventory:
    releases: tuple[ReleaseRecord, ...]

    @property
    def assets(self) -> dict[int, AssetRecord]:
        return {
            asset.asset_id: asset
            for release in self.releases
            for asset in release.assets
        }

    def release_by_tag(self, tag_name: str) -> ReleaseRecord | None:
        matches = [release for release in self.releases if release.tag_name == tag_name]
        if len(matches) > 1:
            raise CounterError(f"GitHub returned duplicate releases for tag {tag_name}")
        return matches[0] if matches else None


@dataclass(frozen=True)
class EvidenceCleanupIntent:
    asset_id: int
    digest: str | None
    state: str
    download_count: int


@dataclass(frozen=True)
class DraftAssetCleanupIntent:
    asset_id: int
    name: str
    digest: str | None
    state: str
    download_count: int


@dataclass(frozen=True)
class EvidenceReplacementIntent:
    old_asset_id: int
    old_digest: str
    expected_digest: str
    cleanup: EvidenceCleanupIntent | None = None


@dataclass(frozen=True)
class PendingPublication:
    operation_id: str
    target_tag: str
    is_versioned: bool
    source_sha: str
    phase: str
    target_release_id: int | None
    prepared_asset_ids: frozenset[int]
    prepared_at: str
    target_draft_at_prepare: bool | None
    evidence_replacement: EvidenceReplacementIntent | None = None
    draft_cleanup: tuple[DraftAssetCleanupIntent, ...] = ()


@dataclass(frozen=True)
class CounterState:
    base_total: int
    base_installer_total: int
    total: int
    installer_total: int
    assets: dict[int, AssetRecord]
    live_asset_ids: frozenset[int]
    pending: PendingPublication | None
    updated: str


@dataclass(frozen=True)
class LegacyState:
    previous_total: int
    previous_installer_total: int
    base_total: int
    base_installer_total: int


LoadedState = CounterState | LegacyState


def _derived_totals(
    base_total: int, base_installer_total: int, assets: Iterable[AssetRecord]
) -> tuple[int, int]:
    total = base_total
    installer_total = base_installer_total
    for asset in assets:
        total += asset.download_count
        if _is_installer(asset.name):
            installer_total += asset.download_count
    return total, installer_total


def _parse_integer_set(value: Any, label: str) -> frozenset[int]:
    if not isinstance(value, list):
        raise CounterError(f"{label} must be an array")
    result: set[int] = set()
    for index, item in enumerate(value):
        parsed = _positive_integer(item, f"{label}[{index}]")
        if parsed in result:
            raise CounterError(f"{label} contains duplicate asset ID {parsed}")
        result.add(parsed)
    return frozenset(result)


def _validate_tag(tag: Any, is_versioned: bool, label: str) -> str:
    if not isinstance(tag, str):
        raise CounterError(f"{label} must be a string")
    if is_versioned:
        if not VERSION_TAG_RE.fullmatch(tag):
            raise CounterError(f"{label} must have the form vMAJOR.MINOR.PATCH")
    elif tag != "nightly":
        raise CounterError(f"{label} must be nightly for a rolling publication")
    return tag


def _parse_asset(value: Any, label: str) -> AssetRecord:
    if not isinstance(value, dict):
        raise CounterError(f"{label} must be an object")
    _require_keys(
        value,
        allowed={"id", "releaseId", "name", "downloadCount", "digest", "state"},
        required={"id", "releaseId", "name", "downloadCount"},
        label=label,
    )
    name = value["name"]
    if not isinstance(name, str) or not name or "\n" in name or "\r" in name:
        raise CounterError(f"{label}.name must be a non-empty single-line string")
    digest = value.get("digest")
    if digest is not None:
        if not isinstance(digest, str) or not DIGEST_RE.fullmatch(digest):
            raise CounterError(f"{label}.digest is invalid")
        digest = digest.lower()
    state = value.get("state", "uploaded")
    if state not in {"starter", "uploaded"}:
        raise CounterError(f"{label}.state must be starter or uploaded")
    if state == "starter" and digest is not None:
        raise CounterError(f"{label}.starter assets cannot have a digest")
    return AssetRecord(
        _positive_integer(value["id"], f"{label}.id"),
        _positive_integer(value["releaseId"], f"{label}.releaseId"),
        name,
        _nonnegative_integer(value["downloadCount"], f"{label}.downloadCount"),
        digest,
        state,
    )


def _parse_pending(value: Any, assets: dict[int, AssetRecord]) -> PendingPublication | None:
    if value is None:
        return None
    if not isinstance(value, dict):
        raise CounterError("state.pending must be an object or null")
    keys = {
        "operationId", "targetTag", "isVersioned", "sourceSha",
        "phase", "targetReleaseId", "preparedAssetIds", "preparedAt",
        "targetDraftAtPrepare", "evidenceReplacement", "draftCleanup",
    }
    _require_keys(
        value,
        allowed=keys,
        required=keys - {"evidenceReplacement", "draftCleanup"},
        label="state.pending",
    )
    operation_id = value["operationId"]
    if not isinstance(operation_id, str) or not OPERATION_RE.fullmatch(operation_id):
        raise CounterError("state.pending.operationId must have the form RUN_ID:RUN_ATTEMPT")
    is_versioned = value["isVersioned"]
    if not isinstance(is_versioned, bool):
        raise CounterError("state.pending.isVersioned must be boolean")
    target_tag = _validate_tag(value["targetTag"], is_versioned, "state.pending.targetTag")
    source_sha = value["sourceSha"]
    if not isinstance(source_sha, str) or not SHA_RE.fullmatch(source_sha):
        raise CounterError("state.pending.sourceSha must be a full commit SHA")
    phase = value["phase"]
    if not isinstance(phase, str) or phase not in PENDING_PHASES:
        raise CounterError(
            "state.pending.phase must be one of: " + ", ".join(sorted(PENDING_PHASES))
        )
    release_id_value = value["targetReleaseId"]
    release_id = None if release_id_value is None else _positive_integer(
        release_id_value, "state.pending.targetReleaseId"
    )
    prepared = _parse_integer_set(value["preparedAssetIds"], "state.pending.preparedAssetIds")
    if not prepared.issubset(assets):
        raise CounterError("state.pending.preparedAssetIds contains an unknown asset ID")
    if release_id is None and prepared:
        raise CounterError("a missing target release cannot have prepared asset IDs")
    if any(assets[asset_id].release_id != release_id for asset_id in prepared):
        raise CounterError("state.pending contains an asset from another release")
    target_draft_at_prepare = value["targetDraftAtPrepare"]
    if target_draft_at_prepare is not None and not isinstance(
        target_draft_at_prepare, bool
    ):
        raise CounterError(
            "state.pending.targetDraftAtPrepare must be null or boolean"
        )
    if release_id is None and target_draft_at_prepare is not None:
        raise CounterError(
            "state.pending.targetDraftAtPrepare must be null when the target release was absent"
        )
    if (
        phase == "prepared"
        and release_id is not None
        and target_draft_at_prepare is None
    ):
        raise CounterError(
            "prepared state.pending.targetDraftAtPrepare must describe the existing release"
        )
    raw_replacement = value.get("evidenceReplacement")
    replacement_intent: EvidenceReplacementIntent | None = None
    if raw_replacement is not None:
        if not isinstance(raw_replacement, dict):
            raise CounterError("state.pending.evidenceReplacement must be an object or null")
        replacement_keys = {"oldAssetId", "oldDigest", "expectedDigest", "cleanup"}
        _require_keys(
            raw_replacement,
            allowed=replacement_keys,
            required=replacement_keys - {"cleanup"},
            label="state.pending.evidenceReplacement",
        )
        old_asset_id = _positive_integer(
            raw_replacement["oldAssetId"],
            "state.pending.evidenceReplacement.oldAssetId",
        )
        old_digest = raw_replacement["oldDigest"]
        expected_digest = raw_replacement["expectedDigest"]
        if not isinstance(old_digest, str) or not DIGEST_RE.fullmatch(old_digest):
            raise CounterError("state.pending.evidenceReplacement.oldDigest is invalid")
        if not isinstance(expected_digest, str) or not DIGEST_RE.fullmatch(expected_digest):
            raise CounterError("state.pending.evidenceReplacement.expectedDigest is invalid")
        cleanup: EvidenceCleanupIntent | None = None
        raw_cleanup = raw_replacement.get("cleanup")
        if raw_cleanup is not None:
            if not isinstance(raw_cleanup, dict):
                raise CounterError(
                    "state.pending.evidenceReplacement.cleanup must be an object or null"
                )
            cleanup_keys = {"assetId", "digest", "state", "downloadCount"}
            _require_keys(
                raw_cleanup,
                allowed=cleanup_keys,
                required=cleanup_keys,
                label="state.pending.evidenceReplacement.cleanup",
            )
            cleanup_id = _positive_integer(
                raw_cleanup["assetId"],
                "state.pending.evidenceReplacement.cleanup.assetId",
            )
            cleanup_digest = raw_cleanup["digest"]
            if cleanup_digest is not None:
                if not isinstance(cleanup_digest, str) or not DIGEST_RE.fullmatch(
                    cleanup_digest
                ):
                    raise CounterError(
                        "state.pending.evidenceReplacement.cleanup.digest is invalid"
                    )
                cleanup_digest = cleanup_digest.lower()
            cleanup_state = raw_cleanup["state"]
            if cleanup_state not in {"starter", "uploaded"}:
                raise CounterError(
                    "state.pending.evidenceReplacement.cleanup.state must be starter or uploaded"
                )
            if cleanup_state == "starter" and cleanup_digest is not None:
                raise CounterError(
                    "state.pending.evidenceReplacement.cleanup starter cannot have a digest"
                )
            if cleanup_id == old_asset_id or cleanup_id in prepared:
                raise CounterError(
                    "state.pending.evidenceReplacement.cleanup must identify a later asset"
                )
            cleanup_asset = assets.get(cleanup_id)
            cleanup_count = _nonnegative_integer(
                raw_cleanup["downloadCount"],
                "state.pending.evidenceReplacement.cleanup.downloadCount",
            )
            if (
                cleanup_asset is None
                or cleanup_asset.release_id != release_id
                or cleanup_asset.name != EXACT_CI_EVIDENCE_ASSET
                or cleanup_asset.digest != cleanup_digest
                or cleanup_asset.state != cleanup_state
                or cleanup_asset.download_count != cleanup_count
            ):
                raise CounterError(
                    "state.pending.evidenceReplacement.cleanup is not bound to its ledger asset"
                )
            cleanup = EvidenceCleanupIntent(
                cleanup_id, cleanup_digest, cleanup_state, cleanup_count
            )
        if (
            not is_versioned
            or phase != "prepared"
            or release_id is None
            or target_draft_at_prepare is not True
            or old_asset_id not in prepared
            or assets[old_asset_id].release_id != release_id
            or assets[old_asset_id].name != EXACT_CI_EVIDENCE_ASSET
        ):
            raise CounterError("state.pending.evidenceReplacement is not bound to the prepared stable draft")
        replacement_intent = EvidenceReplacementIntent(
            old_asset_id, old_digest.lower(), expected_digest.lower(), cleanup
        )
    raw_draft_cleanup = value.get("draftCleanup", [])
    if not isinstance(raw_draft_cleanup, list):
        raise CounterError("state.pending.draftCleanup must be an array")
    draft_cleanup: list[DraftAssetCleanupIntent] = []
    cleanup_ids: set[int] = set()
    cleanup_names: set[str] = set()
    for index, raw_cleanup in enumerate(raw_draft_cleanup):
        label = f"state.pending.draftCleanup[{index}]"
        if not isinstance(raw_cleanup, dict):
            raise CounterError(f"{label} must be an object")
        cleanup_keys = {"assetId", "name", "digest", "state", "downloadCount"}
        _require_keys(
            raw_cleanup,
            allowed=cleanup_keys,
            required=cleanup_keys,
            label=label,
        )
        cleanup_id = _positive_integer(raw_cleanup["assetId"], f"{label}.assetId")
        cleanup_name = raw_cleanup["name"]
        if (
            not isinstance(cleanup_name, str)
            or not cleanup_name
            or cleanup_name != Path(cleanup_name).name
            or "\n" in cleanup_name
            or "\r" in cleanup_name
        ):
            raise CounterError(f"{label}.name must be a plain filename")
        cleanup_digest = raw_cleanup["digest"]
        if cleanup_digest is not None:
            if not isinstance(cleanup_digest, str) or not DIGEST_RE.fullmatch(
                cleanup_digest
            ):
                raise CounterError(f"{label}.digest is invalid")
            cleanup_digest = cleanup_digest.lower()
        cleanup_state = raw_cleanup["state"]
        if cleanup_state not in {"starter", "uploaded"}:
            raise CounterError(f"{label}.state must be starter or uploaded")
        if cleanup_state == "starter" and cleanup_digest is not None:
            raise CounterError(f"{label} starter cannot have a digest")
        cleanup_count = _nonnegative_integer(
            raw_cleanup["downloadCount"], f"{label}.downloadCount"
        )
        if cleanup_id in cleanup_ids:
            raise CounterError(
                f"state.pending.draftCleanup repeats asset ID {cleanup_id}"
            )
        if cleanup_name in cleanup_names:
            raise CounterError(
                f"state.pending.draftCleanup repeats asset name {cleanup_name}"
            )
        if cleanup_id in prepared:
            raise CounterError(
                "state.pending.draftCleanup cannot authorize a prepared asset"
            )
        cleanup_asset = assets.get(cleanup_id)
        if (
            cleanup_asset is None
            or cleanup_asset.release_id != release_id
            or cleanup_asset.name != cleanup_name
            or cleanup_asset.digest != cleanup_digest
            or cleanup_asset.state != cleanup_state
            or cleanup_asset.download_count != cleanup_count
        ):
            raise CounterError(f"{label} is not bound to its ledger asset")
        cleanup_ids.add(cleanup_id)
        cleanup_names.add(cleanup_name)
        draft_cleanup.append(
            DraftAssetCleanupIntent(
                cleanup_id,
                cleanup_name,
                cleanup_digest,
                cleanup_state,
                cleanup_count,
            )
        )
    if draft_cleanup and (
        not is_versioned
        or phase != "prepared"
        or release_id is None
        or target_draft_at_prepare is not True
    ):
        raise CounterError(
            "state.pending.draftCleanup is valid only for a prepared stable draft"
        )
    if replacement_intent is not None and draft_cleanup:
        raise CounterError(
            "state.pending cannot mix draft cleanup with exact evidence replacement"
        )
    return PendingPublication(
        operation_id=operation_id,
        target_tag=target_tag,
        is_versioned=is_versioned,
        source_sha=source_sha.lower(),
        phase=phase,
        target_release_id=release_id,
        prepared_asset_ids=prepared,
        prepared_at=_timestamp(value["preparedAt"], "state.pending.preparedAt"),
        target_draft_at_prepare=target_draft_at_prepare,
        evidence_replacement=replacement_intent,
        draft_cleanup=tuple(draft_cleanup),
    )


def _parse_v2_state(value: dict[str, Any]) -> CounterState:
    keys = {
        "schemaVersion", "initialized", "baseTotal", "baseInstallerTotal",
        "total", "installerTotal", "assets", "liveAssetIds", "pending", "updated",
    }
    _require_keys(value, allowed=keys, required=keys, label="counter state")
    if value["initialized"] is not True:
        raise CounterError("state.initialized must be true")
    base_total = _nonnegative_integer(value["baseTotal"], "state.baseTotal")
    base_installer = _nonnegative_integer(
        value["baseInstallerTotal"], "state.baseInstallerTotal"
    )
    if base_installer > base_total:
        raise CounterError("state.baseInstallerTotal cannot exceed state.baseTotal")
    raw_assets = value["assets"]
    if not isinstance(raw_assets, list):
        raise CounterError("state.assets must be an array")
    assets: dict[int, AssetRecord] = {}
    for index, raw_asset in enumerate(raw_assets):
        asset = _parse_asset(raw_asset, f"state.assets[{index}]")
        if asset.asset_id in assets:
            raise CounterError(f"state.assets contains duplicate asset ID {asset.asset_id}")
        assets[asset.asset_id] = asset
    live = _parse_integer_set(value["liveAssetIds"], "state.liveAssetIds")
    if not live.issubset(assets):
        raise CounterError("state.liveAssetIds contains an unknown asset ID")
    total = _nonnegative_integer(value["total"], "state.total")
    installer_total = _nonnegative_integer(value["installerTotal"], "state.installerTotal")
    derived_total, derived_installer = _derived_totals(base_total, base_installer, assets.values())
    if total != derived_total:
        raise CounterError(f"state.total is {total}, but the ledger derives {derived_total}")
    if installer_total != derived_installer:
        raise CounterError(
            f"state.installerTotal is {installer_total}, but the ledger derives {derived_installer}"
        )
    if installer_total > total:
        raise CounterError("state.installerTotal cannot exceed state.total")
    pending = _parse_pending(value["pending"], assets)
    if pending is not None:
        allowed_missing_ids: set[int] = set()
        if pending.evidence_replacement is not None:
            allowed_missing_ids.add(pending.evidence_replacement.old_asset_id)
            if pending.evidence_replacement.cleanup is not None:
                allowed_missing_ids.add(pending.evidence_replacement.cleanup.asset_id)
        allowed_missing_ids.update(
            cleanup.asset_id for cleanup in pending.draft_cleanup
        )
        allowed_missing = frozenset(allowed_missing_ids)
        if not (pending.prepared_asset_ids - allowed_missing).issubset(live):
            raise CounterError("pending assets must have been live when publication was prepared")
    return CounterState(
        base_total, base_installer, total, installer_total, assets, live, pending,
        _timestamp(value["updated"], "state.updated"),
    )


def _parse_legacy_state(value: dict[str, Any]) -> LegacyState:
    allowed = {
        "total", "archivedNightly", "archivedNightlyInstallers",
        "installerTotal", "updated", "snapshotRunId",
    }
    _require_keys(value, allowed=allowed, required={"total"}, label="legacy counter state")
    previous_total = _nonnegative_integer(value["total"], "legacy state.total")
    has_archived = "archivedNightly" in value
    base_total = _nonnegative_integer(
        value.get("archivedNightly", previous_total), "legacy state.archivedNightly"
    )
    base_installer = _nonnegative_integer(
        value.get("archivedNightlyInstallers", 0),
        "legacy state.archivedNightlyInstallers",
    )
    previous_installer = _nonnegative_integer(
        value.get("installerTotal", base_installer), "legacy state.installerTotal"
    )
    if has_archived and base_total > previous_total:
        raise CounterError("legacy archivedNightly cannot exceed total")
    if base_installer > base_total:
        raise CounterError("legacy archivedNightlyInstallers cannot exceed archivedNightly")
    if previous_installer > previous_total:
        raise CounterError("legacy installerTotal cannot exceed total")
    if not isinstance(value.get("snapshotRunId", ""), str):
        raise CounterError("legacy snapshotRunId must be a string")
    if "updated" in value and not isinstance(value["updated"], str):
        raise CounterError("legacy updated must be a string")
    return LegacyState(previous_total, previous_installer, base_total, base_installer)


def load_state(path: Path, *, initialize: bool = False) -> LoadedState:
    if not path.exists():
        if initialize:
            return LegacyState(0, 0, 0, 0)
        raise CounterError(
            f"counter state is missing: {path}; use --initialize only for first bootstrap"
        )
    if not path.is_file():
        raise CounterError(f"counter state is not a regular file: {path}")
    try:
        value = _decode_json(path.read_bytes(), str(path))
    except OSError as exc:
        raise CounterError(f"cannot read counter state {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise CounterError("counter state must be a JSON object")
    schema = value.get("schemaVersion")
    if schema is None:
        return _parse_legacy_state(value)
    if schema != STATE_SCHEMA_VERSION:
        raise CounterError(f"unsupported counter-state schemaVersion: {schema!r}")
    return _parse_v2_state(value)


def _state_payload(state: CounterState) -> dict[str, Any]:
    pending: dict[str, Any] | None = None
    if state.pending is not None:
        pending = {
            "operationId": state.pending.operation_id,
            "targetTag": state.pending.target_tag,
            "isVersioned": state.pending.is_versioned,
            "sourceSha": state.pending.source_sha,
            "phase": state.pending.phase,
            "targetReleaseId": state.pending.target_release_id,
            "preparedAssetIds": sorted(state.pending.prepared_asset_ids),
            "preparedAt": state.pending.prepared_at,
            "targetDraftAtPrepare": state.pending.target_draft_at_prepare,
            "evidenceReplacement": (
                None
                if state.pending.evidence_replacement is None
                else {
                    "oldAssetId": state.pending.evidence_replacement.old_asset_id,
                    "oldDigest": state.pending.evidence_replacement.old_digest,
                    "expectedDigest": state.pending.evidence_replacement.expected_digest,
                    "cleanup": (
                        None
                        if state.pending.evidence_replacement.cleanup is None
                        else {
                            "assetId": state.pending.evidence_replacement.cleanup.asset_id,
                            "digest": state.pending.evidence_replacement.cleanup.digest,
                            "state": state.pending.evidence_replacement.cleanup.state,
                            "downloadCount": state.pending.evidence_replacement.cleanup.download_count,
                        }
                    ),
                }
            ),
            "draftCleanup": [
                {
                    "assetId": cleanup.asset_id,
                    "name": cleanup.name,
                    "digest": cleanup.digest,
                    "state": cleanup.state,
                    "downloadCount": cleanup.download_count,
                }
                for cleanup in state.pending.draft_cleanup
            ],
        }
    return {
        "schemaVersion": STATE_SCHEMA_VERSION,
        "initialized": True,
        "baseTotal": state.base_total,
        "baseInstallerTotal": state.base_installer_total,
        "total": state.total,
        "installerTotal": state.installer_total,
        "assets": [
            {
                "id": asset.asset_id,
                "releaseId": asset.release_id,
                "name": asset.name,
                "downloadCount": asset.download_count,
                "digest": asset.digest,
                "state": asset.state,
            }
            for asset in sorted(state.assets.values(), key=lambda item: item.asset_id)
        ],
        "liveAssetIds": sorted(state.live_asset_ids),
        "pending": pending,
        "updated": state.updated,
    }


@dataclass(frozen=True)
class ApiResponse:
    data: Any
    next_url: str | None


def _next_link(header: str | None) -> str | None:
    if not header:
        return None
    next_url: str | None = None
    for part in header.split(","):
        match = re.fullmatch(r"\s*<([^>]+)>\s*((?:;\s*[^;]+)*)\s*", part)
        if not match:
            raise CounterError(f"malformed GitHub pagination Link header: {header}")
        relations: list[str] = []
        for raw_parameter in match.group(2).split(";"):
            parameter = raw_parameter.strip()
            if not parameter:
                continue
            if "=" not in parameter:
                raise CounterError(f"malformed GitHub pagination parameter: {parameter}")
            name, parameter_value = parameter.split("=", 1)
            if name.strip().lower() == "rel":
                parameter_value = parameter_value.strip()
                if len(parameter_value) < 2 or parameter_value[0] != '"' or parameter_value[-1] != '"':
                    raise CounterError("GitHub pagination rel parameter must be quoted")
                relations.extend(parameter_value[1:-1].split())
        if "next" in relations:
            if next_url is not None:
                raise CounterError("GitHub pagination contains multiple next links")
            next_url = match.group(1)
    return next_url


class GitHubApi:
    def __init__(self, token: str):
        if not token:
            raise CounterError("GH_TOKEN is required")
        self._token = token

    @staticmethod
    def _validated_url(path_or_url: str) -> str:
        url = urllib.parse.urljoin(f"{API_ROOT}/", path_or_url)
        parsed = urllib.parse.urlparse(url)
        if (
            parsed.scheme != "https" or parsed.netloc != "api.github.com"
            or parsed.username is not None or parsed.password is not None or parsed.fragment
        ):
            raise CounterError(f"refusing an unexpected GitHub API URL: {url}")
        return url

    def get(self, path_or_url: str) -> ApiResponse:
        url = self._validated_url(path_or_url)
        request = urllib.request.Request(
            url,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {self._token}",
                "User-Agent": "SparkEngine-download-counter",
                "X-GitHub-Api-Version": "2022-11-28",
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                status, payload = response.status, response.read()
                link = response.headers.get("Link")
        except urllib.error.HTTPError as exc:
            detail = exc.read(1024).decode("utf-8", errors="replace")
            raise CounterError(
                f"GitHub API request failed with HTTP {exc.code} for {url}: {detail}"
            ) from exc
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            raise CounterError(f"GitHub API request failed for {url}: {exc}") from exc
        if status != 200:
            raise CounterError(f"GitHub API returned HTTP {status} for {url}")
        return ApiResponse(_decode_json(payload, url), _next_link(link))


def _validate_next_page_link(
    url: str,
    *,
    repository: str,
    expected_page: int,
    release_id: int | None,
) -> None:
    validated = GitHubApi._validated_url(url)
    parsed = urllib.parse.urlparse(validated)
    query = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
    if set(query) - {"page", "per_page"}:
        raise CounterError(f"unexpected query in GitHub pagination URL: {url}")
    if query.get("page") != [str(expected_page)]:
        raise CounterError(f"GitHub pagination did not advance to page {expected_page}: {url}")
    if "per_page" in query and query["per_page"] != [str(PAGE_SIZE)]:
        raise CounterError(f"unexpected per_page in GitHub pagination URL: {url}")
    escaped_repository = re.escape(repository)
    if release_id is None:
        allowed_paths = (
            rf"/repos/{escaped_repository}/releases",
            r"/repositories/[0-9]+/releases",
        )
    else:
        allowed_paths = (
            rf"/repos/{escaped_repository}/releases/{release_id}/assets",
            rf"/repositories/[0-9]+/releases/{release_id}/assets",
        )
    if not any(re.fullmatch(pattern, parsed.path) for pattern in allowed_paths):
        raise CounterError(f"pagination escaped the expected GitHub endpoint: {url}")


def _page_items(
    api: GitHubApi,
    path: str,
    *,
    repository: str,
    page: int,
    release_id: int | None,
) -> list[Any]:
    response = api.get(f"{path}?per_page={PAGE_SIZE}&page={page}")
    if not isinstance(response.data, list):
        raise CounterError("GitHub paginated response is not an array")
    if len(response.data) > PAGE_SIZE:
        raise CounterError(f"GitHub returned more than {PAGE_SIZE} items in one page")
    if response.next_url is not None:
        _validate_next_page_link(
            response.next_url,
            repository=repository,
            expected_page=page + 1,
            release_id=release_id,
        )
        if len(response.data) < PAGE_SIZE:
            raise CounterError("GitHub returned a next link after a short page")
    return response.data


def fetch_all_releases(api: GitHubApi, repository: str) -> list[dict[str, Any]]:
    releases: list[dict[str, Any]] = []
    seen_ids: set[int] = set()
    seen_tags: set[str] = set()
    for page in range(1, MAX_RELEASE_PAGES + 1):
        items = _page_items(
            api, f"/repos/{repository}/releases", repository=repository,
            page=page, release_id=None,
        )
        if not items:
            return releases
        for index, release in enumerate(items):
            label = f"releases page {page} item {index}"
            if not isinstance(release, dict):
                raise CounterError(f"{label} must be an object")
            release_id = _positive_integer(release.get("id"), f"{label}.id")
            tag_name = release.get("tag_name")
            if not isinstance(tag_name, str) or not tag_name:
                raise CounterError(f"{label}.tag_name must be a non-empty string")
            if release_id in seen_ids:
                raise CounterError(f"GitHub releases contain duplicate ID {release_id}")
            if tag_name in seen_tags:
                raise CounterError(f"GitHub releases contain duplicate tag {tag_name}")
            seen_ids.add(release_id)
            seen_tags.add(tag_name)
            releases.append(release)
            if len(releases) > MAX_RELEASES:
                raise CounterError(f"GitHub releases exceed the {MAX_RELEASES}-release safety limit")
        if len(items) < PAGE_SIZE:
            return releases
    raise CounterError(f"GitHub releases exceed the {MAX_RELEASE_PAGES}-page limit")


def fetch_release_assets(
    api: GitHubApi, repository: str, release_id: int
) -> list[dict[str, Any]]:
    assets: list[dict[str, Any]] = []
    seen_ids: set[int] = set()
    for page in range(1, MAX_ASSET_PAGES + 1):
        items = _page_items(
            api, f"/repos/{repository}/releases/{release_id}/assets",
            repository=repository, page=page, release_id=release_id,
        )
        if not items:
            return assets
        for index, asset in enumerate(items):
            label = f"release {release_id} assets page {page} item {index}"
            if not isinstance(asset, dict):
                raise CounterError(f"{label} must be an object")
            asset_id = _positive_integer(asset.get("id"), f"{label}.id")
            if asset_id in seen_ids:
                raise CounterError(f"GitHub release {release_id} contains duplicate asset ID {asset_id}")
            seen_ids.add(asset_id)
            assets.append(asset)
        if len(items) < PAGE_SIZE:
            return assets
    raise CounterError(
        f"GitHub release {release_id} assets exceed the {MAX_ASSET_PAGES}-page limit"
    )


def fetch_inventory_once(api: GitHubApi, repository: str) -> Inventory:
    raw_releases = fetch_all_releases(api, repository)
    releases: list[ReleaseRecord] = []
    all_asset_ids: set[int] = set()
    for release_index, raw_release in enumerate(raw_releases):
        release_id = _positive_integer(raw_release.get("id"), f"releases[{release_index}].id")
        tag_name = raw_release.get("tag_name")
        if not isinstance(tag_name, str) or not tag_name:
            raise CounterError(f"releases[{release_index}].tag_name must be non-empty")
        draft = raw_release.get("draft")
        if not isinstance(draft, bool):
            raise CounterError(f"releases[{release_index}].draft must be boolean")
        prerelease = raw_release.get("prerelease")
        if not isinstance(prerelease, bool):
            raise CounterError(f"releases[{release_index}].prerelease must be boolean")
        raw_assets = fetch_release_assets(api, repository, release_id)
        release_assets: list[AssetRecord] = []
        for asset_index, raw_asset in enumerate(raw_assets):
            label = f"release {release_id} asset {asset_index}"
            asset_id = _positive_integer(raw_asset.get("id"), f"{label}.id")
            name = raw_asset.get("name")
            if not isinstance(name, str) or not name or "\n" in name or "\r" in name:
                raise CounterError(f"{label}.name must be a non-empty single-line string")
            count = _nonnegative_integer(
                raw_asset.get("download_count"), f"{label}.download_count"
            )
            digest = raw_asset.get("digest")
            if digest is not None:
                if not isinstance(digest, str) or not DIGEST_RE.fullmatch(digest):
                    raise CounterError(f"{label}.digest must be a SHA-256 digest")
                digest = digest.lower()
            state = raw_asset.get("state")
            if state not in {"starter", "uploaded"}:
                raise CounterError(f"{label}.state must be starter or uploaded")
            if state == "starter" and digest is not None:
                raise CounterError(f"{label}.starter asset cannot have a digest")
            if asset_id in all_asset_ids:
                raise CounterError(f"GitHub inventory repeats asset ID {asset_id}")
            all_asset_ids.add(asset_id)
            release_assets.append(
                AssetRecord(asset_id, release_id, name, count, digest, state)
            )
        releases.append(
            ReleaseRecord(
                release_id, tag_name, tuple(release_assets), draft, prerelease
            )
        )
    return Inventory(tuple(releases))


def _inventory_shape(
    inventory: Inventory,
) -> dict[int, tuple[str, bool, bool, dict[int, tuple[str, str | None, str]]]]:
    return {
        release.release_id: (
            release.tag_name,
            release.draft,
            release.prerelease,
            {
                asset.asset_id: (asset.name, asset.digest, asset.state)
                for asset in release.assets
            },
        )
        for release in inventory.releases
    }


def fetch_consistent_inventory(api: GitHubApi, repository: str) -> Inventory:
    first = fetch_inventory_once(api, repository)
    second = fetch_inventory_once(api, repository)
    if _inventory_shape(first) != _inventory_shape(second):
        raise CounterError("GitHub release/asset inventory changed between verification passes")
    first_assets = first.assets
    for asset_id, current in second.assets.items():
        if current.download_count < first_assets[asset_id].download_count:
            raise CounterError(f"asset {asset_id} download count regressed between verification passes")
    return second


def _migrate_legacy(legacy: LegacyState, inventory: Inventory, *, updated: str) -> CounterState:
    assets = dict(inventory.assets)
    total, installer_total = _derived_totals(
        legacy.base_total, legacy.base_installer_total, assets.values()
    )
    if total < legacy.previous_total:
        raise CounterError(
            f"legacy migration would regress lifetime downloads from {legacy.previous_total} to {total}"
        )
    if installer_total < legacy.previous_installer_total:
        raise CounterError(
            "legacy migration would regress installer downloads from "
            f"{legacy.previous_installer_total} to {installer_total}"
        )
    return CounterState(
        legacy.base_total, legacy.base_installer_total, total, installer_total,
        assets, frozenset(assets), None, updated,
    )


def reconcile_inventory(
    state: CounterState,
    inventory: Inventory,
    *,
    allowed_missing_asset_ids: frozenset[int] = frozenset(),
    updated: str,
) -> CounterState:
    if not allowed_missing_asset_ids.issubset(state.live_asset_ids):
        raise CounterError("allowed missing assets were not live in the prior state")
    current_assets = inventory.assets
    missing = state.live_asset_ids - current_assets.keys()
    unexpected = missing - allowed_missing_asset_ids
    if unexpected:
        rendered = ", ".join(str(asset_id) for asset_id in sorted(unexpected))
        raise CounterError(
            "live GitHub assets disappeared without a durable pending publication: " + rendered
        )
    ledger = dict(state.assets)
    for asset_id, current in current_assets.items():
        previous = ledger.get(asset_id)
        if previous is not None:
            if previous.release_id != current.release_id or previous.name != current.name:
                raise CounterError(f"GitHub asset identity changed for ID {asset_id}")
            if current.download_count < previous.download_count:
                raise CounterError(
                    f"GitHub asset {asset_id} download count regressed from "
                    f"{previous.download_count} to {current.download_count}"
                )
            if previous.state == "uploaded" and current.state != "uploaded":
                raise CounterError(f"GitHub asset {asset_id} regressed to starter state")
            if previous.digest is not None and current.digest != previous.digest:
                raise CounterError(f"GitHub asset digest changed for ID {asset_id}")
            if (
                previous.state == "starter"
                and current.state == "uploaded"
                and current.digest is None
            ):
                raise CounterError(
                    f"GitHub asset {asset_id} became uploaded without a digest"
                )
        ledger[asset_id] = current
    total, installer_total = _derived_totals(
        state.base_total, state.base_installer_total, ledger.values()
    )
    if total < state.total or installer_total < state.installer_total:
        raise CounterError("reconciled cumulative counters regressed")
    return CounterState(
        state.base_total, state.base_installer_total, total, installer_total,
        ledger, frozenset(current_assets), state.pending, updated,
    )


@dataclass(frozen=True)
class PublicationResult:
    total: int
    installer_total: int
    target_downloads: int
    operation_id: str
    recovered_pending: bool
    replacement_required: bool = False
    cleanup_required: bool = False


@dataclass(frozen=True)
class PublicationPreflightResult:
    target_exists: bool
    target_asset_count: int
    target_release_id: int | None
    target_is_draft: bool


@dataclass(frozen=True)
class EvidenceReplacementPreflightResult:
    replacement_required: bool
    delete_required: bool
    upload_required: bool
    target_release_id: int | None
    delete_asset_id: int | None
    delete_asset_digest: str | None
    delete_asset_state: str | None
    delete_asset_download_count: int | None


@dataclass(frozen=True)
class DraftCleanupPreflightResult:
    cleanup_required: bool
    target_release_id: int | None
    cleanup_assets: tuple[DraftAssetCleanupIntent, ...]


@dataclass(frozen=True)
class CounterRefreshResult:
    total: int
    installer_total: int


def _validate_target_inputs(
    *, repository: str, is_versioned: bool, target_tag: str
) -> None:
    if not REPOSITORY_RE.fullmatch(repository):
        raise CounterError(f"invalid GitHub repository name: {repository!r}")
    _validate_tag(target_tag, is_versioned, "target tag")


def _validate_publication_inputs(
    *, repository: str, is_versioned: bool, run_id: str, run_attempt: str,
    source_sha: str, target_tag: str,
) -> str:
    _validate_target_inputs(
        repository=repository, is_versioned=is_versioned, target_tag=target_tag
    )
    run_id = _decimal_string(run_id, "GITHUB_RUN_ID")
    run_attempt = _decimal_string(run_attempt, "GITHUB_RUN_ATTEMPT")
    if not SHA_RE.fullmatch(source_sha):
        raise CounterError("GITHUB_SHA must be a full commit SHA")
    return f"{run_id}:{run_attempt}"


def _require_pending_target(
    pending: PendingPublication,
    *,
    is_versioned: bool,
    source_sha: str,
    target_tag: str,
) -> None:
    if (
        pending.target_tag != target_tag
        or pending.is_versioned != is_versioned
        or pending.source_sha != source_sha.lower()
    ):
        raise CounterError(
            "pending publication belongs to a different target, channel, or source SHA"
        )


def _require_pending_operation(
    pending: PendingPublication,
    *,
    operation_id: str,
    is_versioned: bool,
    source_sha: str,
    target_tag: str,
    phase: str,
    label: str,
) -> None:
    _require_pending_target(
        pending,
        is_versioned=is_versioned,
        source_sha=source_sha,
        target_tag=target_tag,
    )
    if pending.operation_id != operation_id or pending.phase != phase:
        raise CounterError(
            f"{label} requires this operation's durable {phase} publication"
        )


def _badge(label: str, message: str, color: str) -> dict[str, Any]:
    return {
        "schemaVersion": 1, "label": label, "message": message, "color": color,
        "namedLogo": "github", "logoColor": "white",
    }


def _serialize_json(value: Any) -> bytes:
    return (json.dumps(value, indent=4, ensure_ascii=False) + "\n").encode("utf-8")


def _write_files(files: dict[Path, bytes]) -> None:
    staged: list[tuple[Path, Path]] = []
    try:
        for destination, payload in files.items():
            destination.parent.mkdir(parents=True, exist_ok=True)
            descriptor, temporary_name = tempfile.mkstemp(
                prefix=f".{destination.name}.", dir=destination.parent
            )
            temporary = Path(temporary_name)
            try:
                with os.fdopen(descriptor, "wb") as stream:
                    stream.write(payload)
                    stream.flush()
                    os.fsync(stream.fileno())
            except Exception:
                temporary.unlink(missing_ok=True)
                raise
            staged.append((temporary, destination))
        for temporary, destination in staged:
            os.replace(temporary, destination)
    except OSError as exc:
        raise CounterError(f"cannot write download-counter files: {exc}") from exc
    finally:
        for temporary, _ in staged:
            temporary.unlink(missing_ok=True)


def _counter_files(
    state: CounterState, *, data_file: Path, badge_directory: Path
) -> dict[Path, bytes]:
    return {
        data_file: _serialize_json(_state_payload(state)),
        badge_directory / "downloads.json": _serialize_json(
            _badge("lifetime downloads", f"{state.total:,}", "brightgreen")
        ),
        badge_directory / "installer-downloads.json": _serialize_json(
            _badge("installer downloads", f"{state.installer_total:,}", "blueviolet")
        ),
    }


def prepare_publication(
    *,
    repository: str,
    is_versioned: bool,
    run_id: str,
    run_attempt: str,
    source_sha: str,
    target_tag: str,
    data_file: Path,
    badge_directory: Path,
    api: GitHubApi,
    initialize: bool = False,
    expected_assets_file: Path | None = None,
    expected_digests_file: Path | None = None,
    allow_exact_ci_evidence_replacement: bool = False,
    now: Callable[[], datetime] = lambda: datetime.now(timezone.utc),
) -> PublicationResult:
    operation_id = _validate_publication_inputs(
        repository=repository, is_versioned=is_versioned, run_id=run_id,
        run_attempt=run_attempt, source_sha=source_sha, target_tag=target_tag,
    )
    timestamp = _utc_now(now)
    loaded = load_state(data_file, initialize=initialize)
    inherited_public_nightly: PendingPublication | None = None
    if isinstance(loaded, CounterState) and loaded.pending is not None:
        _require_pending_target(
            loaded.pending,
            is_versioned=is_versioned,
            source_sha=source_sha,
            target_tag=target_tag,
        )
        if (
            not is_versioned
            and loaded.pending.phase == "prepared"
            and loaded.pending.target_draft_at_prepare is False
        ):
            inherited_public_nightly = loaded.pending
    inventory = fetch_consistent_inventory(api, repository)
    recovered = isinstance(loaded, CounterState) and loaded.pending is not None

    preserve_public_nightly_origin = False
    if inherited_public_nightly is not None and isinstance(loaded, CounterState):
        inherited_target = inventory.release_by_tag(target_tag)
        if (
            inherited_target is not None
            and inherited_target.draft
            and inherited_target.prerelease
            and inherited_target.release_id
            == inherited_public_nightly.target_release_id
            and frozenset(asset.asset_id for asset in inherited_target.assets)
            == inherited_public_nightly.prepared_asset_ids
            and all(
                _same_asset_record(loaded.assets[asset.asset_id], asset)
                for asset in inherited_target.assets
            )
        ):
            preserve_public_nightly_origin = True

    expected_assets: frozenset[str] | None = None
    expected_digests: dict[str, str] | None = None
    if (expected_assets_file is None) != (expected_digests_file is None):
        raise CounterError("expected asset and digest files must be provided together")
    if expected_assets_file is not None and expected_digests_file is not None:
        expected_assets = _read_expected_assets(expected_assets_file)
        expected_digests = _read_expected_digests(
            expected_digests_file, expected_assets
        )
    if allow_exact_ci_evidence_replacement:
        if not is_versioned:
            raise CounterError("exact CI evidence replacement is valid only for stable releases")
        if expected_assets is None or expected_digests is None:
            raise CounterError(
                "exact CI evidence replacement requires expected asset and digest files"
            )
        if EXACT_CI_EVIDENCE_ASSET not in expected_assets:
            raise CounterError("expected release assets omit the exact CI evidence asset")

    if (
        isinstance(loaded, CounterState)
        and loaded.pending is not None
        and loaded.pending.is_versioned
        and loaded.pending.phase == "prepared"
    ):
        if (
            expected_assets is None
            or expected_digests is None
        ):
            raise CounterError(
                "recovering a prepared stable publication requires expected asset evidence"
            )
        target = inventory.release_by_tag(target_tag)
        state, rebound_pending = _recover_stable_draft_cleanup(
            loaded=loaded,
            inventory=inventory,
            pending=loaded.pending,
            target=target,
            expected_assets=expected_assets,
            expected_digests=expected_digests,
            operation_id=operation_id,
            timestamp=timestamp,
        )
        replacement_required = rebound_pending.evidence_replacement is not None
        if replacement_required:
            if not allow_exact_ci_evidence_replacement:
                raise CounterError(
                    "recovering exact CI evidence replacement requires explicit authorization"
                )
            rebound_pending, replacement_required = _recover_evidence_replacement_intent(
                state=state,
                pending=rebound_pending,
                target=target,
                expected_assets=expected_assets,
                expected_digests=expected_digests,
                operation_id=operation_id,
                timestamp=timestamp,
            )
        state = replace(state, pending=rebound_pending)
        if replacement_required:
            _validate_evidence_replacement_inventory(
                state=state,
                pending=rebound_pending,
                target=target,
                expected_assets=expected_assets,
                expected_digests=expected_digests,
            )
        elif rebound_pending.draft_cleanup:
            _validate_draft_cleanup_inventory(
                state=state,
                pending=rebound_pending,
                target=target,
                expected_assets=expected_assets,
                expected_digests=expected_digests,
            )
        elif target is not None:
            _validate_existing_target(
                expected_assets=expected_assets,
                expected_digests=expected_digests,
                target=target,
                is_versioned=True,
            )
        target_assets = target.assets if target else ()
        _write_files(
            _counter_files(state, data_file=data_file, badge_directory=badge_directory)
        )
        return PublicationResult(
            state.total,
            state.installer_total,
            sum(asset.download_count for asset in target_assets),
            operation_id,
            True,
            replacement_required,
            bool(rebound_pending.draft_cleanup),
        )

    if isinstance(loaded, LegacyState):
        state = _migrate_legacy(loaded, inventory, updated=timestamp)
    else:
        if loaded.pending is None:
            allowed = frozenset()
        elif loaded.pending.is_versioned:
            allowed = _pending_authorized_missing_ids(loaded, loaded.pending)
        else:
            # Rolling nightly staging intentionally replaces the prior asset IDs.
            allowed = loaded.pending.prepared_asset_ids
        state = reconcile_inventory(
            loaded, inventory, allowed_missing_asset_ids=allowed, updated=timestamp
        )
        state = replace(state, pending=None)
    target = inventory.release_by_tag(target_tag)
    if target is not None:
        _require_channel_prerelease(
            target,
            is_versioned=is_versioned,
            label="prepared target",
        )
    target_assets = target.assets if target else ()
    replacement_intent: EvidenceReplacementIntent | None = None
    if allow_exact_ci_evidence_replacement and target is not None:
        if expected_assets is None or expected_digests is None:
            raise CounterError("internal exact CI evidence contract is unavailable")
        _validate_existing_target(
            expected_assets=expected_assets,
            expected_digests=expected_digests,
            target=target,
            is_versioned=is_versioned,
            allow_exact_ci_evidence_replacement=True,
        )
        evidence_assets = [
            asset for asset in target.assets if asset.name == EXACT_CI_EVIDENCE_ASSET
        ]
        if (
            target.draft
            and evidence_assets
            and evidence_assets[0].digest
            != expected_digests[EXACT_CI_EVIDENCE_ASSET]
        ):
            evidence = evidence_assets[0]
            if evidence.digest is None or not DIGEST_RE.fullmatch(evidence.digest):
                raise CounterError("existing exact CI evidence asset has no valid digest")
            replacement_intent = EvidenceReplacementIntent(
                evidence.asset_id,
                evidence.digest.lower(),
                expected_digests[EXACT_CI_EVIDENCE_ASSET],
            )
    state = replace(
        state,
        pending=PendingPublication(
            operation_id=operation_id,
            target_tag=target_tag,
            is_versioned=is_versioned,
            source_sha=source_sha.lower(),
            phase="prepared",
            target_release_id=target.release_id if target else None,
            prepared_asset_ids=frozenset(asset.asset_id for asset in target_assets),
            prepared_at=timestamp,
            target_draft_at_prepare=(
                False
                if preserve_public_nightly_origin
                else target.draft if target else None
            ),
            evidence_replacement=replacement_intent,
        ),
    )
    _write_files(_counter_files(state, data_file=data_file, badge_directory=badge_directory))
    return PublicationResult(
        state.total, state.installer_total,
        sum(asset.download_count for asset in target_assets), operation_id, recovered,
        replacement_intent is not None,
    )


def refresh_counters(
    *,
    repository: str,
    data_file: Path,
    badge_directory: Path,
    api: GitHubApi,
    now: Callable[[], datetime] = lambda: datetime.now(timezone.utc),
) -> CounterRefreshResult:
    """Refresh durable lifetime counters without beginning a publication."""
    if not REPOSITORY_RE.fullmatch(repository):
        raise CounterError(f"invalid GitHub repository name: {repository!r}")
    timestamp = _utc_now(now)
    loaded = load_state(data_file)
    inventory = fetch_consistent_inventory(api, repository)
    nightly = inventory.release_by_tag("nightly")
    if nightly is not None and nightly.draft:
        raise CounterError(
            "cannot refresh counters while the nightly release is hidden as a draft"
        )
    if isinstance(loaded, LegacyState):
        state = _migrate_legacy(loaded, inventory, updated=timestamp)
    else:
        if loaded.pending is not None:
            raise CounterError(
                "cannot refresh counters while a publication is pending recovery"
            )
        state = reconcile_inventory(loaded, inventory, updated=timestamp)
    _write_files(
        _counter_files(state, data_file=data_file, badge_directory=badge_directory)
    )
    return CounterRefreshResult(state.total, state.installer_total)


def _read_expected_assets(path: Path) -> frozenset[str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise CounterError(f"cannot read expected release assets {path}: {exc}") from exc
    if not lines:
        raise CounterError("expected release asset list is empty")
    expected: set[str] = set()
    for index, name in enumerate(lines):
        if not name or name != Path(name).name or "\r" in name or "\n" in name:
            raise CounterError(f"expected asset line {index + 1} is not a plain filename")
        if name in expected:
            raise CounterError(f"expected release asset list repeats {name}")
        expected.add(name)
    return frozenset(expected)


def _read_expected_digests(
    path: Path, expected_assets: frozenset[str]
) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise CounterError(f"cannot read expected release digests {path}: {exc}") from exc
    digests: dict[str, str] = {}
    for index, line in enumerate(lines):
        match = re.fullmatch(r"([0-9a-fA-F]{64})  (.+)", line)
        if match is None:
            raise CounterError(f"expected digest line {index + 1} is malformed")
        name = match.group(2)
        if not name or name != Path(name).name:
            raise CounterError(f"expected digest line {index + 1} has an invalid filename")
        if name in digests:
            raise CounterError(f"expected release digests repeat {name}")
        digests[name] = "sha256:" + match.group(1).lower()
    actual_names = frozenset(digests)
    if actual_names != expected_assets:
        missing = expected_assets - actual_names
        unexpected = actual_names - expected_assets
        details: list[str] = []
        if missing:
            details.append("missing: " + ", ".join(sorted(missing)))
        if unexpected:
            details.append("unexpected: " + ", ".join(sorted(unexpected)))
        raise CounterError(
            "expected digest set differs from expected assets ("
            + "; ".join(details)
            + ")"
        )
    return digests


def _require_exact_asset_set(
    expected: frozenset[str], assets: tuple[AssetRecord, ...], *, label: str
) -> None:
    actual_names = [asset.name for asset in assets]
    if len(actual_names) != len(set(actual_names)):
        raise CounterError(f"{label} contains duplicate asset names")
    actual = set(actual_names)
    missing = expected - actual
    unexpected = actual - expected
    if missing or unexpected:
        details: list[str] = []
        if missing:
            details.append("missing: " + ", ".join(sorted(missing)))
        if unexpected:
            details.append("unexpected: " + ", ".join(sorted(unexpected)))
        raise CounterError(f"{label} asset set differs (" + "; ".join(details) + ")")


def _require_asset_name_subset(
    expected: frozenset[str], assets: tuple[AssetRecord, ...], *, label: str
) -> None:
    actual_names = [asset.name for asset in assets]
    if len(actual_names) != len(set(actual_names)):
        raise CounterError(f"{label} contains duplicate asset names")
    unexpected = set(actual_names) - expected
    if unexpected:
        raise CounterError(
            f"{label} contains unexpected assets: "
            + ", ".join(sorted(unexpected))
        )


def _require_exact_asset_digests(
    expected: dict[str, str],
    assets: tuple[AssetRecord, ...],
    *,
    label: str,
    ignored_names: frozenset[str] = frozenset(),
) -> None:
    actual = {asset.name: asset.digest for asset in assets}
    missing_digests = sorted(
        name
        for name, digest in actual.items()
        if name not in ignored_names and digest is None
    )
    incomplete = sorted(
        asset.name
        for asset in assets
        if asset.name not in ignored_names and asset.state != "uploaded"
    )
    mismatched = sorted(
        name
        for name, digest in actual.items()
        if name not in ignored_names
        and digest is not None
        and expected.get(name) != digest
    )
    if missing_digests or mismatched or incomplete:
        details: list[str] = []
        if missing_digests:
            details.append("missing remote digest: " + ", ".join(missing_digests))
        if mismatched:
            details.append("digest mismatch: " + ", ".join(mismatched))
        if incomplete:
            details.append("not uploaded: " + ", ".join(incomplete))
        raise CounterError(f"{label} asset digests differ (" + "; ".join(details) + ")")


def _require_channel_prerelease(
    target: ReleaseRecord,
    *,
    is_versioned: bool,
    label: str,
    allow_legacy_nightly: bool = False,
) -> None:
    if is_versioned and target.prerelease:
        raise CounterError(f"{label} stable release cannot be a prerelease")
    if not is_versioned and not target.prerelease and not allow_legacy_nightly:
        raise CounterError(f"{label} nightly release must be a prerelease")


def _validate_existing_target(
    *,
    expected_assets: frozenset[str],
    expected_digests: dict[str, str],
    target: ReleaseRecord,
    is_versioned: bool,
    allow_exact_ci_evidence_replacement: bool = False,
    allow_stable_draft_recovery_candidates: bool = False,
    allow_legacy_nightly_prerelease: bool = False,
) -> None:
    _require_channel_prerelease(
        target,
        is_versioned=is_versioned,
        label="existing target",
        allow_legacy_nightly=allow_legacy_nightly_prerelease,
    )
    if not is_versioned:
        # Rolling nightlies may add newly introduced aliases/metadata, but an
        # asset removed from the desired contract is stale and must be handled
        # by an explicit migration rather than silently surviving forever.
        _require_asset_name_subset(
            expected_assets, target.assets, label="existing nightly release"
        )
        return

    if target.draft:
        # An interrupted stable publication may resume only missing uploads.
        # Every asset already present must be one of the desired names and
        # must already have the exact digest generated by this source commit.
        _require_asset_name_subset(
            expected_assets, target.assets, label="existing draft release"
        )
        if allow_stable_draft_recovery_candidates:
            return
        ignored_names: frozenset[str] = frozenset()
        if allow_exact_ci_evidence_replacement:
            if EXACT_CI_EVIDENCE_ASSET not in expected_assets:
                raise CounterError(
                    "exact CI evidence replacement requires its exact expected asset"
                )
            ignored_names = frozenset({EXACT_CI_EVIDENCE_ASSET})
        _require_exact_asset_digests(
            expected_digests,
            target.assets,
            label="existing draft release",
            ignored_names=ignored_names,
        )
        return

    # Published versioned releases are immutable. A rerun is a no-op only when
    # both the complete name set and all GitHub-computed digests are identical.
    _require_exact_asset_set(
        expected_assets, target.assets, label="existing release"
    )
    _require_exact_asset_digests(
        expected_digests, target.assets, label="existing release"
    )


def _same_asset_record(left: AssetRecord, right: AssetRecord) -> bool:
    return (
        left.asset_id == right.asset_id
        and left.release_id == right.release_id
        and left.name == right.name
        and left.download_count == right.download_count
        and left.digest == right.digest
        and left.state == right.state
    )


def _pending_authorized_missing_ids(
    state: CounterState, pending: PendingPublication
) -> frozenset[int]:
    candidate_ids = {cleanup.asset_id for cleanup in pending.draft_cleanup}
    if pending.evidence_replacement is not None:
        candidate_ids.add(pending.evidence_replacement.old_asset_id)
        if pending.evidence_replacement.cleanup is not None:
            candidate_ids.add(pending.evidence_replacement.cleanup.asset_id)
    return frozenset(candidate_ids & state.live_asset_ids)


def _recover_stable_draft_cleanup(
    *,
    loaded: CounterState,
    inventory: Inventory,
    pending: PendingPublication,
    target: ReleaseRecord | None,
    expected_assets: frozenset[str],
    expected_digests: dict[str, str],
    operation_id: str,
    timestamp: str,
) -> tuple[CounterState, PendingPublication]:
    """Freeze prior IDs and bind only newly introduced failed draft uploads."""

    if not pending.is_versioned or pending.phase != "prepared":
        raise CounterError("stable-draft cleanup recovery requires a prepared stable publication")
    if target is None:
        if pending.target_release_id is not None or pending.prepared_asset_ids:
            raise CounterError("prepared stable target disappeared during recovery")
        if pending.draft_cleanup or pending.evidence_replacement is not None:
            raise CounterError("an absent stable target cannot retain cleanup intent")
        state = reconcile_inventory(loaded, inventory, updated=timestamp)
        return state, replace(
            pending,
            operation_id=operation_id,
            prepared_at=timestamp,
        )

    _require_channel_prerelease(
        target, is_versioned=True, label="recovered stable target"
    )
    if pending.target_release_id is not None and (
        target.release_id != pending.target_release_id
    ):
        raise CounterError("stable release identity changed after durable preparation")
    if pending.target_release_id is None and pending.prepared_asset_ids:
        raise CounterError("an absent prepared target cannot protect asset IDs")

    if not target.draft:
        if pending.draft_cleanup or pending.evidence_replacement is not None:
            raise CounterError("stable draft became public with unresolved cleanup")
        _validate_existing_target(
            expected_assets=expected_assets,
            expected_digests=expected_digests,
            target=target,
            is_versioned=True,
        )
        state = reconcile_inventory(loaded, inventory, updated=timestamp)
        current_ids = frozenset(asset.asset_id for asset in target.assets)
        if pending.target_release_id is not None and (
            pending.prepared_asset_ids != current_ids
        ):
            raise CounterError("published stable asset IDs changed after preparation")
        rebound = replace(
            pending,
            operation_id=operation_id,
            target_release_id=target.release_id,
            prepared_asset_ids=current_ids,
            prepared_at=timestamp,
            target_draft_at_prepare=False,
        )
        return state, rebound

    _require_asset_name_subset(
        expected_assets, target.assets, label="recovered stable draft"
    )
    current_by_id = {asset.asset_id: asset for asset in target.assets}
    current_by_name = {asset.name: asset for asset in target.assets}
    if len(current_by_name) != len(target.assets):
        raise CounterError("recovered stable draft contains duplicate asset names")

    replacement = pending.evidence_replacement
    replacement_old_id = replacement.old_asset_id if replacement is not None else None
    replacement_cleanup_id = (
        replacement.cleanup.asset_id
        if replacement is not None and replacement.cleanup is not None
        else None
    )
    protected_ids = set(pending.prepared_asset_ids)
    for asset_id in sorted(pending.prepared_asset_ids):
        current = current_by_id.get(asset_id)
        previous = loaded.assets.get(asset_id)
        if current is None:
            if asset_id == replacement_old_id:
                continue
            raise CounterError(
                f"prepared stable draft asset {asset_id} disappeared without authorization"
            )
        if (
            previous is None
            or current.release_id != previous.release_id
            or current.name != previous.name
            or current.state != previous.state
            or current.digest != previous.digest
            or current.download_count < previous.download_count
        ):
            raise CounterError(
                f"prepared stable draft asset {asset_id} changed after authorization"
            )
        if asset_id == replacement_old_id:
            if (
                current.name != EXACT_CI_EVIDENCE_ASSET
                or current.state != "uploaded"
                or current.digest != replacement.old_digest
            ):
                raise CounterError("authorized old exact CI evidence asset changed")
        elif (
            current.state != "uploaded"
            or current.digest != expected_digests.get(current.name)
        ):
            raise CounterError(
                f"prepared stable draft asset is no longer exact: {current.name}"
            )

    remaining_cleanup: list[DraftAssetCleanupIntent] = []
    known_cleanup_ids = {cleanup.asset_id for cleanup in pending.draft_cleanup}
    for cleanup in pending.draft_cleanup:
        current = current_by_id.get(cleanup.asset_id)
        if current is None:
            remaining_cleanup.append(cleanup)
            continue
        expected_record = AssetRecord(
            cleanup.asset_id,
            target.release_id,
            cleanup.name,
            cleanup.download_count,
            cleanup.digest,
            cleanup.state,
        )
        if _same_asset_record(current, expected_record):
            remaining_cleanup.append(cleanup)
            continue
        if (
            cleanup.state == "starter"
            and cleanup.digest is None
            and current.release_id == target.release_id
            and current.name == cleanup.name
            and current.download_count == cleanup.download_count
            and current.state == "uploaded"
            and current.digest == expected_digests.get(current.name)
        ):
            protected_ids.add(current.asset_id)
            continue
        raise CounterError(
            f"durably authorized draft cleanup asset changed: {cleanup.name}"
        )

    reserved_ids = set(protected_ids) | known_cleanup_ids
    if replacement_old_id is not None:
        reserved_ids.add(replacement_old_id)
    if replacement_cleanup_id is not None:
        reserved_ids.add(replacement_cleanup_id)
    new_cleanup: list[DraftAssetCleanupIntent] = []
    for asset in target.assets:
        if asset.asset_id in reserved_ids:
            continue
        if asset.asset_id in loaded.assets and asset.asset_id not in loaded.live_asset_ids:
            raise CounterError(
                f"stable draft reuses historical asset ID {asset.asset_id}"
            )
        if replacement is not None and asset.name == EXACT_CI_EVIDENCE_ASSET:
            # The exact-evidence replacement state machine binds this candidate.
            continue
        if asset.state == "uploaded" and asset.digest == expected_digests[asset.name]:
            protected_ids.add(asset.asset_id)
            continue
        new_cleanup.append(
            DraftAssetCleanupIntent(
                asset.asset_id,
                asset.name,
                asset.digest,
                asset.state,
                asset.download_count,
            )
        )

    cleanup = tuple(
        sorted(remaining_cleanup + new_cleanup, key=lambda item: item.asset_id)
    )
    if replacement is not None and cleanup:
        raise CounterError(
            "stable draft cleanup must be accepted before exact evidence replacement"
        )
    cleanup_names = [item.name for item in cleanup]
    if len(cleanup_names) != len(set(cleanup_names)):
        raise CounterError("stable draft cleanup would authorize duplicate asset names")

    state = reconcile_inventory(
        loaded,
        inventory,
        allowed_missing_asset_ids=_pending_authorized_missing_ids(loaded, pending),
        updated=timestamp,
    )
    rebound = replace(
        pending,
        operation_id=operation_id,
        target_release_id=target.release_id,
        prepared_asset_ids=frozenset(protected_ids),
        prepared_at=timestamp,
        target_draft_at_prepare=True,
        draft_cleanup=cleanup,
    )
    return state, rebound


def _validate_draft_cleanup_inventory(
    *,
    state: CounterState,
    pending: PendingPublication,
    target: ReleaseRecord | None,
    expected_assets: frozenset[str],
    expected_digests: dict[str, str],
) -> tuple[DraftAssetCleanupIntent, ...]:
    """Bind every cleanup candidate and every protected ID to the stable draft."""

    if not pending.draft_cleanup:
        raise CounterError("stable draft cleanup has no durable intent")
    if pending.evidence_replacement is not None:
        raise CounterError("stable draft cleanup cannot overlap evidence replacement")
    if (
        not pending.is_versioned
        or pending.phase != "prepared"
        or pending.target_release_id is None
        or pending.target_draft_at_prepare is not True
        or target is None
        or target.release_id != pending.target_release_id
        or not target.draft
        or target.prerelease
    ):
        raise CounterError("stable draft cleanup is not bound to the same unpublished release")

    _require_asset_name_subset(
        expected_assets, target.assets, label="stable draft cleanup target"
    )
    current_by_id = {asset.asset_id: asset for asset in target.assets}
    if len({asset.name for asset in target.assets}) != len(target.assets):
        raise CounterError("stable draft cleanup target contains duplicate asset names")

    cleanup_by_id = {cleanup.asset_id: cleanup for cleanup in pending.draft_cleanup}
    allowed_ids = set(pending.prepared_asset_ids) | set(cleanup_by_id)
    unexpected_ids = set(current_by_id) - allowed_ids
    if unexpected_ids:
        raise CounterError(
            "stable draft gained assets after cleanup authorization: "
            + ", ".join(str(asset_id) for asset_id in sorted(unexpected_ids))
        )

    for asset_id in sorted(pending.prepared_asset_ids):
        current = current_by_id.get(asset_id)
        previous = state.assets.get(asset_id)
        if current is None:
            raise CounterError(
                f"prepared stable draft asset {asset_id} disappeared during cleanup"
            )
        if (
            previous is None
            or current.release_id != previous.release_id
            or current.name != previous.name
            or current.state != previous.state
            or current.digest != previous.digest
            or current.download_count < previous.download_count
        ):
            raise CounterError(
                f"prepared stable draft asset {asset_id} changed during cleanup"
            )
        if (
            current.state != "uploaded"
            or current.digest != expected_digests.get(current.name)
        ):
            raise CounterError(
                f"prepared stable draft asset is not exact during cleanup: {current.name}"
            )

    present_cleanup: list[DraftAssetCleanupIntent] = []
    for cleanup in pending.draft_cleanup:
        current = current_by_id.get(cleanup.asset_id)
        if current is None:
            continue
        expected_record = AssetRecord(
            cleanup.asset_id,
            target.release_id,
            cleanup.name,
            cleanup.download_count,
            cleanup.digest,
            cleanup.state,
        )
        if not _same_asset_record(current, expected_record):
            raise CounterError(
                f"stable draft cleanup candidate changed: {cleanup.name}"
            )
        if (
            current.state == "uploaded"
            and current.digest == expected_digests.get(current.name)
        ):
            raise CounterError(
                f"stable draft cleanup cannot delete an exact asset: {cleanup.name}"
            )
        present_cleanup.append(cleanup)
    return tuple(present_cleanup)


def preflight_draft_cleanup(
    *,
    repository: str,
    is_versioned: bool,
    run_id: str,
    run_attempt: str,
    source_sha: str,
    target_tag: str,
    expected_assets_file: Path,
    expected_digests_file: Path,
    data_file: Path,
    api: GitHubApi,
) -> DraftCleanupPreflightResult:
    """Return only still-present assets from one durable cleanup intent."""

    operation_id = _validate_publication_inputs(
        repository=repository,
        is_versioned=is_versioned,
        run_id=run_id,
        run_attempt=run_attempt,
        source_sha=source_sha,
        target_tag=target_tag,
    )
    if not is_versioned:
        raise CounterError("stable draft cleanup is valid only for versioned releases")
    loaded = load_state(data_file)
    if not isinstance(loaded, CounterState) or loaded.pending is None:
        raise CounterError("stable draft cleanup preflight requires durable pending state")
    pending = loaded.pending
    _require_pending_operation(
        pending,
        operation_id=operation_id,
        is_versioned=is_versioned,
        source_sha=source_sha,
        target_tag=target_tag,
        phase="prepared",
        label="stable draft cleanup preflight",
    )
    expected = _read_expected_assets(expected_assets_file)
    expected_digests = _read_expected_digests(expected_digests_file, expected)
    inventory = fetch_consistent_inventory(api, repository)
    target = inventory.release_by_tag(target_tag)
    present = _validate_draft_cleanup_inventory(
        state=loaded,
        pending=pending,
        target=target,
        expected_assets=expected,
        expected_digests=expected_digests,
    )
    return DraftCleanupPreflightResult(True, pending.target_release_id, present)


def accept_draft_cleanup(
    *,
    repository: str,
    is_versioned: bool,
    run_id: str,
    run_attempt: str,
    source_sha: str,
    target_tag: str,
    expected_assets_file: Path,
    expected_digests_file: Path,
    data_file: Path,
    badge_directory: Path,
    api: GitHubApi,
    now: Callable[[], datetime] = lambda: datetime.now(timezone.utc),
) -> PublicationResult:
    """Accept only after every durably authorized failed upload is absent."""

    operation_id = _validate_publication_inputs(
        repository=repository,
        is_versioned=is_versioned,
        run_id=run_id,
        run_attempt=run_attempt,
        source_sha=source_sha,
        target_tag=target_tag,
    )
    if not is_versioned:
        raise CounterError("stable draft cleanup is valid only for versioned releases")
    loaded = load_state(data_file)
    if not isinstance(loaded, CounterState) or loaded.pending is None:
        raise CounterError("stable draft cleanup accept requires durable pending state")
    pending = loaded.pending
    _require_pending_operation(
        pending,
        operation_id=operation_id,
        is_versioned=is_versioned,
        source_sha=source_sha,
        target_tag=target_tag,
        phase="prepared",
        label="stable draft cleanup accept",
    )
    expected = _read_expected_assets(expected_assets_file)
    expected_digests = _read_expected_digests(expected_digests_file, expected)
    inventory = fetch_consistent_inventory(api, repository)
    target = inventory.release_by_tag(target_tag)
    present = _validate_draft_cleanup_inventory(
        state=loaded,
        pending=pending,
        target=target,
        expected_assets=expected,
        expected_digests=expected_digests,
    )
    if present:
        raise CounterError(
            "stable draft cleanup assets remain present: "
            + ", ".join(cleanup.name for cleanup in present)
        )
    timestamp = _utc_now(now)
    state = reconcile_inventory(
        loaded,
        inventory,
        allowed_missing_asset_ids=_pending_authorized_missing_ids(loaded, pending),
        updated=timestamp,
    )
    if target is None:
        raise CounterError("stable draft disappeared during cleanup acceptance")
    accepted_pending = replace(
        pending,
        prepared_asset_ids=frozenset(asset.asset_id for asset in target.assets),
        prepared_at=timestamp,
        draft_cleanup=(),
    )
    state = replace(state, pending=accepted_pending)
    _write_files(
        _counter_files(state, data_file=data_file, badge_directory=badge_directory)
    )
    return PublicationResult(
        state.total,
        state.installer_total,
        sum(asset.download_count for asset in target.assets),
        operation_id,
        True,
        False,
        False,
    )


def _replacement_live_missing_ids(
    state: CounterState, intent: EvidenceReplacementIntent
) -> frozenset[int]:
    candidate_ids = {intent.old_asset_id}
    if intent.cleanup is not None:
        candidate_ids.add(intent.cleanup.asset_id)
    return frozenset(candidate_ids & state.live_asset_ids)


def _recover_evidence_replacement_intent(
    *,
    state: CounterState,
    pending: PendingPublication,
    target: ReleaseRecord | None,
    expected_assets: frozenset[str],
    expected_digests: dict[str, str],
    operation_id: str,
    timestamp: str,
) -> tuple[PendingPublication, bool]:
    """Durably rebind a draft-only replacement and any failed-upload cleanup."""

    intent = pending.evidence_replacement
    if intent is None:
        raise CounterError("exact CI evidence replacement has no durable intent")
    if (
        target is None
        or target.release_id != pending.target_release_id
        or not target.draft
        or target.prerelease
    ):
        raise CounterError("exact CI evidence replacement is not bound to the same stable draft")

    _require_asset_name_subset(
        expected_assets, target.assets, label="evidence-replacement draft"
    )
    evidence_assets = [
        asset for asset in target.assets if asset.name == EXACT_CI_EVIDENCE_ASSET
    ]
    if len(evidence_assets) > 1:
        raise CounterError("evidence-replacement draft contains duplicate exact CI evidence assets")

    expected_digest = expected_digests[EXACT_CI_EVIDENCE_ASSET]
    cleanup = intent.cleanup
    evidence = evidence_assets[0] if evidence_assets else None
    if evidence is not None and evidence.asset_id == intent.old_asset_id:
        if cleanup is not None:
            raise CounterError("authorized old exact CI evidence asset reappeared after cleanup")
        if (
            evidence.state != "uploaded"
            or evidence.digest != intent.old_digest
            or evidence.download_count
            != state.assets[intent.old_asset_id].download_count
        ):
            raise CounterError("authorized old exact CI evidence asset changed before replacement")
        if expected_digest == intent.old_digest:
            rebound = replace(
                pending,
                operation_id=operation_id,
                prepared_asset_ids=frozenset(asset.asset_id for asset in target.assets),
                prepared_at=timestamp,
                evidence_replacement=None,
            )
            return rebound, False
        cleanup = None
    elif evidence is not None:
        if evidence.state == "uploaded" and evidence.digest == expected_digest:
            cleanup = None
        else:
            cleanup = EvidenceCleanupIntent(
                evidence.asset_id,
                evidence.digest,
                evidence.state,
                evidence.download_count,
            )

    rebound_intent = EvidenceReplacementIntent(
        intent.old_asset_id,
        intent.old_digest,
        expected_digest,
        cleanup,
    )
    rebound = replace(
        pending,
        operation_id=operation_id,
        prepared_at=timestamp,
        evidence_replacement=rebound_intent,
    )
    return rebound, True


def _validate_evidence_replacement_inventory(
    *,
    state: CounterState,
    pending: PendingPublication,
    target: ReleaseRecord | None,
    expected_assets: frozenset[str],
    expected_digests: dict[str, str],
) -> tuple[str, AssetRecord | None]:
    """Validate one durable stable-draft evidence replacement state."""

    intent = pending.evidence_replacement
    if intent is None:
        raise CounterError("exact CI evidence replacement has no durable intent")
    if (
        not pending.is_versioned
        or pending.phase != "prepared"
        or target is None
        or not target.draft
        or target.prerelease
        or target.release_id != pending.target_release_id
    ):
        raise CounterError("exact CI evidence replacement is not bound to the same stable draft")
    if expected_digests.get(EXACT_CI_EVIDENCE_ASSET) != intent.expected_digest:
        raise CounterError("exact CI evidence expected digest changed after replacement authorization")

    _require_asset_name_subset(
        expected_assets, target.assets, label="evidence-replacement draft"
    )
    current_evidence = [
        asset for asset in target.assets if asset.name == EXACT_CI_EVIDENCE_ASSET
    ]
    if len(current_evidence) > 1:
        raise CounterError("evidence-replacement draft contains duplicate exact CI evidence assets")

    old_id = intent.old_asset_id
    original_non_evidence_ids = pending.prepared_asset_ids - {old_id}
    current_non_evidence = {
        asset.asset_id: asset
        for asset in target.assets
        if asset.name != EXACT_CI_EVIDENCE_ASSET
    }
    if set(current_non_evidence) != original_non_evidence_ids:
        raise CounterError("non-evidence draft asset IDs changed during evidence replacement")
    for asset_id, asset in current_non_evidence.items():
        previous = state.assets.get(asset_id)
        if (
            previous is None
            or previous.release_id != target.release_id
            or previous.name != asset.name
            or previous.download_count != asset.download_count
            or asset.state != "uploaded"
            or asset.digest != expected_digests.get(asset.name)
        ):
            raise CounterError(
                f"non-evidence draft asset changed during evidence replacement: {asset.name}"
            )

    if not current_evidence:
        return "missing", None
    evidence = current_evidence[0]
    if evidence.asset_id == old_id:
        previous = state.assets.get(old_id)
        if (
            previous is None
            or previous.release_id != target.release_id
            or previous.name != EXACT_CI_EVIDENCE_ASSET
            or previous.download_count != evidence.download_count
            or evidence.state != "uploaded"
            or evidence.digest != intent.old_digest
        ):
            raise CounterError("authorized old exact CI evidence asset changed before replacement")
        return "old", evidence
    if evidence.state != "uploaded" or evidence.digest != intent.expected_digest:
        cleanup = intent.cleanup
        if (
            cleanup is None
            or evidence.asset_id != cleanup.asset_id
            or evidence.digest != cleanup.digest
            or evidence.state != cleanup.state
            or evidence.download_count != cleanup.download_count
        ):
            raise CounterError(
                "replacement exact CI evidence cleanup asset is not durably authorized"
            )
        return "cleanup", evidence
    previous_replacement = state.assets.get(evidence.asset_id)
    if previous_replacement is not None and (
        evidence.asset_id not in state.live_asset_ids
        or previous_replacement.release_id != evidence.release_id
        or previous_replacement.name != evidence.name
        or previous_replacement.download_count != evidence.download_count
        or previous_replacement.digest != evidence.digest
        or previous_replacement.state != evidence.state
    ):
        raise CounterError(
            "replacement exact CI evidence asset reuses a historical asset ID"
        )
    return "new", evidence


def inspect_publication_target(
    *,
    repository: str,
    is_versioned: bool,
    target_tag: str,
    expected_assets_file: Path,
    expected_digests_file: Path,
    api: GitHubApi,
    allow_exact_ci_evidence_replacement: bool = False,
    allow_stable_draft_recovery_candidates: bool = False,
) -> PublicationPreflightResult:
    """Validate the target before any release, tag, or asset mutation."""
    _validate_target_inputs(
        repository=repository, is_versioned=is_versioned, target_tag=target_tag
    )
    if allow_exact_ci_evidence_replacement and not is_versioned:
        raise CounterError(
            "replaceable draft assets are valid only for versioned publications"
        )
    if allow_stable_draft_recovery_candidates and not is_versioned:
        raise CounterError(
            "stable draft recovery candidates are valid only for versioned publications"
        )
    expected = _read_expected_assets(expected_assets_file)
    expected_digests = _read_expected_digests(expected_digests_file, expected)
    inventory = fetch_consistent_inventory(api, repository)
    target = inventory.release_by_tag(target_tag)
    if target is None:
        return PublicationPreflightResult(False, 0, None, False)
    _validate_existing_target(
        expected_assets=expected,
        expected_digests=expected_digests,
        target=target,
        is_versioned=is_versioned,
        allow_exact_ci_evidence_replacement=allow_exact_ci_evidence_replacement,
        allow_stable_draft_recovery_candidates=(
            allow_stable_draft_recovery_candidates
        ),
        allow_legacy_nightly_prerelease=not is_versioned,
    )
    return PublicationPreflightResult(
        True, len(target.assets), target.release_id, target.draft
    )


def preflight_publication(
    *,
    repository: str,
    is_versioned: bool,
    run_id: str,
    run_attempt: str,
    source_sha: str,
    target_tag: str,
    expected_assets_file: Path,
    expected_digests_file: Path,
    data_file: Path,
    api: GitHubApi,
    expect_target_draft: bool | None = None,
) -> PublicationPreflightResult:
    """Prove an existing target release is a safe, snapshotted publication base."""
    operation_id = _validate_publication_inputs(
        repository=repository, is_versioned=is_versioned, run_id=run_id,
        run_attempt=run_attempt, source_sha=source_sha, target_tag=target_tag,
    )
    loaded = load_state(data_file)
    if not isinstance(loaded, CounterState) or loaded.pending is None:
        raise CounterError("preflight requires a durable pending publication")
    pending = loaded.pending
    _require_pending_operation(
        pending,
        operation_id=operation_id,
        is_versioned=is_versioned,
        source_sha=source_sha,
        target_tag=target_tag,
        phase="prepared",
        label="preflight",
    )
    if pending.evidence_replacement is not None:
        raise CounterError(
            "ordinary preflight requires accepted exact CI evidence replacement state"
        )
    if pending.draft_cleanup:
        raise CounterError(
            "ordinary preflight requires accepted stable draft cleanup state"
        )
    if expect_target_draft is False and (
        is_versioned or pending.target_draft_at_prepare is not False
    ):
        raise CounterError(
            "pre-hide preflight requires an originally public nightly release"
        )

    expected = _read_expected_assets(expected_assets_file)
    expected_digests = _read_expected_digests(expected_digests_file, expected)
    inventory = fetch_consistent_inventory(api, repository)
    target = inventory.release_by_tag(target_tag)
    if target is None:
        if pending.target_release_id is not None or pending.prepared_asset_ids:
            raise CounterError("target release disappeared after the durable counter snapshot")
        return PublicationPreflightResult(False, 0, None, False)

    actual_asset_ids = frozenset(asset.asset_id for asset in target.assets)
    if (
        pending.target_release_id != target.release_id
        or pending.prepared_asset_ids != actual_asset_ids
    ):
        raise CounterError("target release assets changed after the durable counter snapshot")
    if is_versioned and pending.target_draft_at_prepare != target.draft:
        raise CounterError(
            "stable release draft state changed after the durable counter snapshot"
        )
    if expect_target_draft is not None and target.draft is not expect_target_draft:
        expected_visibility = "draft" if expect_target_draft else "public"
        raise CounterError(
            f"target release is not {expected_visibility} at the requested preflight boundary"
        )
    changed_counts = sorted(
        asset.name
        for asset in target.assets
        if loaded.assets[asset.asset_id].download_count != asset.download_count
    )
    if changed_counts:
        raise CounterError(
            "target release download counts changed after the durable counter snapshot: "
            + ", ".join(changed_counts)
        )

    _validate_existing_target(
        expected_assets=expected,
        expected_digests=expected_digests,
        target=target,
        is_versioned=is_versioned,
    )
    if not is_versioned and expect_target_draft is not False and not target.draft:
        raise CounterError(
            "existing nightly release is not draft after the pre-publication hide"
        )
    return PublicationPreflightResult(
        True, len(target.assets), target.release_id, target.draft
    )


def preflight_evidence_replacement(
    *,
    repository: str,
    is_versioned: bool,
    run_id: str,
    run_attempt: str,
    source_sha: str,
    target_tag: str,
    expected_assets_file: Path,
    expected_digests_file: Path,
    data_file: Path,
    api: GitHubApi,
) -> EvidenceReplacementPreflightResult:
    """Prove the exact draft state authorized by a durable replacement intent."""

    operation_id = _validate_publication_inputs(
        repository=repository,
        is_versioned=is_versioned,
        run_id=run_id,
        run_attempt=run_attempt,
        source_sha=source_sha,
        target_tag=target_tag,
    )
    if not is_versioned:
        raise CounterError("exact CI evidence replacement is valid only for stable releases")
    loaded = load_state(data_file)
    if not isinstance(loaded, CounterState) or loaded.pending is None:
        raise CounterError("evidence replacement preflight requires a durable pending publication")
    pending = loaded.pending
    _require_pending_operation(
        pending,
        operation_id=operation_id,
        is_versioned=is_versioned,
        source_sha=source_sha,
        target_tag=target_tag,
        phase="prepared",
        label="evidence replacement preflight",
    )
    if pending.evidence_replacement is None:
        return EvidenceReplacementPreflightResult(
            False, False, False, None, None, None, None, None
        )
    expected = _read_expected_assets(expected_assets_file)
    expected_digests = _read_expected_digests(expected_digests_file, expected)
    inventory = fetch_consistent_inventory(api, repository)
    target = inventory.release_by_tag(target_tag)
    replacement_state, replacement_asset = _validate_evidence_replacement_inventory(
        state=loaded,
        pending=pending,
        target=target,
        expected_assets=expected,
        expected_digests=expected_digests,
    )
    delete_asset = (
        replacement_asset
        if replacement_state in {"old", "cleanup"}
        else None
    )
    return EvidenceReplacementPreflightResult(
        True,
        replacement_state in {"old", "cleanup"},
        replacement_state in {"old", "missing", "cleanup"},
        target.release_id if target else None,
        delete_asset.asset_id if delete_asset else None,
        delete_asset.digest if delete_asset else None,
        delete_asset.state if delete_asset else None,
        delete_asset.download_count if delete_asset else None,
    )


def accept_evidence_replacement(
    *,
    repository: str,
    is_versioned: bool,
    run_id: str,
    run_attempt: str,
    source_sha: str,
    target_tag: str,
    expected_assets_file: Path,
    expected_digests_file: Path,
    data_file: Path,
    badge_directory: Path,
    api: GitHubApi,
    now: Callable[[], datetime] = lambda: datetime.now(timezone.utc),
) -> PublicationResult:
    """Accept one uploaded replacement and durably reconcile its new asset ID."""

    operation_id = _validate_publication_inputs(
        repository=repository,
        is_versioned=is_versioned,
        run_id=run_id,
        run_attempt=run_attempt,
        source_sha=source_sha,
        target_tag=target_tag,
    )
    if not is_versioned:
        raise CounterError("exact CI evidence replacement is valid only for stable releases")
    loaded = load_state(data_file)
    if not isinstance(loaded, CounterState) or loaded.pending is None:
        raise CounterError("evidence replacement accept requires a durable pending publication")
    pending = loaded.pending
    _require_pending_operation(
        pending,
        operation_id=operation_id,
        is_versioned=is_versioned,
        source_sha=source_sha,
        target_tag=target_tag,
        phase="prepared",
        label="evidence replacement accept",
    )
    if pending.evidence_replacement is None:
        raise CounterError("evidence replacement accept requires a durable replacement intent")
    expected = _read_expected_assets(expected_assets_file)
    expected_digests = _read_expected_digests(expected_digests_file, expected)
    inventory = fetch_consistent_inventory(api, repository)
    target = inventory.release_by_tag(target_tag)
    replacement_state, replacement_asset = _validate_evidence_replacement_inventory(
        state=loaded,
        pending=pending,
        target=target,
        expected_assets=expected,
        expected_digests=expected_digests,
    )
    if replacement_state != "new" or replacement_asset is None:
        raise CounterError("exact CI evidence replacement is not fully uploaded")
    allowed_missing = _replacement_live_missing_ids(
        loaded, pending.evidence_replacement
    )
    timestamp = _utc_now(now)
    state = reconcile_inventory(
        loaded,
        inventory,
        allowed_missing_asset_ids=allowed_missing,
        updated=timestamp,
    )
    accepted_pending = replace(
        pending,
        prepared_asset_ids=frozenset(asset.asset_id for asset in target.assets),
        prepared_at=timestamp,
        evidence_replacement=None,
    )
    state = replace(state, pending=accepted_pending)
    _write_files(
        _counter_files(state, data_file=data_file, badge_directory=badge_directory)
    )
    return PublicationResult(
        state.total,
        state.installer_total,
        sum(asset.download_count for asset in target.assets),
        operation_id,
        True,
        False,
    )


def stage_publication(
    *,
    repository: str,
    is_versioned: bool,
    run_id: str,
    run_attempt: str,
    source_sha: str,
    target_tag: str,
    expected_assets_file: Path,
    expected_digests_file: Path,
    data_file: Path,
    api: GitHubApi,
    now: Callable[[], datetime] = lambda: datetime.now(timezone.utc),
) -> PublicationResult:
    operation_id = _validate_publication_inputs(
        repository=repository, is_versioned=is_versioned, run_id=run_id,
        run_attempt=run_attempt, source_sha=source_sha, target_tag=target_tag,
    )
    loaded = load_state(data_file)
    if not isinstance(loaded, CounterState):
        raise CounterError("stage requires schema-v2 state")
    pending = loaded.pending
    if pending is None:
        raise CounterError("stage requires a durable pending publication")
    _require_pending_operation(
        pending,
        operation_id=operation_id,
        is_versioned=is_versioned,
        source_sha=source_sha,
        target_tag=target_tag,
        phase="prepared",
        label="stage",
    )
    if pending.evidence_replacement is not None:
        raise CounterError("stage requires accepted exact CI evidence replacement state")
    if pending.draft_cleanup:
        raise CounterError("stage requires accepted stable draft cleanup state")
    expected = _read_expected_assets(expected_assets_file)
    expected_digests = _read_expected_digests(expected_digests_file, expected)
    timestamp = _utc_now(now)
    inventory = fetch_consistent_inventory(api, repository)
    state = reconcile_inventory(
        loaded, inventory,
        allowed_missing_asset_ids=pending.prepared_asset_ids,
        updated=timestamp,
    )
    target = inventory.release_by_tag(target_tag)
    if target is None:
        raise CounterError(f"staged release {target_tag} is absent")
    _require_channel_prerelease(
        target, is_versioned=is_versioned, label="staged target"
    )
    if is_versioned:
        if (
            pending.target_release_id is not None
            and pending.target_release_id != target.release_id
        ):
            raise CounterError("stable release identity changed after preflight")
        if pending.target_release_id is None:
            if not target.draft:
                raise CounterError("new stable release must be staged as a draft")
        elif pending.target_draft_at_prepare != target.draft:
            raise CounterError("stable release draft state changed before staging")
        actual_asset_ids = frozenset(asset.asset_id for asset in target.assets)
        if pending.target_draft_at_prepare is True:
            if not pending.prepared_asset_ids.issubset(actual_asset_ids):
                raise CounterError(
                    "stable draft replaced an existing asset after preflight"
                )
        elif pending.target_draft_at_prepare is False:
            if pending.prepared_asset_ids != actual_asset_ids:
                raise CounterError(
                    "published stable no-op assets changed after preflight"
                )
    elif not target.draft:
        raise CounterError("staged nightly release must remain a draft")
    _require_exact_asset_set(expected, target.assets, label="staged release")
    _require_exact_asset_digests(
        expected_digests, target.assets, label="staged release"
    )
    state = replace(
        state,
        pending=PendingPublication(
            operation_id=operation_id,
            target_tag=target_tag,
            is_versioned=is_versioned,
            source_sha=source_sha.lower(),
            phase="staged",
            target_release_id=target.release_id,
            prepared_asset_ids=frozenset(asset.asset_id for asset in target.assets),
            prepared_at=pending.prepared_at,
            target_draft_at_prepare=pending.target_draft_at_prepare,
        ),
    )
    _write_files({data_file: _serialize_json(_state_payload(state))})
    target_downloads = sum(asset.download_count for asset in target.assets)
    return PublicationResult(
        state.total, state.installer_total, target_downloads, operation_id, False
    )


def prepublish_publication(
    *,
    repository: str,
    is_versioned: bool,
    run_id: str,
    run_attempt: str,
    source_sha: str,
    target_tag: str,
    expected_assets_file: Path,
    expected_digests_file: Path,
    data_file: Path,
    api: GitHubApi,
    expect_draft: bool = True,
) -> PublicationPreflightResult:
    """Prove the complete release still matches its durable staged checkpoint."""

    operation_id = _validate_publication_inputs(
        repository=repository,
        is_versioned=is_versioned,
        run_id=run_id,
        run_attempt=run_attempt,
        source_sha=source_sha,
        target_tag=target_tag,
    )
    loaded = load_state(data_file)
    if not isinstance(loaded, CounterState) or loaded.pending is None:
        raise CounterError("publish preflight requires a durable staged publication")
    pending = loaded.pending
    _require_pending_operation(
        pending,
        operation_id=operation_id,
        is_versioned=is_versioned,
        source_sha=source_sha,
        target_tag=target_tag,
        phase="staged",
        label="publish preflight",
    )
    if pending.evidence_replacement is not None:
        raise CounterError("publish preflight found unresolved evidence replacement")
    if pending.draft_cleanup:
        raise CounterError("publish preflight found unresolved stable draft cleanup")

    expected = _read_expected_assets(expected_assets_file)
    expected_digests = _read_expected_digests(expected_digests_file, expected)
    inventory = fetch_consistent_inventory(api, repository)
    reconcile_inventory(loaded, inventory, updated=loaded.updated)
    target = inventory.release_by_tag(target_tag)
    if target is None:
        raise CounterError(f"staged release {target_tag} disappeared before publication")
    if target.draft is not expect_draft:
        if expect_draft:
            raise CounterError("staged release became public before the authorized publish")
        raise CounterError("published release returned to draft or never became public")
    _require_channel_prerelease(
        target, is_versioned=is_versioned, label="publish-preflight target"
    )
    actual_asset_ids = frozenset(asset.asset_id for asset in target.assets)
    if (
        target.release_id != pending.target_release_id
        or actual_asset_ids != pending.prepared_asset_ids
    ):
        raise CounterError("staged release identity or asset IDs changed before publication")
    changed_counts = sorted(
        asset.name
        for asset in target.assets
        if loaded.assets.get(asset.asset_id) is None
        or (
            loaded.assets[asset.asset_id].download_count != asset.download_count
            if expect_draft
            else loaded.assets[asset.asset_id].download_count > asset.download_count
        )
    )
    if changed_counts:
        raise CounterError(
            (
                "staged release download counts changed before publication: "
                if expect_draft
                else "published release download counts regressed after publication: "
            )
            + ", ".join(changed_counts)
        )
    _require_exact_asset_set(expected, target.assets, label="publish-preflight release")
    _require_exact_asset_digests(
        expected_digests, target.assets, label="publish-preflight release"
    )
    return PublicationPreflightResult(
        True, len(target.assets), target.release_id, target.draft
    )


def complete_publication(
    *,
    repository: str,
    is_versioned: bool,
    run_id: str,
    run_attempt: str,
    source_sha: str,
    target_tag: str,
    expected_assets_file: Path,
    expected_digests_file: Path,
    data_file: Path,
    badge_directory: Path,
    api: GitHubApi,
    now: Callable[[], datetime] = lambda: datetime.now(timezone.utc),
) -> PublicationResult:
    operation_id = _validate_publication_inputs(
        repository=repository, is_versioned=is_versioned, run_id=run_id,
        run_attempt=run_attempt, source_sha=source_sha, target_tag=target_tag,
    )
    loaded = load_state(data_file)
    if not isinstance(loaded, CounterState):
        raise CounterError("complete requires schema-v2 state")
    pending = loaded.pending
    if pending is None:
        raise CounterError("complete requires a durable pending publication")
    _require_pending_operation(
        pending,
        operation_id=operation_id,
        is_versioned=is_versioned,
        source_sha=source_sha,
        target_tag=target_tag,
        phase="staged",
        label="complete",
    )
    expected = _read_expected_assets(expected_assets_file)
    expected_digests = _read_expected_digests(expected_digests_file, expected)
    timestamp = _utc_now(now)
    inventory = fetch_consistent_inventory(api, repository)
    state = reconcile_inventory(loaded, inventory, updated=timestamp)
    target = inventory.release_by_tag(target_tag)
    if target is None:
        raise CounterError(f"published release {target_tag} is absent")
    _require_channel_prerelease(
        target, is_versioned=is_versioned, label="published target"
    )
    actual_asset_ids = frozenset(asset.asset_id for asset in target.assets)
    if (
        pending.target_release_id != target.release_id
        or pending.prepared_asset_ids != actual_asset_ids
    ):
        raise CounterError("published release assets changed after staging")
    if target.draft:
        raise CounterError("published release remains a draft")
    _require_exact_asset_set(expected, target.assets, label="published release")
    _require_exact_asset_digests(
        expected_digests, target.assets, label="published release"
    )
    state = replace(state, pending=None)
    files = _counter_files(state, data_file=data_file, badge_directory=badge_directory)
    target_downloads = sum(asset.download_count for asset in target.assets)
    badge_name, badge_label, badge_color = (
        ("stable-downloads.json", "release downloads", "blue")
        if is_versioned
        else ("nightly-downloads.json", "nightly downloads", "brightgreen")
    )
    files[badge_directory / badge_name] = _serialize_json(
        _badge(badge_label, f"{target_downloads:,}", badge_color)
    )
    _write_files(files)
    return PublicationResult(
        state.total, state.installer_total, target_downloads, operation_id, False
    )


def _report_publication_target(result: PublicationPreflightResult) -> None:
    print(
        "Existing target release: "
        + ("present" if result.target_exists else "absent (first publication)")
    )
    print(f"Existing target assets: {result.target_asset_count}")
    output_path = os.environ.get("GITHUB_OUTPUT")
    if not output_path:
        return
    with open(output_path, "a", encoding="utf-8", newline="\n") as output:
        output.write(f"target_exists={'true' if result.target_exists else 'false'}\n")
        output.write(f"target_asset_count={result.target_asset_count}\n")
        output.write(
            "target_release_id="
            + (str(result.target_release_id) if result.target_release_id else "")
            + "\n"
        )
        output.write(
            f"target_is_draft={'true' if result.target_is_draft else 'false'}\n"
        )


def _report_evidence_replacement(
    result: EvidenceReplacementPreflightResult,
) -> None:
    print(
        "Exact CI evidence replacement: "
        + ("required" if result.replacement_required else "not required")
    )
    output_path = os.environ.get("GITHUB_OUTPUT")
    if not output_path:
        return
    with open(output_path, "a", encoding="utf-8", newline="\n") as output:
        output.write(
            f"replacement_required={'true' if result.replacement_required else 'false'}\n"
        )
        output.write(
            f"delete_required={'true' if result.delete_required else 'false'}\n"
        )
        output.write(
            f"upload_required={'true' if result.upload_required else 'false'}\n"
        )
        output.write(
            "target_release_id="
            + (str(result.target_release_id) if result.target_release_id else "")
            + "\n"
        )
        output.write(
            "delete_evidence_asset_id="
            + (str(result.delete_asset_id) if result.delete_asset_id else "")
            + "\n"
        )
        output.write(
            "delete_evidence_asset_digest="
            + (result.delete_asset_digest or "")
            + "\n"
        )
        output.write(
            "delete_evidence_asset_state="
            + (result.delete_asset_state or "")
            + "\n"
        )
        output.write(
            "delete_evidence_download_count="
            + (
                str(result.delete_asset_download_count)
                if result.delete_asset_download_count is not None
                else ""
            )
            + "\n"
        )


def _report_draft_cleanup(result: DraftCleanupPreflightResult) -> None:
    payload = [
        {
            "assetId": cleanup.asset_id,
            "name": cleanup.name,
            "digest": cleanup.digest,
            "state": cleanup.state,
            "downloadCount": cleanup.download_count,
        }
        for cleanup in result.cleanup_assets
    ]
    print(f"Stable draft cleanup assets still present: {len(payload)}")
    output_path = os.environ.get("GITHUB_OUTPUT")
    if not output_path:
        return
    with open(output_path, "a", encoding="utf-8", newline="\n") as output:
        output.write(
            f"cleanup_required={'true' if result.cleanup_required else 'false'}\n"
        )
        output.write(
            "target_release_id="
            + (str(result.target_release_id) if result.target_release_id else "")
            + "\n"
        )
        output.write(
            "cleanup_assets="
            + json.dumps(payload, ensure_ascii=True, separators=(",", ":"))
            + "\n"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "phase",
        choices=(
            "inspect",
            "preflight",
            "pre-hide-preflight",
            "prepare",
            "cleanup-preflight",
            "cleanup-accept",
            "replacement-preflight",
            "replacement-accept",
            "stage",
            "publish-preflight",
            "post-publish-preflight",
            "complete",
            "refresh",
        ),
    )
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY"))
    parser.add_argument(
        "--is-versioned", choices=("true", "false"),
        default=os.environ.get("IS_VERSIONED"),
    )
    parser.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID"))
    parser.add_argument("--run-attempt", default=os.environ.get("GITHUB_RUN_ATTEMPT"))
    parser.add_argument("--source-sha", default=os.environ.get("GITHUB_SHA"))
    parser.add_argument("--target-tag", default=os.environ.get("RELEASE_TAG"))
    parser.add_argument(
        "--data-file", type=Path, default=Path(".github/badges/downloads-data.json")
    )
    parser.add_argument(
        "--badge-directory", type=Path, default=Path(".github/badges")
    )
    parser.add_argument("--expected-assets-file", type=Path)
    parser.add_argument("--expected-digests-file", type=Path)
    parser.add_argument(
        "--allow-exact-ci-evidence-replacement",
        action="store_true",
        help="authorize only the fixed exact-CI evidence asset on an unpublished stable draft",
    )
    parser.add_argument(
        "--allow-stable-draft-recovery-candidates",
        action="store_true",
        help="permit read-only inspect of expected-name failed uploads on a stable draft",
    )
    parser.add_argument("--initialize", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        api = GitHubApi(os.environ.get("GH_TOKEN", ""))
        if args.phase == "refresh":
            if args.repository is None:
                raise CounterError("missing required argument: repository")
            if args.initialize:
                raise CounterError("--initialize is valid only during prepare")
            if (
                args.expected_assets_file is not None
                or args.expected_digests_file is not None
                or args.allow_exact_ci_evidence_replacement
                or args.allow_stable_draft_recovery_candidates
            ):
                raise CounterError("expected asset files are not valid during refresh")
            refreshed = refresh_counters(
                repository=args.repository,
                data_file=args.data_file,
                badge_directory=args.badge_directory,
                api=api,
            )
            print(f"Lifetime downloads: {refreshed.total}")
            print(f"Installer downloads: {refreshed.installer_total}")
            output_path = os.environ.get("GITHUB_OUTPUT")
            if output_path:
                with open(output_path, "a", encoding="utf-8", newline="\n") as output:
                    output.write(f"new_total={refreshed.total}\n")
                    output.write(f"installer_total={refreshed.installer_total}\n")
            return 0

        if args.phase == "inspect":
            required = {
                "repository": args.repository,
                "is-versioned": args.is_versioned,
                "target-tag": args.target_tag,
            }
            missing = [name for name, value in required.items() if value is None]
            if missing:
                raise CounterError("missing required arguments: " + ", ".join(missing))
            if args.initialize:
                raise CounterError("--initialize is valid only during prepare")
            if args.expected_assets_file is None:
                raise CounterError("--expected-assets-file is required during inspect")
            if args.expected_digests_file is None:
                raise CounterError("--expected-digests-file is required during inspect")
            result = inspect_publication_target(
                repository=args.repository,
                is_versioned=args.is_versioned == "true",
                target_tag=args.target_tag,
                expected_assets_file=args.expected_assets_file,
                expected_digests_file=args.expected_digests_file,
                api=api,
                allow_exact_ci_evidence_replacement=(
                    args.allow_exact_ci_evidence_replacement
                ),
                allow_stable_draft_recovery_candidates=(
                    args.allow_stable_draft_recovery_candidates
                ),
            )
            _report_publication_target(result)
            return 0

        required = {
            "repository": args.repository, "is-versioned": args.is_versioned,
            "run-id": args.run_id, "run-attempt": args.run_attempt,
            "source-sha": args.source_sha, "target-tag": args.target_tag,
        }
        missing = [name for name, value in required.items() if value is None]
        if missing:
            raise CounterError("missing required arguments: " + ", ".join(missing))
        identity = dict(
            repository=args.repository,
            is_versioned=args.is_versioned == "true",
            run_id=args.run_id,
            run_attempt=args.run_attempt,
            source_sha=args.source_sha,
            target_tag=args.target_tag,
        )
        if args.phase in {
            "preflight",
            "pre-hide-preflight",
            "publish-preflight",
            "post-publish-preflight",
        }:
            if (
                args.allow_exact_ci_evidence_replacement
                or args.allow_stable_draft_recovery_candidates
            ):
                raise CounterError(
                    "--allow-exact-ci-evidence-replacement is valid only during inspect or prepare"
                )
            if args.initialize:
                raise CounterError("--initialize is valid only during prepare")
            if args.expected_assets_file is None:
                raise CounterError(
                    f"--expected-assets-file is required during {args.phase}"
                )
            if args.expected_digests_file is None:
                raise CounterError(
                    f"--expected-digests-file is required during {args.phase}"
                )
            common = dict(
                **identity,
                expected_assets_file=args.expected_assets_file,
                expected_digests_file=args.expected_digests_file,
                data_file=args.data_file,
                api=api,
            )
            if args.phase in {"preflight", "pre-hide-preflight"}:
                result = preflight_publication(
                    **common,
                    expect_target_draft=(
                        False if args.phase == "pre-hide-preflight" else None
                    ),
                )
            else:
                result = prepublish_publication(
                    **common, expect_draft=args.phase == "publish-preflight"
                )
            _report_publication_target(result)
            return 0

        if args.phase == "replacement-preflight":
            if (
                args.initialize
                or args.allow_exact_ci_evidence_replacement
                or args.allow_stable_draft_recovery_candidates
            ):
                raise CounterError("replacement preflight has no initialization or broad authorization flag")
            if args.expected_assets_file is None or args.expected_digests_file is None:
                raise CounterError("replacement preflight requires expected asset and digest files")
            replacement = preflight_evidence_replacement(
                **identity,
                expected_assets_file=args.expected_assets_file,
                expected_digests_file=args.expected_digests_file,
                data_file=args.data_file,
                api=api,
            )
            _report_evidence_replacement(replacement)
            return 0

        if args.phase == "cleanup-preflight":
            if (
                args.initialize
                or args.allow_exact_ci_evidence_replacement
                or args.allow_stable_draft_recovery_candidates
            ):
                raise CounterError(
                    "cleanup preflight has no initialization or broad authorization flag"
                )
            if args.expected_assets_file is None or args.expected_digests_file is None:
                raise CounterError(
                    "cleanup preflight requires expected asset and digest files"
                )
            cleanup = preflight_draft_cleanup(
                **identity,
                expected_assets_file=args.expected_assets_file,
                expected_digests_file=args.expected_digests_file,
                data_file=args.data_file,
                api=api,
            )
            _report_draft_cleanup(cleanup)
            return 0

        if args.phase == "prepare":
            if args.allow_stable_draft_recovery_candidates:
                raise CounterError(
                    "--allow-stable-draft-recovery-candidates is valid only during inspect"
                )
            result = prepare_publication(
                **identity,
                data_file=args.data_file,
                badge_directory=args.badge_directory,
                api=api,
                initialize=args.initialize,
                expected_assets_file=args.expected_assets_file,
                expected_digests_file=args.expected_digests_file,
                allow_exact_ci_evidence_replacement=(
                    args.allow_exact_ci_evidence_replacement
                ),
            )
        else:
            if (
                args.allow_exact_ci_evidence_replacement
                or args.allow_stable_draft_recovery_candidates
            ):
                raise CounterError(
                    "--allow-exact-ci-evidence-replacement is valid only during inspect or prepare"
                )
            if args.initialize:
                raise CounterError("--initialize is valid only during prepare")
            if args.expected_assets_file is None:
                raise CounterError(
                    f"--expected-assets-file is required during {args.phase}"
                )
            if args.expected_digests_file is None:
                raise CounterError(
                    f"--expected-digests-file is required during {args.phase}"
                )
            phase_common = dict(
                **identity,
                expected_assets_file=args.expected_assets_file,
                expected_digests_file=args.expected_digests_file,
                data_file=args.data_file,
                api=api,
            )
            if args.phase == "replacement-accept":
                result = accept_evidence_replacement(
                    **phase_common, badge_directory=args.badge_directory
                )
            elif args.phase == "cleanup-accept":
                result = accept_draft_cleanup(
                    **phase_common, badge_directory=args.badge_directory
                )
            elif args.phase == "stage":
                result = stage_publication(**phase_common)
            else:
                result = complete_publication(
                    **phase_common, badge_directory=args.badge_directory
                )
        print(f"Lifetime downloads: {result.total}")
        print(f"Installer downloads: {result.installer_total}")
        print(f"Target release downloads: {result.target_downloads}")
        print(f"Publication operation: {result.operation_id}")
        if result.recovered_pending:
            print("Recovered an interrupted pending publication")
        output_path = os.environ.get("GITHUB_OUTPUT")
        if output_path:
            with open(output_path, "a", encoding="utf-8", newline="\n") as output:
                output.write(f"new_total={result.total}\n")
                output.write(f"installer_total={result.installer_total}\n")
                output.write(f"target_downloads={result.target_downloads}\n")
                output.write(f"operation_id={result.operation_id}\n")
                output.write(
                    f"recovered_pending={'true' if result.recovered_pending else 'false'}\n"
                )
                output.write(
                    f"replacement_required={'true' if result.replacement_required else 'false'}\n"
                )
                output.write(
                    f"cleanup_required={'true' if result.cleanup_required else 'false'}\n"
                )
        return 0
    except (CounterError, OSError) as exc:
        print(f"error: {exc}", file=os.sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
