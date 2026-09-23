#!/usr/bin/env python3
"""Independently download a stable release and retain exact-source verification.

This consumer has no publication authority and never edits the readiness ledger.
Its receipt means publication-verified, not that the release is finally ready.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from receipt_publication import publish_receipt_no_replace
from verify_release_bundle import verify_release_bundle


SIGNATURE_CONTROL_ASSET = "SparkEngine-release-signature-bundle.tar.gz"

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "site-data"))
from exact_evidence import verify_manifest, values_from_gate_output


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate GitHub metadata field: {key}")
        result[key] = value
    return result


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def expected_assets(tag):
    require(re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag), "stable tag must be vMAJOR.MINOR.PATCH")
    prefix = f"SparkEngine-{tag[1:]}-Windows-AMD64-MinSizeRel"
    return {prefix + suffix for suffix in (".zip", "-Runtime.exe", "-Runtime.msi")} | {
        "shipping-package-manifest.json", "SparkEngine-SBOM.spdx.json",
        "SparkEngine-Exact-CI-Evidence.json", "SHA256SUMS",
    }


def release_identity(record, tag):
    require(isinstance(record, dict), "release record must be an object")
    require(type(record.get("id")) is int and record["id"] > 0, "release has no immutable id")
    require(record.get("tag_name") == tag and record.get("draft") is False
            and record.get("prerelease") is False, "release is not the requested published stable version")
    require(record.get("immutable") is True, "published stable release must be immutable")
    require(isinstance(record.get("published_at"), str) and bool(record["published_at"]), "release has no publication time")
    return {key: record[key] for key in ("id", "tag_name", "draft", "prerelease", "immutable", "published_at")}


def asset_identity(assets, tag, *, signature_control_asset=None):
    expected = expected_assets(tag)
    allowed = expected | ({signature_control_asset} if signature_control_asset else set())
    require(isinstance(assets, list) and len(assets) == len(allowed), "published asset count differs from stable contract")
    result = {}
    ids = set()
    for asset in assets:
        require(isinstance(asset, dict), "asset record must be an object")
        name, asset_id, size = asset.get("name"), asset.get("id"), asset.get("size")
        require(isinstance(name, str) and name in allowed and name not in result, "unexpected or duplicate release asset")
        require(type(asset_id) is int and asset_id > 0 and asset_id not in ids, "invalid or duplicate asset id")
        require(type(size) is int and 0 < size <= 8 * 1024 ** 3, "asset size is outside the bounded download contract")
        require(asset.get("state") == "uploaded", "release asset is not uploaded")
        checksum = asset.get("digest")
        require(isinstance(checksum, str) and re.fullmatch(r"sha256:[0-9a-f]{64}", checksum), "release asset lacks an authoritative SHA-256 digest")
        if signature_control_asset is not None and name == signature_control_asset:
            uploader = asset.get("uploader")
            require(isinstance(uploader, dict) and uploader.get("id") == 41898282
                    and uploader.get("login") == "github-actions[bot]",
                    "signature control asset uploader is untrusted")
        result[name] = {"id": asset_id, "size": size, "sha256": checksum[7:]}
        ids.add(asset_id)
    return result


class GitHub:
    def __init__(self, repository):
        self.repository = repository

    def json(self, suffix):
        response = subprocess.run(["gh", "api", f"repos/{self.repository}/{suffix}"],
                                  capture_output=True, timeout=60, check=True)
        require(len(response.stdout) <= 1024 * 1024, "GitHub metadata exceeds the size limit")
        return json.loads(response.stdout, object_pairs_hook=unique_object)

    def download(self, asset_id, destination):
        with destination.open("xb") as output:
            subprocess.run(["gh", "api", f"repos/{self.repository}/releases/assets/{asset_id}",
                            "-H", "Accept: application/octet-stream"], stdout=output,
                           stderr=subprocess.PIPE, timeout=600, check=True)

    def verify_attestation(self, tag):
        subprocess.run(["gh", "release", "verify", tag, "--repo", self.repository],
                       capture_output=True, timeout=300, check=True)


def tag_commit(api, tag):
    record = api.json(f"git/ref/tags/{tag}")
    require(isinstance(record, dict) and record.get("ref") == f"refs/tags/{tag}", "tag identity drifted")
    obj = record.get("object", {})
    for _ in range(4):
        require(isinstance(obj, dict) and isinstance(obj.get("sha"), str)
                and re.fullmatch(r"[0-9a-f]{40}", obj["sha"]), "tag object identity is invalid")
        if obj.get("type") == "commit":
            return obj["sha"]
        require(obj.get("type") == "tag", "tag does not resolve to a commit")
        obj = api.json(f"git/tags/{obj['sha']}").get("object", {})
    raise ValueError("tag nesting exceeds the bounded identity check")


def verify(*, repository, tag, source_commit, directory, signature_directory, fingerprint,
           gate_output, receipt, run_id, run_attempt, api=None,
           bundle_verifier=verify_release_bundle, provenance_verifier=verify_manifest,
           signature_control_asset=None):
    require(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository), "invalid repository identity")
    require(re.fullmatch(r"[0-9a-f]{40}", source_commit), "invalid source commit")
    require(type(run_id) is int and run_id > 0 and type(run_attempt) is int and run_attempt > 0, "invalid verifier run identity")
    names = expected_assets(tag)
    api = api or GitHub(repository)
    identity = release_identity(api.json(f"releases/tags/{tag}"), tag)
    require(tag_commit(api, tag) == source_commit, "published tag differs from candidate SHA")
    endpoint = f"releases/{identity['id']}/assets?per_page=100"
    assets = asset_identity(api.json(endpoint), tag, signature_control_asset=signature_control_asset)
    directory.mkdir(mode=0o700, parents=False, exist_ok=False)
    for name in sorted(names):
        entry = assets[name]
        destination = directory / name
        api.download(entry["id"], destination)
        require(destination.is_file() and not destination.is_symlink(), "download is not a regular file")
        require(destination.stat().st_size == entry["size"] and digest(destination) == entry["sha256"],
                f"downloaded asset differs from GitHub identity: {name}")
    if signature_control_asset is not None:
        control = assets[signature_control_asset]
        control_archive = directory.parent / signature_control_asset
        api.download(control["id"], control_archive)
        require(control_archive.is_file() and not control_archive.is_symlink()
                and control_archive.stat().st_size == control["size"]
                and digest(control_archive) == control["sha256"],
                "downloaded signature control asset differs from GitHub identity")
        from extract_release_signature_bundle import extract_archive
        extract_archive(control_archive, signature_directory)
    expected = directory / "expected-assets.txt"
    expected.write_text("\n".join(sorted(names)) + "\n", encoding="utf-8")
    bundle_verifier(
        bundle_directory=directory, expected_assets_file=expected,
        sha256sums=directory / "SHA256SUMS", sbom=directory / "SparkEngine-SBOM.spdx.json",
        provenance_manifest=directory / "SparkEngine-Exact-CI-Evidence.json",
        signature_manifest=signature_directory / "release-signatures.json",
        source_commit=source_commit, trusted_public_key=signature_directory / "spark-release-public-key.pem",
        trusted_key_fingerprint=fingerprint, signature_directory=signature_directory,
    )
    provenance_verifier(directory / "SparkEngine-Exact-CI-Evidence.json", values_from_gate_output(
        gate_output, repository=repository, source_commit=source_commit))
    api.verify_attestation(tag)
    require(release_identity(api.json(f"releases/{identity['id']}"), tag) == identity,
            "published release changed during consumer verification")
    require(asset_identity(api.json(endpoint), tag, signature_control_asset=signature_control_asset) == assets,
            "published asset set changed during consumer verification")
    require(tag_commit(api, tag) == source_commit, "tag changed during consumer verification")
    result = {
        "schemaVersion": 1, "state": "publication-verified", "repository": repository,
        "sourceCommit": source_commit, "release": identity, "assets": assets,
        "trustedSigningKeyFingerprint": fingerprint,
        "verifier": {"workflow": ".github/workflows/release.yml", "job": "verify-stable-publication",
                     "runId": run_id, "runAttempt": run_attempt, "sourceCommit": source_commit},
        "checks": ["fresh-download", "asset-identity", "detached-signatures", "checksums", "sbom",
                   "exact-ci", "github-release-attestation", "immutable-tag"],
        "limitations": ["Does not promote readiness or prove live-site consumption, owner sign-off, or Windows certification."],
    }
    publish_receipt_no_replace(receipt, (json.dumps(result, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("repository", "tag", "source-commit", "fingerprint"):
        parser.add_argument("--" + name, required=True)
    for name in ("directory", "signature-directory", "gate-output", "receipt"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--run-id", required=True, type=int)
    parser.add_argument("--run-attempt", required=True, type=int)
    parser.add_argument("--signature-control-asset", default=SIGNATURE_CONTROL_ASSET)
    args = parser.parse_args()
    try:
        verify(**vars(args))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"published stable verification failed: {error}", file=sys.stderr)
        return 1
    print("Published stable assets independently verified; final readiness remains a separate evidence decision")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
