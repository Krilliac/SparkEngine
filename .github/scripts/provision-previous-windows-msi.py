#!/usr/bin/env python3
"""Provision the immutable predecessor MSI used by the stable Windows gate.

This is deliberately a small GitHub REST client rather than a checkout helper:
the predecessor is selected from published, non-prerelease releases, its tag is
resolved to a commit, and both assets are downloaded by numeric asset id.  No
credentials are written to the receipt or passed through the output paths.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

SEMVER_RE = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
DIGEST_RE = re.compile(r"^sha256:([0-9a-f]{64})$")
MAX_RESPONSE_BYTES = 2 * 1024 * 1024
MAX_DOWNLOAD_BYTES = 1024 * 1024 * 1024
MAX_RELEASE_PAGES = 10
MAX_ASSETS = 256
DEFAULT_TIMEOUT = 30.0
DEFAULT_DEADLINE = 180.0
MANIFEST_KEYS = frozenset({"schemaVersion", "commitSHA", "profile", "configuration", "version", "msi", "sha256"})


class ProvisionError(RuntimeError):
    pass


def _reject_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _json_bytes(data: bytes, label: str):
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys)
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as exc:
        raise ProvisionError(f"{label} is not valid UTF-8 JSON: {exc}") from exc


def _is_link_like(path: Path) -> bool:
    for component in (path, *path.parents):
        try:
            info = component.lstat()
        except FileNotFoundError:
            continue
        if component.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
            return True
    return False


def _occupied_no_follow(path: Path) -> bool:
    """Treat even a dangling symlink as occupied before any write."""
    try:
        os.lstat(path)
    except FileNotFoundError:
        return False
    return True


def _semver(tag: str):
    match = SEMVER_RE.fullmatch(tag)
    if not match:
        return None
    return tuple(int(value) for value in match.groups())


class GitHubApi:
    def __init__(self, repository: str, token: str | None = None, *, base_url: str = "https://api.github.com", timeout: float = DEFAULT_TIMEOUT, deadline: float = DEFAULT_DEADLINE):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise ProvisionError("repository must be owner/name")
        self.repository = repository
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.timeout = timeout
        self.deadline = time.monotonic() + deadline

    def get(self, path: str, *, accept: str = "application/vnd.github+json", max_bytes: int = MAX_RESPONSE_BYTES) -> bytes:
        if not path.startswith("/") or ".." in path.split("/"):
            raise ProvisionError("invalid GitHub API path")
        if time.monotonic() >= self.deadline:
            raise ProvisionError("GitHub API deadline exceeded")
        headers = {"Accept": accept, "User-Agent": "SparkEngine-release-provisioner/1"}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        request = Request(self.base_url + path, headers=headers)
        try:
            with urlopen(request, timeout=min(self.timeout, max(0.1, self.deadline - time.monotonic()))) as response:
                content_length = response.headers.get("Content-Length")
                if content_length and int(content_length) > max_bytes:
                    raise ProvisionError("GitHub API response exceeds bounded size")
                chunks, total = [], 0
                while True:
                    chunk = response.read(min(64 * 1024, max_bytes - total + 1))
                    if not chunk:
                        break
                    total += len(chunk)
                    if total > max_bytes:
                        raise ProvisionError("GitHub API response exceeds bounded size")
                    chunks.append(chunk)
                return b"".join(chunks)
        except (HTTPError, URLError, TimeoutError, OSError) as exc:
            raise ProvisionError(f"GitHub API request failed for {path}: {exc}") from exc

    def json(self, path: str):
        return _json_bytes(self.get(path), path)

    def download_asset(self, asset_id: int) -> bytes:
        return self.get(f"/repos/{self.repository}/releases/assets/{asset_id}", accept="application/octet-stream", max_bytes=MAX_DOWNLOAD_BYTES)


def _release_candidates(api: GitHubApi, current_version):
    # The first fully gated stable release has a reviewed predecessor identity.
    # An arbitrary lower version (including a nightly relabeled as stable) must
    # not silently satisfy the v1.0.0 upgrade/rollback qualification policy.
    required_tag = "v0.9.0" if current_version == (1, 0, 0) else None
    found = []
    for page in range(1, MAX_RELEASE_PAGES + 1):
        payload = api.json(f"/repos/{api.repository}/releases?per_page=100&page={page}")
        if not isinstance(payload, list) or len(payload) > 100:
            raise ProvisionError("GitHub releases response must be a bounded array")
        for release in payload:
            if not isinstance(release, dict) or release.get("draft") is not False or release.get("prerelease") is not False:
                continue
            tag = release.get("tag_name")
            version = _semver(tag) if isinstance(tag, str) else None
            if (version is not None and version < current_version
                    and (required_tag is None or tag == required_tag)):
                found.append((version, tag, release))
        if len(payload) < 100:
            break
    if not found:
        if required_tag is not None:
            raise ProvisionError("no published non-prerelease v0.9.0 predecessor release exists")
        raise ProvisionError("no published non-prerelease predecessor release exists (first stable release is unsupported)")
    if required_tag is not None and len(found) != 1:
        raise ProvisionError("v0.9.0 predecessor release identity is ambiguous")
    found.sort(key=lambda item: item[0], reverse=True)
    return found[0]


def _tag_commit(api: GitHubApi, tag: str) -> str:
    ref = api.json(f"/repos/{api.repository}/git/ref/tags/{tag}")
    if not isinstance(ref, dict) or ref.get("ref") != f"refs/tags/{tag}":
        raise ProvisionError("tag ref response is not the requested immutable tag")
    obj = ref.get("object")
    if not isinstance(obj, dict) or obj.get("type") not in {"commit", "tag"} or not SHA_RE.fullmatch(str(obj.get("sha", ""))):
        raise ProvisionError("tag ref did not identify an exact object SHA")
    if obj["type"] == "commit":
        return obj["sha"]
    annotated = api.json(f"/repos/{api.repository}/git/tags/{obj['sha']}")
    target = annotated.get("object") if isinstance(annotated, dict) else None
    if not isinstance(target, dict) or target.get("type") != "commit" or not SHA_RE.fullmatch(str(target.get("sha", ""))):
        raise ProvisionError("annotated tag does not resolve directly to a commit")
    return target["sha"]


def _asset(release: dict, name: str):
    assets = release.get("assets")
    if not isinstance(assets, list) or len(assets) > MAX_ASSETS:
        raise ProvisionError("release assets response is not bounded")
    matches = [asset for asset in assets if isinstance(asset, dict) and asset.get("name") == name]
    if len(matches) != 1:
        raise ProvisionError(f"release must contain exactly one asset named {name}")
    asset = matches[0]
    asset_id, size, digest = asset.get("id"), asset.get("size"), asset.get("digest")
    if type(asset_id) is not int or asset_id <= 0 or type(size) is not int or size <= 0:
        raise ProvisionError(f"asset metadata is invalid for {name}")
    digest_match = DIGEST_RE.fullmatch(digest) if isinstance(digest, str) else None
    if not digest_match:
        raise ProvisionError(f"asset digest is missing or not sha256 for {name}")
    return {"id": asset_id, "name": name, "size": size, "digest": digest_match.group(1)}


def _release_identity(release: dict):
    release_id = release.get("id")
    if (type(release_id) is not int or release_id <= 0
            or not isinstance(release.get("tag_name"), str)
            or type(release.get("draft")) is not bool
            or type(release.get("prerelease")) is not bool):
        raise ProvisionError("release metadata is missing its immutable identity fields")
    if release.get("immutable") is not True:
        raise ProvisionError("predecessor release is not proven immutable")
    return (release_id, release["tag_name"], release["draft"], release["prerelease"], True)


def _manifest(path: Path, *, tag_sha: str, version: str, msi_name: str, msi_sha: str):
    data = _json_bytes(path.read_bytes(), "shipping-package-manifest.json")
    if not isinstance(data, dict) or set(data) != MANIFEST_KEYS or any(type(value) is not str for value in data.values()):
        raise ProvisionError("shipping-package-manifest.json has an invalid closed schema")
    expected = {"schemaVersion": "spark-shipping-package-v1", "commitSHA": tag_sha, "profile": "stable-v1", "configuration": "MinSizeRel", "version": version, "msi": msi_name, "sha256": msi_sha}
    if data != expected:
        raise ProvisionError("shipping-package-manifest.json does not match the predecessor tag and MSI")


def provision(repository: str, current_version: str, output_dir: Path, receipt: Path, *, api=None, token=None, base_url="https://api.github.com"):
    current = _semver("v" + current_version)
    if current is None:
        raise ProvisionError("current version must be X.Y.Z")
    if api is None:
        api = GitHubApi(repository, token, base_url=base_url)
    output_dir = Path(output_dir).absolute()
    receipt = Path(receipt).absolute()
    if (_occupied_no_follow(output_dir) or _occupied_no_follow(receipt)
            or _is_link_like(output_dir.parent) or _is_link_like(receipt.parent)):
        raise ProvisionError("provisioning output must be fresh and link-free")
    version_tuple, tag, release = _release_candidates(api, current)
    version = ".".join(str(part) for part in version_tuple)
    release_identity = _release_identity(release)
    commit_sha = _tag_commit(api, tag)
    msi_name = f"SparkEngine-{version}-Windows-AMD64-MinSizeRel-Runtime.msi"
    msi = _asset(release, msi_name)
    manifest_asset = _asset(release, "shipping-package-manifest.json")
    output_dir.mkdir(parents=True)
    packages = output_dir / "packages"
    packages.mkdir()
    downloaded = {}
    try:
        for metadata, destination in ((msi, packages / msi_name), (manifest_asset, output_dir / "shipping-package-manifest.json")):
            content = api.download_asset(metadata["id"])
            digest = hashlib.sha256(content).hexdigest()
            if len(content) != metadata["size"] or digest != metadata["digest"]:
                raise ProvisionError(f"downloaded asset failed size/digest verification: {metadata['name']}")
            if destination.exists() or _is_link_like(destination.parent):
                raise ProvisionError("download destination is not fresh or link-free")
            destination.write_bytes(content)
            if destination.stat().st_size != metadata["size"] or hashlib.sha256(destination.read_bytes()).hexdigest() != metadata["digest"]:
                raise ProvisionError(f"written asset failed size/digest verification: {metadata['name']}")
            downloaded[metadata["name"]] = (destination, digest)
        # Re-read the release after both downloads. A release owner must not be
        # able to swap either asset or its identity between metadata selection
        # and consumption by the transaction gate.
        refreshed = api.json(f"/repos/{api.repository}/releases/{release_identity[0]}")
        if _release_identity(refreshed) != release_identity:
            raise ProvisionError("predecessor release identity changed during provisioning")
        for name, expected_asset in ((msi_name, msi), ("shipping-package-manifest.json", manifest_asset)):
            if _asset(refreshed, name) != expected_asset:
                raise ProvisionError(f"predecessor release asset metadata changed during provisioning: {name}")
        if _tag_commit(api, tag) != commit_sha:
            raise ProvisionError("predecessor tag target changed during provisioning")
        manifest_path, _ = downloaded["shipping-package-manifest.json"]
        msi_path, msi_sha = downloaded[msi_name]
        _manifest(manifest_path, tag_sha=commit_sha, version=version, msi_name=msi_name, msi_sha=msi_sha)
        Path(output_dir / "previous-version.txt").write_text(version + "\n", encoding="ascii", newline="")
        result = {
            "schema": "spark-previous-windows-msi-v1",
            "repository": repository,
            "current_version": current_version,
            "previous_version": version,
            "tag": tag,
            "tag_commit_sha": commit_sha,
            "release_id": release_identity[0],
            "release_immutable": release_identity[4],
            "msi": {**msi, "downloaded_sha256": msi_sha, "path": "packages/" + msi_name},
            "manifest": {**manifest_asset, "path": "shipping-package-manifest.json"},
        }
        receipt.parent.mkdir(parents=True, exist_ok=True)
        receipt.write_text(json.dumps(result, sort_keys=True, indent=2) + "\n", encoding="utf-8", newline="")
        return result
    except Exception:
        # Never leave a success-looking partial output for the caller to consume.
        import shutil
        shutil.rmtree(output_dir, ignore_errors=True)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", required=True)
    parser.add_argument("--current-version", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--api-base-url", default="https://api.github.com")
    args = parser.parse_args(argv)
    try:
        token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
        result = provision(args.repository, args.current_version, args.output_dir, args.receipt, token=token, base_url=args.api_base_url)
        print(json.dumps({"schema": result["schema"], "previous_version": result["previous_version"], "tag_commit_sha": result["tag_commit_sha"], "msi_sha256": result["msi"]["downloaded_sha256"]}, sort_keys=True))
    except (ProvisionError, OSError, ValueError) as exc:
        print(f"provision-previous-windows-msi: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
