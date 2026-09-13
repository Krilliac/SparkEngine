#!/usr/bin/env python3
"""Asset-integrity validation for declared asset packages.

RDY-020 promises that every declared manifest resolves, that no reference is
case-mismatched, and that a tampered or traversing path fails before package
assembly. Existence checks cannot deliver that: they pass on any checkout. The
walk here resolves every reference against the git index (case-sensitive even on
a case-insensitive filesystem), recomputes the recorded SHA-256, and refuses a
package that ships a file no manifest declares.
"""

from __future__ import annotations

import hashlib
import ntpath
import re
import unicodedata
from pathlib import Path
from typing import Any

from common import REPO_ROOT, SiteDataError, load_json, tracked_paths


MANIFEST_NAME = "manifest.json"
# The package's own provenance records are not themselves declared assets.
UNDECLARED_EXEMPT = {MANIFEST_NAME, "README.md"}
PACKAGE_ROOTS = ("Templates", "GameModules")
MAX_MANIFEST_BYTES = 4 * 1024 * 1024
MAX_ASSET_BYTES = 256 * 1024 * 1024
MAX_PATH_BYTES = 1024
HASH_CHUNK_BYTES = 1024 * 1024
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
INVALID_WINDOWS_CHARS = frozenset('<>:"|?*')
RESERVED_WINDOWS_NAMES = frozenset(
    {"CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$"}
    | {f"COM{i}" for i in range(1, 10)}
    | {f"LPT{i}" for i in range(1, 10)}
)


def asset_package_roots() -> list[str]:
    """Repository-relative ``<root>/<package>/Assets`` directories that exist."""
    roots: list[str] = []
    for parent in PACKAGE_ROOTS:
        for candidate in sorted((REPO_ROOT / parent).glob("*/Assets")):
            if candidate.is_dir():
                roots.append(candidate.relative_to(REPO_ROOT).as_posix())
    return roots


def file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    read = 0
    with path.open("rb") as stream:
        while chunk := stream.read(HASH_CHUNK_BYTES):
            read += len(chunk)
            if read > MAX_ASSET_BYTES:
                raise SiteDataError(f"declared asset exceeds {MAX_ASSET_BYTES} bytes: {path}")
            digest.update(chunk)
    return digest.hexdigest()


def _reference_error(reference: Any) -> str | None:
    """Reject a reference before it is ever joined to a directory."""
    if not isinstance(reference, str) or not reference:
        return "asset path must be a non-empty string"
    try:
        encoded = reference.encode("utf-8")
    except UnicodeEncodeError as error:
        return f"asset path is not valid Unicode: {error}"
    if len(encoded) > MAX_PATH_BYTES:
        return f"asset path exceeds {MAX_PATH_BYTES} UTF-8 bytes"
    if reference != unicodedata.normalize("NFC", reference):
        return "asset path is not NFC-normalized"
    if "\\" in reference:
        return f"asset path uses backslashes: {reference!r}"
    if ntpath.isabs(reference) or ntpath.splitdrive(reference)[0] or reference.startswith("/"):
        return f"asset path is absolute: {reference!r}"
    parts = reference.split("/")
    if any(part in ("", ".", "..") for part in parts):
        return f"asset path traverses outside its package: {reference!r}"
    for part in parts:
        if part != part.strip() or part.endswith((".", " ")):
            return f"asset path has non-canonical whitespace or trailing dot: {reference!r}"
        for char in part:
            codepoint = ord(char)
            if codepoint < 0x20 or codepoint == 0x7F:
                return f"asset path contains control character U+{codepoint:04X}: {reference!r}"
            if char in INVALID_WINDOWS_CHARS:
                return f"asset path contains Windows-invalid character {char!r}: {reference!r}"
        device_stem = part.split(".", 1)[0].rstrip(" .").upper()
        if device_stem in RESERVED_WINDOWS_NAMES:
            return f"asset path contains reserved Windows device name: {reference!r}"
    return None


def validate_package(directory: str, tracked: frozenset[str]) -> list[tuple[str, str]]:
    """Validate one ``<package>/Assets`` directory. Returns (location, message)."""
    findings: list[tuple[str, str]] = []
    manifest_relative = f"{directory}/{MANIFEST_NAME}"
    manifest_path = REPO_ROOT / manifest_relative
    if not manifest_path.is_file():
        return [(directory, "asset package has no manifest.json recording provenance and SHA-256")]
    try:
        manifest = load_json(manifest_path, maximum=MAX_MANIFEST_BYTES)
    except SiteDataError as error:
        return [(manifest_relative, f"manifest is unreadable: {error}")]
    if not isinstance(manifest, dict):
        return [(manifest_relative, "manifest must be a JSON object")]
    if manifest.get("manifestVersion") != 1:
        findings.append((manifest_relative, "manifestVersion must be 1"))
    if not isinstance(manifest.get("license"), str) or not manifest.get("license"):
        findings.append((manifest_relative, "manifest must record a license"))
    entries = manifest.get("assets")
    if not isinstance(entries, list) or not entries:
        findings.append((manifest_relative, "manifest declares no assets"))
        entries = []

    declared: set[str] = set()
    for index, entry in enumerate(entries):
        location = f"{manifest_relative}.assets[{index}]"
        if not isinstance(entry, dict):
            findings.append((location, "asset entry must be an object"))
            continue
        reference = entry.get("path")
        problem = _reference_error(reference)
        if problem:
            findings.append((location, problem))
            continue
        resolved = f"{directory}/{reference}"
        if resolved in declared:
            findings.append((location, f"asset is declared twice: {reference}"))
            continue
        declared.add(resolved)
        if not isinstance(entry.get("origin"), str) or not entry["origin"]:
            findings.append((location, f"asset records no provenance: {reference}"))
        recorded = entry.get("sha256")
        if not isinstance(recorded, str) or not SHA256_PATTERN.fullmatch(recorded):
            findings.append((location, f"asset records no lowercase SHA-256: {reference}"))
            recorded = None
        # Membership in the git index is case-sensitive; the Windows filesystem
        # is not, so a reference that only differs in case must fail here.
        if resolved not in tracked:
            findings.append((location, f"asset reference resolves to no tracked file: {reference}"))
            continue
        path = REPO_ROOT / resolved
        if path.is_symlink() or not path.is_file():
            findings.append((location, f"declared asset is not a regular file: {reference}"))
            continue
        if recorded is not None:
            digest = file_digest(path)
            if digest != recorded:
                findings.append(
                    (location, f"asset content differs from its manifest SHA-256: {reference}")
                )

    prefix = f"{directory}/"
    shipped = {
        value
        for value in tracked
        if value.startswith(prefix) and Path(value).name not in UNDECLARED_EXEMPT
    }
    for extra in sorted(shipped - declared):
        findings.append((extra, "file ships in an asset package but no manifest declares it"))
    return findings


def validate_assets() -> list[tuple[str, str]]:
    """Validate every declared asset package. An empty result is a clean walk."""
    tracked = tracked_paths()
    roots = asset_package_roots()
    if not roots:
        return [("Assets", "no asset package declares a manifest")]
    findings: list[tuple[str, str]] = []
    for directory in roots:
        findings.extend(validate_package(directory, tracked))
    return findings
