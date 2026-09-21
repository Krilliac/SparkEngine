#!/usr/bin/env python3
"""Fail-closed consumer verification for a stable release bundle.

This verifier consumes release evidence produced by an external signing and
attestation boundary. It deliberately does not create signatures or infer
trust from their names: a stable bundle is rejected until every expected
artifact has a non-empty detached signature and the signature manifest binds
that signature to the exact local bytes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from typing import Any


SHA256 = re.compile(r"[0-9a-f]{64}")
COMMIT = re.compile(r"[0-9a-f]{40}")
FINGERPRINT = re.compile(r"[0-9a-f]{64}")
MAX_JSON_BYTES = 2 * 1024 * 1024
MAX_TEXT_BYTES = 512 * 1024
MAX_SIGNATURE_BYTES = 4 * 1024 * 1024


class BundleError(ValueError):
    """The release bundle is incomplete, stale, or not trusted."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise BundleError(message)


def _no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise BundleError(f"JSON object repeats key {key!r}")
        result[key] = value
    return result


def _load_json(path: Path, label: str) -> Any:
    try:
        _require(path.is_file() and not path.is_symlink(), f"{label} is missing or not a regular file")
        _require(path.stat().st_size <= MAX_JSON_BYTES, f"{label} exceeds the bounded size")
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_no_duplicates)
    except BundleError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BundleError(f"cannot read {label}: {exc}") from exc


def _digest(path: Path) -> str:
    value = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                value.update(chunk)
    except OSError as exc:
        raise BundleError(f"cannot hash {path.name}: {exc}") from exc
    return value.hexdigest()


def _safe_name(value: Any, label: str) -> str:
    _require(isinstance(value, str) and bool(value), f"{label} must be a non-empty string")
    _require(value == Path(value).name and value not in {".", ".."}, f"{label} is not a safe basename")
    _require("\x00" not in value and "\\" not in value and "/" not in value and ":" not in value,
             f"{label} contains an ambiguous separator")
    _require(not value.endswith((".", " ")), f"{label} has a trailing dot or space")
    stem = value.split(".", 1)[0].casefold()
    _require(stem not in {"con", "prn", "aux", "nul"}
             and not (len(stem) == 4 and stem[:3] in {"com", "lpt"} and stem[3].isdigit()),
             f"{label} uses a reserved Windows device name")
    return value


def _read_expected(path: Path) -> list[str]:
    try:
        _require(path.stat().st_size <= MAX_TEXT_BYTES, "expected asset list exceeds the bounded size")
        values = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise BundleError(f"cannot read expected asset list: {exc}") from exc
    _require(bool(values), "expected stable asset set must not be empty")
    names = [_safe_name(value, "expected asset name") for value in values]
    folded = [name.casefold() for name in names]
    _require(len(names) == len(set(folded)), "expected stable asset set has a case-fold collision")
    return names


def _read_sums(path: Path, expected: set[str], root: Path, provenance_name: str) -> dict[str, str]:
    try:
        _require(path.is_file() and not path.is_symlink(), "SHA256SUMS is missing or not a regular file")
        _require(path.stat().st_size <= MAX_TEXT_BYTES, "SHA256SUMS exceeds the bounded size")
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise BundleError(f"cannot read SHA256SUMS: {exc}") from exc
    _require(bool(lines), "SHA256SUMS must not be empty")
    sums: dict[str, str] = {}
    for index, line in enumerate(lines, 1):
        parts = line.split("  ", 1)
        _require(len(parts) == 2 and SHA256.fullmatch(parts[0]) is not None,
                 f"SHA256SUMS line {index} is malformed")
        name = _safe_name(parts[1], f"SHA256SUMS line {index} asset")
        _require(name not in sums, f"SHA256SUMS repeats {name}")
        sums[name] = parts[0]
    # The exact-CI manifest has a separately verified digest because stable
    # draft recovery may replace that evidence atomically. All other expected
    # assets must make the normal checksum round trip.
    _require(set(sums) == expected - {provenance_name}, "SHA256SUMS does not cover the exact expected asset set")
    for name, digest in sums.items():
        asset = root / name
        _require(asset.is_file() and not asset.is_symlink(), f"checksum asset {name} is missing")
        _require(_digest(asset) == digest, f"SHA256SUMS digest mismatch for {name}")
    return sums


def _verify_spdx(path: Path, root: Path, expected: set[str]) -> None:
    data = _load_json(path, "SPDX SBOM")
    _require(isinstance(data, dict), "SPDX SBOM must be an object")
    _require(isinstance(data.get("spdxVersion"), str) and data["spdxVersion"].startswith("SPDX-"),
             "SPDX SBOM has no valid spdxVersion")
    _require(isinstance(data.get("SPDXID"), str) and data["SPDXID"].startswith("SPDXRef-"),
             "SPDX SBOM has no valid SPDXID")
    _require(isinstance(data.get("documentNamespace"), str) and bool(data["documentNamespace"]),
             "SPDX SBOM has no documentNamespace")
    files = data.get("files")
    packages = data.get("packages")
    _require(isinstance(files, list) and bool(files), "SPDX SBOM files inventory is empty")
    _require(isinstance(packages, list) and bool(packages), "SPDX SBOM packages inventory is empty")
    package_assets = {name for name in expected if name.lower().endswith((".zip", ".tar.gz", ".exe", ".msi"))}
    _require(bool(package_assets), "stable bundle has no package payload for SPDX coverage")
    package_by_fold = {name.casefold(): name for name in package_assets}
    covered_assets: set[str] = set()
    for index, entry in enumerate(files, 1):
        _require(isinstance(entry, dict) and isinstance(entry.get("fileName"), str)
                 and bool(entry["fileName"]), f"SPDX file entry {index} has no fileName")
        file_name = _safe_name(Path(entry["fileName"]).name, f"SPDX file entry {index}.fileName")
        canonical_name = package_by_fold.get(file_name.casefold())
        _require(canonical_name is not None,
                 f"SPDX file entry {index} does not map to a stable package payload")
        assert canonical_name is not None
        covered_assets.add(canonical_name)
        checksums = entry.get("checksums")
        _require(isinstance(checksums, list) and bool(checksums),
                 f"SPDX file entry {index} has no checksums")
        sha256_checksums = [item for item in checksums
                            if isinstance(item, dict) and item.get("algorithm") == "SHA256"
                            and isinstance(item.get("checksumValue"), str)
                            and SHA256.fullmatch(item["checksumValue"].lower())]
        _require(bool(sha256_checksums), f"SPDX file entry {index} has no valid SHA256 checksum")
        actual = _digest(root / canonical_name)
        _require(any(item["checksumValue"].lower() == actual for item in sha256_checksums),
                 f"SPDX checksum does not match bundle payload {canonical_name}")
    _require(covered_assets == package_assets,
             "SPDX coverage does not include every stable package payload")


def _verify_provenance(path: Path, source_commit: str, expected: set[str], root: Path) -> str:
    _require(COMMIT.fullmatch(source_commit) is not None, "source commit must be a lowercase 40-character SHA")
    data = _load_json(path, "exact-CI provenance manifest")
    _require(isinstance(data, dict), "exact-CI provenance manifest must be an object")
    _require(data.get("schemaVersion") == 2, "exact-CI provenance manifest schema must be version 2")
    _require(data.get("sourceCommit") == source_commit, "exact-CI provenance source commit drifted")
    name = path.name
    _require(name in expected, "exact-CI provenance manifest is outside the expected stable asset set")
    return _digest(path)


def _verify_signatures(path: Path, root: Path, signature_root: Path, expected: set[str], public_key: Path,
                       fingerprint: str, openssl: str) -> None:
    _require(public_key.is_file() and not public_key.is_symlink(),
             "trusted public key is not provisioned; stable signing precondition is unsatisfied")
    _require(FINGERPRINT.fullmatch(fingerprint) is not None,
             "trusted public-key fingerprint must be a lowercase SHA-256 digest")
    try:
        der = subprocess.run(
            [openssl, "pkey", "-pubin", "-in", str(public_key), "-outform", "DER"],
            check=True, capture_output=True, timeout=10,
        ).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        raise BundleError(f"cannot inspect trusted public key with {openssl!r}: {exc}") from exc
    _require(hashlib.sha256(der).hexdigest() == fingerprint,
             "trusted public-key fingerprint does not match provisioned key")
    data = _load_json(path, "detached signature manifest")
    _require(isinstance(data, dict), "detached signature manifest must be an object")
    _require(data.get("schemaVersion") == 1, "detached signature manifest schema must be version 1")
    _require(data.get("algorithm") == "detached-sha256", "detached signature algorithm is not approved")
    entries = data.get("artifacts")
    _require(isinstance(entries, list) and bool(entries), "detached signature manifest artifacts are missing")
    seen: set[str] = set()
    folded_assets = {name.casefold() for name in expected}
    referenced_signatures: set[str] = set()
    folded_signatures: set[str] = set()
    for index, entry in enumerate(entries, 1):
        _require(isinstance(entry, dict), f"signature entry {index} must be an object")
        name = _safe_name(entry.get("name"), f"signature entry {index}.name")
        _require(name in expected, f"signature manifest names unexpected artifact {name}")
        _require(name not in seen, f"signature manifest repeats {name}")
        seen.add(name)
        signature = _safe_name(entry.get("signature"), f"signature entry {index}.signature")
        _require(signature.lower().endswith(".sig"), f"signature entry {index} must use the .sig suffix")
        _require(signature.casefold() not in folded_signatures,
                 f"signature manifest has a case-fold collision for {signature}")
        folded_signatures.add(signature.casefold())
        _require(signature not in referenced_signatures, f"signature manifest repeats detached signature {signature}")
        referenced_signatures.add(signature)
        _require(signature.casefold() not in folded_assets
                 and signature.casefold() not in {path.name.casefold(), public_key.name.casefold()},
                 f"signature entry {index} aliases a payload or control file")
        _require(SHA256.fullmatch(entry.get("artifactSha256", "")) is not None,
                 f"signature entry {index} has an invalid artifact digest")
        actual = _digest(root / name)
        _require(entry["artifactSha256"] == actual, f"signature binding drifted for {name}")
        detached = signature_root / signature
        _require(detached.is_file() and not detached.is_symlink() and 0 < detached.stat().st_size <= MAX_SIGNATURE_BYTES,
                 f"detached signature is missing or empty for {name}")
        _require(isinstance(entry.get("signerFingerprint"), str) and bool(entry["signerFingerprint"]),
                 f"signature entry {index} has no signer fingerprint")
        _require(entry["signerFingerprint"] == fingerprint,
                 f"signature entry {index} signer fingerprint is not trusted")
        try:
            verified = subprocess.run(
                [openssl, "dgst", "-sha256", "-verify", str(public_key),
                 "-signature", str(detached), str(root / name)],
                check=False, capture_output=True, text=True, timeout=30,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise BundleError(f"cannot verify detached signature for {name}: {exc}") from exc
        _require(verified.returncode == 0 and verified.stdout.strip() == "Verified OK",
                 f"detached signature cryptographic verification failed for {name}")
    _require(seen == expected, "detached signature manifest does not cover every expected artifact")
    for child in signature_root.iterdir():
        if child.name.casefold().endswith(".sig"):
            _require(child.is_file() and not child.is_symlink(),
                     f"detached signature {child.name} is not a regular file")
            _require(child.name.casefold() in folded_signatures,
                     f"unreferenced detached signature {child.name}")


def verify_release_bundle(*, bundle_directory: Path, expected_assets_file: Path, sha256sums: Path,
                          sbom: Path, provenance_manifest: Path, signature_manifest: Path,
                          source_commit: str, trusted_public_key: Path,
                          trusted_key_fingerprint: str, signature_directory: Path | None = None,
                          openssl: str = "openssl") -> None:
    root = bundle_directory.resolve()
    _require(root.is_dir(), "bundle directory is missing")
    expected_names = _read_expected(expected_assets_file)
    _require("SHA256SUMS" in expected_names,
             "generated release inventory must include SHA256SUMS as a control file")
    expected_names = [name for name in expected_names if name != "SHA256SUMS"]
    expected = set(expected_names)
    known_control_files = {sha256sums.name, signature_manifest.name}
    promotable_candidates = {
        child.name
        for child in root.iterdir()
        if child.is_file() and not child.is_symlink()
        and (child.name.endswith((".zip", ".tar.gz", ".exe", ".msi", ".spdx.json"))
             or child.name in {"shipping-package-manifest.json", "SparkEngine-Exact-CI-Evidence.json"})
    }
    _require(
        promotable_candidates <= expected | known_control_files,
        "bundle contains an extra promotable artifact outside the expected stable set",
    )
    signature_root = (signature_directory or signature_manifest.parent).resolve()
    _require(signature_root.is_dir(), "signature directory is missing")
    _require(trusted_public_key.resolve().parent == signature_root,
             "trusted public key must be provisioned inside the signature directory")
    _require(provenance_manifest.resolve().parent == root, "provenance manifest must be in the bundle directory")
    _require(sbom.resolve().parent == root, "SPDX SBOM must be in the bundle directory")
    _require(signature_manifest.resolve().parent == signature_root,
             "signature manifest must be in the signature directory")
    _require(provenance_manifest.name in expected, "provenance manifest is not an expected asset")
    _require(sbom.name in expected, "SPDX SBOM is not an expected asset")
    for name in expected_names:
        asset = root / name
        _require(asset.is_file() and not asset.is_symlink(), f"expected stable asset {name} is missing")
    provenance_digest = _verify_provenance(provenance_manifest, source_commit, expected, root)
    sums = _read_sums(sha256sums, expected, root, provenance_manifest.name)
    _verify_spdx(sbom, root, expected)
    _require(sbom.name in sums, "SPDX SBOM is not bound by SHA256SUMS")
    # Provenance is outside SHA256SUMS by design, but must still be bound by a
    # signature entry and by the exact local bytes consumed here.
    _require(provenance_digest == _digest(provenance_manifest), "provenance digest changed during verification")
    _verify_signatures(signature_manifest, root, signature_root, expected, trusted_public_key,
                       trusted_key_fingerprint, openssl)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle-directory", type=Path, required=True)
    parser.add_argument("--expected-assets-file", type=Path, required=True)
    parser.add_argument("--sha256sums", type=Path, required=True)
    parser.add_argument("--sbom", type=Path, required=True)
    parser.add_argument("--provenance-manifest", type=Path, required=True)
    parser.add_argument("--signature-manifest", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--trusted-public-key", type=Path, required=True)
    parser.add_argument("--trusted-key-fingerprint", required=True)
    parser.add_argument("--signature-directory", type=Path)
    parser.add_argument("--openssl", default="openssl")
    args = parser.parse_args(argv)
    try:
        verify_release_bundle(**vars(args))
    except BundleError as exc:
        parser.error(str(exc))
    print("release bundle verified: expected assets, SHA256SUMS, SPDX SBOM, exact-CI provenance, and detached signatures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
