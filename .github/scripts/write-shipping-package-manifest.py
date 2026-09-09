#!/usr/bin/env python3
"""Write a hash-bound identity manifest for one Windows Shipping MSI."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re

import package_evidence_io


SCHEMA_VERSION = "spark-shipping-package-v1"
PROFILE = "stable-v1"
CONFIGURATION = "MinSizeRel"
_VERSION_RE = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
_SHA1_RE = re.compile(r"[0-9a-f]{40}")


def _expected_msi_name(version: str) -> str:
    return f"SparkEngine-{version}-Windows-AMD64-{CONFIGURATION}-Runtime.msi"


def _digest(path: Path) -> str:
    state = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            state.update(chunk)
    return state.hexdigest()


def _select_msi(packages: Path, version: str) -> Path:
    if not packages.is_dir() or packages.is_symlink():
        raise ValueError("shipping package directory must be a real directory")
    expected = _expected_msi_name(version)
    candidates = sorted(path for path in packages.iterdir() if path.suffix.lower() == ".msi")
    if len(candidates) != 1 or candidates[0].name != expected:
        raise ValueError(f"expected exactly one Windows Shipping MSI named {expected}")
    msi = candidates[0]
    if not msi.is_file() or msi.is_symlink():
        raise ValueError("shipping MSI must be a regular file without symlink traversal")
    return msi


def _atomic_write_json(path: Path, document: dict[str, str]) -> None:
    payload = (json.dumps(document, indent=2) + "\n").encode("utf-8")
    package_evidence_io.publish_bytes_no_replace(path, payload)


def write_manifest(packages: Path, version: str, commit_sha: str, out: Path) -> dict[str, str]:
    """Publish one closed identity document for the exact CPack MSI bytes."""
    if not _VERSION_RE.fullmatch(version):
        raise ValueError("shipping package version must be a semantic X.Y.Z value")
    if not _SHA1_RE.fullmatch(commit_sha):
        raise ValueError("shipping package commit SHA must be 40 lower-case hexadecimal characters")
    msi = _select_msi(packages, version)
    document = {
        "schemaVersion": SCHEMA_VERSION,
        "commitSHA": commit_sha,
        "profile": PROFILE,
        "configuration": CONFIGURATION,
        "version": version,
        "msi": msi.name,
        "sha256": _digest(msi),
    }
    _atomic_write_json(out, document)
    return document


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packages", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    if os.name != "nt":
        parser.error("Windows is required for Shipping package manifest publication")
    try:
        write_manifest(args.packages, args.version, args.commit_sha, args.out)
    except (OSError, ValueError, package_evidence_io.PackageEvidenceIOError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
