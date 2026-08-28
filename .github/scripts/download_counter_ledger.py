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


@dataclass(frozen=True)
class ReleaseRecord:
    release_id: int
    tag_name: str
    assets: tuple[AssetRecord, ...]
    draft: bool = False


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
class PendingPublication:
    operation_id: str
    target_tag: str
    is_versioned: bool
    source_sha: str
    target_release_id: int | None
    prepared_asset_ids: frozenset[int]
    prepared_at: str


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
        allowed={"id", "releaseId", "name", "downloadCount"},
        required={"id", "releaseId", "name", "downloadCount"},
        label=label,
    )
    name = value["name"]
    if not isinstance(name, str) or not name or "\n" in name or "\r" in name:
        raise CounterError(f"{label}.name must be a non-empty single-line string")
    return AssetRecord(
        _positive_integer(value["id"], f"{label}.id"),
        _positive_integer(value["releaseId"], f"{label}.releaseId"),
        name,
        _nonnegative_integer(value["downloadCount"], f"{label}.downloadCount"),
    )


def _parse_pending(value: Any, assets: dict[int, AssetRecord]) -> PendingPublication | None:
    if value is None:
        return None
    if not isinstance(value, dict):
        raise CounterError("state.pending must be an object or null")
    keys = {
        "operationId", "targetTag", "isVersioned", "sourceSha",
        "targetReleaseId", "preparedAssetIds", "preparedAt",
    }
    _require_keys(value, allowed=keys, required=keys, label="state.pending")
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
    return PendingPublication(
        operation_id, target_tag, is_versioned, source_sha.lower(), release_id,
        prepared, _timestamp(value["preparedAt"], "state.pending.preparedAt"),
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
    if pending is not None and not pending.prepared_asset_ids.issubset(live):
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
            "targetReleaseId": state.pending.target_release_id,
            "preparedAssetIds": sorted(state.pending.prepared_asset_ids),
            "preparedAt": state.pending.prepared_at,
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
            if asset_id in all_asset_ids:
                raise CounterError(f"GitHub inventory repeats asset ID {asset_id}")
            all_asset_ids.add(asset_id)
            release_assets.append(
                AssetRecord(asset_id, release_id, name, count, digest)
            )
        releases.append(
            ReleaseRecord(release_id, tag_name, tuple(release_assets), draft)
        )
    return Inventory(tuple(releases))


def _inventory_shape(
    inventory: Inventory,
) -> dict[int, tuple[str, bool, dict[int, tuple[str, str | None]]]]:
    return {
        release.release_id: (
            release.tag_name,
            release.draft,
            {
                asset.asset_id: (asset.name, asset.digest)
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


@dataclass(frozen=True)
class PublicationPreflightResult:
    target_exists: bool
    target_asset_count: int
    target_release_id: int | None
    target_is_draft: bool


@dataclass(frozen=True)
class CounterRefreshResult:
    total: int
    installer_total: int


def _validate_publication_inputs(
    *, repository: str, is_versioned: bool, run_id: str, run_attempt: str,
    source_sha: str, target_tag: str,
) -> str:
    if not REPOSITORY_RE.fullmatch(repository):
        raise CounterError(f"invalid GitHub repository name: {repository!r}")
    run_id = _decimal_string(run_id, "GITHUB_RUN_ID")
    run_attempt = _decimal_string(run_attempt, "GITHUB_RUN_ATTEMPT")
    if not SHA_RE.fullmatch(source_sha):
        raise CounterError("GITHUB_SHA must be a full commit SHA")
    _validate_tag(target_tag, is_versioned, "target tag")
    return f"{run_id}:{run_attempt}"


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
    now: Callable[[], datetime] = lambda: datetime.now(timezone.utc),
) -> PublicationResult:
    operation_id = _validate_publication_inputs(
        repository=repository, is_versioned=is_versioned, run_id=run_id,
        run_attempt=run_attempt, source_sha=source_sha, target_tag=target_tag,
    )
    timestamp = _utc_now(now)
    loaded = load_state(data_file, initialize=initialize)
    inventory = fetch_consistent_inventory(api, repository)
    recovered = isinstance(loaded, CounterState) and loaded.pending is not None
    if isinstance(loaded, LegacyState):
        state = _migrate_legacy(loaded, inventory, updated=timestamp)
    else:
        allowed = loaded.pending.prepared_asset_ids if loaded.pending else frozenset()
        state = reconcile_inventory(
            loaded, inventory, allowed_missing_asset_ids=allowed, updated=timestamp
        )
        state = replace(state, pending=None)
    target = inventory.release_by_tag(target_tag)
    target_assets = target.assets if target else ()
    state = replace(
        state,
        pending=PendingPublication(
            operation_id, target_tag, is_versioned, source_sha.lower(),
            target.release_id if target else None,
            frozenset(asset.asset_id for asset in target_assets), timestamp,
        ),
    )
    _write_files(_counter_files(state, data_file=data_file, badge_directory=badge_directory))
    return PublicationResult(
        state.total, state.installer_total,
        sum(asset.download_count for asset in target_assets), operation_id, recovered,
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


def _require_existing_nightly_subset(
    expected: frozenset[str], assets: tuple[AssetRecord, ...]
) -> None:
    actual_names = [asset.name for asset in assets]
    if len(actual_names) != len(set(actual_names)):
        raise CounterError("existing nightly release contains duplicate asset names")
    unexpected = set(actual_names) - expected
    if unexpected:
        raise CounterError(
            "existing nightly release contains unexpected assets: "
            + ", ".join(sorted(unexpected))
        )


def _require_exact_asset_digests(
    expected: dict[str, str], assets: tuple[AssetRecord, ...], *, label: str
) -> None:
    actual = {asset.name: asset.digest for asset in assets}
    missing_digests = sorted(name for name, digest in actual.items() if digest is None)
    mismatched = sorted(
        name
        for name, digest in actual.items()
        if digest is not None and expected.get(name) != digest
    )
    if missing_digests or mismatched:
        details: list[str] = []
        if missing_digests:
            details.append("missing remote digest: " + ", ".join(missing_digests))
        if mismatched:
            details.append("digest mismatch: " + ", ".join(mismatched))
        raise CounterError(f"{label} asset digests differ (" + "; ".join(details) + ")")


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
    if (
        pending.operation_id != operation_id or pending.target_tag != target_tag
        or pending.is_versioned != is_versioned
        or pending.source_sha != source_sha.lower()
    ):
        raise CounterError("preflight does not match the durable pending publication")

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

    if is_versioned:
        _require_exact_asset_set(expected, target.assets, label="existing release")
        _require_exact_asset_digests(
            expected_digests, target.assets, label="existing release"
        )
    else:
        # Rolling nightlies may add newly introduced aliases/metadata, but an
        # asset removed from the desired contract is stale and must be handled
        # by an explicit migration rather than silently surviving forever.
        _require_existing_nightly_subset(expected, target.assets)
    return PublicationPreflightResult(
        True, len(target.assets), target.release_id, target.draft
    )


def finalize_publication(
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
        raise CounterError("finalize requires schema-v2 state")
    pending = loaded.pending
    if pending is None:
        raise CounterError("finalize requires a durable pending publication")
    if (
        pending.operation_id != operation_id or pending.target_tag != target_tag
        or pending.is_versioned != is_versioned
        or pending.source_sha != source_sha.lower()
    ):
        raise CounterError("finalize does not match the durable pending publication")
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
        raise CounterError(f"published release {target_tag} is absent")
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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "phase", choices=("preflight", "prepare", "finalize", "refresh")
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
        if args.phase == "preflight":
            if args.initialize:
                raise CounterError("--initialize is valid only during prepare")
            if args.expected_assets_file is None:
                raise CounterError("--expected-assets-file is required during preflight")
            if args.expected_digests_file is None:
                raise CounterError("--expected-digests-file is required during preflight")
            result = preflight_publication(
                **identity,
                expected_assets_file=args.expected_assets_file,
                expected_digests_file=args.expected_digests_file,
                data_file=args.data_file,
                api=api,
            )
            print(
                "Existing target release: "
                + ("present" if result.target_exists else "absent (first publication)")
            )
            print(f"Existing target assets: {result.target_asset_count}")
            output_path = os.environ.get("GITHUB_OUTPUT")
            if output_path:
                with open(output_path, "a", encoding="utf-8", newline="\n") as output:
                    output.write(
                        f"target_exists={'true' if result.target_exists else 'false'}\n"
                    )
                    output.write(f"target_asset_count={result.target_asset_count}\n")
                    output.write(
                        "target_release_id="
                        + (
                            str(result.target_release_id)
                            if result.target_release_id
                            else ""
                        )
                        + "\n"
                    )
                    output.write(
                        f"target_is_draft={'true' if result.target_is_draft else 'false'}\n"
                    )
            return 0

        common = dict(
            **identity,
            data_file=args.data_file,
            badge_directory=args.badge_directory,
            api=api,
        )
        if args.phase == "prepare":
            result = prepare_publication(**common, initialize=args.initialize)
        else:
            if args.initialize:
                raise CounterError("--initialize is valid only during prepare")
            if args.expected_assets_file is None:
                raise CounterError("--expected-assets-file is required during finalize")
            if args.expected_digests_file is None:
                raise CounterError("--expected-digests-file is required during finalize")
            result = finalize_publication(
                **common,
                expected_assets_file=args.expected_assets_file,
                expected_digests_file=args.expected_digests_file,
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
        return 0
    except (CounterError, OSError) as exc:
        print(f"error: {exc}", file=os.sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
