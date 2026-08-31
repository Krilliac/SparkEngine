#!/usr/bin/env python3
"""Fail-closed verification of the exact GitHub release asset boundary."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
from typing import Any


EXPECTED_UPLOADER_ID = 41898282
EXPECTED_UPLOADER_LOGIN = "github-actions[bot]"
SHA256_LINE_RE = re.compile(r"([0-9a-fA-F]{64})  ([^\r\n]+)")
SHA256_DIGEST_RE = re.compile(r"sha256:[0-9a-f]{64}")


class BoundaryError(ValueError):
    """Raised when release metadata is not the exact staged publication."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise BoundaryError(message)


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise BoundaryError(f"JSON object repeats key {key!r}")
        value[key] = item
    return value


def _exact_int(value: Any, label: str, *, positive: bool = False) -> int:
    minimum = 1 if positive else 0
    if type(value) is not int or value < minimum:
        qualifier = "positive" if positive else "nonnegative"
        raise BoundaryError(f"{label} must be a {qualifier} integer")
    return value


def _load_json(path: Path, label: str) -> Any:
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
        )
    except BoundaryError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BoundaryError(f"cannot read {label}: {exc}") from exc


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise BoundaryError(f"cannot hash local release asset {path.name}: {exc}") from exc
    return "sha256:" + digest.hexdigest()


def _read_expected_names(path: Path) -> list[str]:
    try:
        names = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise BoundaryError(f"cannot read expected asset names: {exc}") from exc
    _require(bool(names), "expected asset list must not be empty")
    for name in names:
        _require(bool(name) and name == Path(name).name, f"unsafe expected asset name: {name!r}")
    _require(len(names) == len(set(names)), "expected asset names must be unique")
    return names


def _read_expected_digests(path: Path, names: list[str]) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise BoundaryError(f"cannot read expected asset digests: {exc}") from exc
    digests: dict[str, str] = {}
    for index, line in enumerate(lines):
        match = SHA256_LINE_RE.fullmatch(line)
        _require(match is not None, f"expected digest line {index + 1} is malformed")
        assert match is not None
        name = match.group(2)
        _require(name not in digests, f"expected digest repeats asset {name}")
        digests[name] = "sha256:" + match.group(1).lower()
    _require(set(digests) == set(names), "expected asset names and digests differ")
    return digests


def verify_release_asset_boundary(
    *,
    release_json: Path,
    assets_json: Path,
    ledger_json: Path,
    expected_assets_file: Path,
    expected_digests_file: Path,
    asset_directory: Path,
    release_id: int,
    release_tag: str,
    is_versioned: bool,
    expected_draft: bool,
) -> None:
    """Verify raw GitHub metadata against durable state and exact local bytes."""

    _require(release_id > 0, "release ID must be positive")
    _require(bool(release_tag), "release tag must not be empty")
    names = _read_expected_names(expected_assets_file)
    digests = _read_expected_digests(expected_digests_file, names)

    asset_root = asset_directory.resolve()
    local_sizes: dict[str, int] = {}
    for name in names:
        path = asset_root / name
        _require(not path.is_symlink(), f"local release asset {name} must not be a symlink")
        _require(path.is_file(), f"local release asset {name} is missing or not a file")
        _require(_sha256(path) == digests[name], f"local release asset {name} digest drifted")
        local_sizes[name] = path.stat().st_size

    release = _load_json(release_json, "release metadata")
    _require(isinstance(release, dict), "release metadata must be an object")
    _require(_exact_int(release.get("id"), "release.id", positive=True) == release_id,
             "release ID differs from the durable target")
    _require(release.get("tag_name") == release_tag, "release tag differs from the durable target")
    _require(release.get("draft") is expected_draft, "release draft visibility is not exact")
    _require(release.get("prerelease") is (not is_versioned), "release channel is not exact")
    _require(release.get("immutable") is False, "release is immutable at a recoverable publication boundary")

    pages = _load_json(assets_json, "release asset metadata")
    _require(isinstance(pages, list) and bool(pages), "release asset pages must be a nonempty array")
    assets: list[dict[str, Any]] = []
    for page_index, page in enumerate(pages):
        _require(isinstance(page, list), f"release asset page {page_index + 1} must be an array")
        _require(bool(page), f"release asset page {page_index + 1} must not be empty")
        if page_index < len(pages) - 1:
            _require(
                len(page) == 100,
                f"nonterminal release asset page {page_index + 1} must contain 100 records",
            )
        else:
            _require(
                len(page) <= 100,
                f"terminal release asset page {page_index + 1} exceeds the requested page size",
            )
        for asset_index, asset in enumerate(page):
            _require(isinstance(asset, dict),
                     f"release asset {page_index + 1}:{asset_index + 1} must be an object")
            assets.append(asset)
    _require(len(assets) == len(names), "release asset count differs from the expected set")

    actual_by_id: dict[int, dict[str, Any]] = {}
    actual_by_name: dict[str, dict[str, Any]] = {}
    for index, asset in enumerate(assets):
        label = f"release asset {index + 1}"
        asset_id = _exact_int(asset.get("id"), f"{label}.id", positive=True)
        name = asset.get("name")
        _require(isinstance(name, str) and bool(name), f"{label}.name must be nonempty")
        _require(asset_id not in actual_by_id, f"release assets repeat ID {asset_id}")
        _require(name not in actual_by_name, f"release assets repeat name {name}")
        _require(name in digests, f"release contains unexpected asset {name}")
        _require(asset.get("state") == "uploaded", f"release asset {name} is not uploaded")
        digest = asset.get("digest")
        _require(isinstance(digest, str) and SHA256_DIGEST_RE.fullmatch(digest) is not None,
                 f"release asset {name} has an invalid digest")
        _require(digest == digests[name], f"release asset {name} digest drifted")
        _require(_exact_int(asset.get("size"), f"release asset {name}.size") == local_sizes[name],
                 f"release asset {name} size drifted")
        _exact_int(asset.get("download_count"), f"release asset {name}.download_count")
        uploader = asset.get("uploader")
        _require(isinstance(uploader, dict), f"release asset {name} uploader is missing")
        _require(_exact_int(uploader.get("id"), f"release asset {name}.uploader.id", positive=True)
                 == EXPECTED_UPLOADER_ID, f"release asset {name} uploader ID is untrusted")
        _require(uploader.get("login") == EXPECTED_UPLOADER_LOGIN,
                 f"release asset {name} uploader login is untrusted")
        actual_by_id[asset_id] = asset
        actual_by_name[name] = asset
    _require(set(actual_by_name) == set(names), "release asset names are not exact")

    ledger = _load_json(ledger_json, "durable release ledger")
    _require(isinstance(ledger, dict), "durable release ledger must be an object")
    _require(type(ledger.get("schemaVersion")) is int and ledger["schemaVersion"] == 2,
             "durable release ledger schema must be integer version 2")
    pending = ledger.get("pending")
    _require(isinstance(pending, dict), "durable release ledger has no pending publication")
    _require(pending.get("phase") == "staged", "durable release ledger is not staged")
    _require(pending.get("targetReleaseId") == release_id, "ledger release ID is not exact")
    _require(pending.get("targetTag") == release_tag, "ledger release tag is not exact")
    _require(pending.get("isVersioned") is is_versioned, "ledger release channel is not exact")
    target_draft_at_prepare = pending.get("targetDraftAtPrepare")
    _require(
        not is_versioned
        or target_draft_at_prepare is None
        or target_draft_at_prepare is True,
        "ledger was prepared from an already-public release",
    )
    _require(pending.get("evidenceReplacement") is None, "ledger retains evidence replacement intent")
    _require(pending.get("draftCleanup", []) == [], "ledger retains draft cleanup intent")

    prepared_ids_value = pending.get("preparedAssetIds")
    _require(isinstance(prepared_ids_value, list), "ledger prepared asset IDs must be an array")
    prepared_ids = {
        _exact_int(asset_id, "ledger prepared asset ID", positive=True)
        for asset_id in prepared_ids_value
    }
    _require(len(prepared_ids) == len(prepared_ids_value), "ledger prepared asset IDs repeat")
    _require(prepared_ids == set(actual_by_id), "release asset IDs differ from the durable checkpoint")

    ledger_assets_value = ledger.get("assets")
    _require(isinstance(ledger_assets_value, list), "ledger assets must be an array")
    ledger_by_id: dict[int, dict[str, Any]] = {}
    for index, entry in enumerate(ledger_assets_value):
        _require(isinstance(entry, dict), f"ledger asset {index + 1} must be an object")
        asset_id = _exact_int(entry.get("id"), f"ledger asset {index + 1}.id", positive=True)
        _require(asset_id not in ledger_by_id, f"ledger assets repeat ID {asset_id}")
        ledger_by_id[asset_id] = entry

    for asset_id, asset in actual_by_id.items():
        name = asset["name"]
        entry = ledger_by_id.get(asset_id)
        _require(entry is not None, f"release asset {name} is absent from the durable ledger")
        assert entry is not None
        _require(entry.get("releaseId") == release_id, f"ledger release ID drifted for asset {name}")
        _require(entry.get("name") == name, f"ledger name drifted for asset {name}")
        _require(entry.get("digest") == digests[name], f"ledger digest drifted for asset {name}")
        _require(entry.get("state") == "uploaded", f"ledger state drifted for asset {name}")
        ledger_count = _exact_int(entry.get("downloadCount"), f"ledger asset {name}.downloadCount")
        live_count = _exact_int(asset.get("download_count"), f"release asset {name}.download_count")
        if expected_draft:
            _require(live_count == ledger_count, f"draft download count drifted for asset {name}")
        else:
            _require(live_count >= ledger_count, f"published download count regressed for asset {name}")


def _parse_boolean(value: str) -> bool:
    if value == "true":
        return True
    if value == "false":
        return False
    raise argparse.ArgumentTypeError("expected true or false")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-json", type=Path, required=True)
    parser.add_argument("--assets-json", type=Path, required=True)
    parser.add_argument("--ledger-json", type=Path, required=True)
    parser.add_argument("--expected-assets-file", type=Path, required=True)
    parser.add_argument("--expected-digests-file", type=Path, required=True)
    parser.add_argument("--asset-directory", type=Path, required=True)
    parser.add_argument("--release-id", type=int, required=True)
    parser.add_argument("--release-tag", required=True)
    parser.add_argument("--is-versioned", type=_parse_boolean, required=True)
    parser.add_argument("--expected-draft", type=_parse_boolean, required=True)
    args = parser.parse_args(argv)
    try:
        verify_release_asset_boundary(
            release_json=args.release_json,
            assets_json=args.assets_json,
            ledger_json=args.ledger_json,
            expected_assets_file=args.expected_assets_file,
            expected_digests_file=args.expected_digests_file,
            asset_directory=args.asset_directory,
            release_id=args.release_id,
            release_tag=args.release_tag,
            is_versioned=args.is_versioned,
            expected_draft=args.expected_draft,
        )
    except BoundaryError as exc:
        parser.error(str(exc))
    print(f"Verified exact release asset boundary for release {args.release_id}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
