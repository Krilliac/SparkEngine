#!/usr/bin/env python3
"""Verify a PLT-200 evidence bundle against the real bytes on disk.

A declared artifact is a claim, not a fact.  Everything here exists to turn
the claim into a measurement:

  - every declared path is resolved beneath an explicit, confined per-row root
  - every component of that path is lstat-ed, so a symlink, junction, or any
    other reparse point anywhere in the chain refuses the bundle
  - the file is opened without following links and re-identified through the
    handle, so a swap between check and open cannot substitute a file
  - size and SHA-256 come from that handle; the declared values must match
  - counts and byte totals are bounded before anything is read
  - every file present in the bundle must be declared, so nothing can be
    smuggled in beside the evidence

The bundle digest recorded in `collector.attestation.bundleSha256` is
recomputed from the *measured* digests, so a record cannot attest to content
it did not actually produce.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any

import dependency_authority as da
import safe_fs

# ── Resource limits ────────────────────────────────────────────────────────
MAX_ARTIFACT_BYTES = 64 * 1024 * 1024
MAX_BUNDLE_BYTES = 512 * 1024 * 1024
MAX_BUNDLE_FILES = 2000
MAX_BUNDLE_DEPTH = 8
MAX_DECLARED_ITEMS = 1200

# The canonical byte encoding of one entry in the attested bundle digest.
# Documented here because the collector must reproduce it exactly.
_DIGEST_SEPARATOR = b"\x00"
_DIGEST_TERMINATOR = b"\n"


def bundle_digest(items: list[tuple[str, str]]) -> str:
    """SHA-256 over the sorted (declared path, measured sha256) pairs."""
    digest = hashlib.sha256()
    for path, sha in sorted(items):
        digest.update(path.encode("utf-8"))
        digest.update(_DIGEST_SEPARATOR)
        digest.update(sha.encode("ascii"))
        digest.update(_DIGEST_TERMINATOR)
    return digest.hexdigest()


def _declared_items(evidence: dict[str, Any]) -> list[tuple[str, str, int, str]]:
    """Collect (context, path, sizeBytes, sha256) for every declared file."""
    items: list[tuple[str, str, int, str]] = []
    probes = evidence.get("probes")
    if isinstance(probes, dict):
        for name in sorted(probes):
            probe = probes[name]
            if not isinstance(probe, dict):
                continue
            artifacts = probe.get("artifacts")
            if not isinstance(artifacts, list):
                continue
            for index, artifact in enumerate(artifacts):
                if not isinstance(artifact, dict):
                    continue
                items.append(
                    (
                        f"probes.{name}.artifacts[{index}]",
                        artifact.get("path"),
                        artifact.get("sizeBytes"),
                        artifact.get("sha256"),
                    )
                )
    deps = evidence.get("dependencyClosure")
    if isinstance(deps, list):
        for index, dep in enumerate(deps):
            if not isinstance(dep, dict):
                continue
            items.append(
                (
                    f"dependencyClosure[{index}]",
                    dep.get("path"),
                    dep.get("sizeBytes"),
                    dep.get("sha256"),
                )
            )
    return items


def verify_bundle(
    evidence: dict[str, Any],
    *,
    artifact_root: Path,
    row_id: str,
) -> tuple[list[str], str | None]:
    """Measure every declared file. Returns (errors, measured bundle digest).

    The digest is None whenever any measurement failed, so a caller can never
    compare an attestation against a partially-measured bundle.
    """
    errors: list[str] = []
    declared = _declared_items(evidence)

    if len(declared) > MAX_DECLARED_ITEMS:
        return (
            [
                f"evidence declares {len(declared)} files "
                f"(max {MAX_DECLARED_ITEMS})"
            ],
            None,
        )

    try:
        safe_fs.lstat_checked(artifact_root, expect="dir")
    except safe_fs.FileSecurityError as exc:
        return [f"artifact root is unusable: {exc}"], None

    row_root = artifact_root / row_id
    try:
        safe_fs.lstat_checked(row_root, expect="dir")
    except safe_fs.FileSecurityError as exc:
        if not declared:
            # Nothing declared and nothing on disk is internally consistent;
            # the probe rules elsewhere decide whether that is certifiable.
            return [], bundle_digest([])
        return [f"artifact directory for row {row_id!r} is unusable: {exc}"], None

    seen: dict[str, str] = {}
    measured: list[tuple[str, str]] = []
    total_bytes = 0
    failed = False

    for context, declared_path, declared_size, declared_sha in declared:
        if not isinstance(declared_path, str) or not declared_path:
            errors.append(f"{context}.path: required non-empty string")
            failed = True
            continue

        collision = safe_fs.case_collision_key(declared_path)
        if collision in seen:
            errors.append(
                f"{context}.path: {declared_path!r} collides with "
                f"{seen[collision]!r} on a case-insensitive filesystem"
            )
            failed = True
            continue
        seen[collision] = declared_path

        try:
            target = safe_fs.resolve_under(row_root, declared_path)
            safe_fs.assert_path_chain_safe(row_root, target)
            actual_sha, actual_size = safe_fs.measure_file(
                target, max_bytes=MAX_ARTIFACT_BYTES
            )
        except safe_fs.FileSecurityError as exc:
            errors.append(f"{context}: {exc}")
            failed = True
            continue

        total_bytes += actual_size
        if total_bytes > MAX_BUNDLE_BYTES:
            errors.append(
                f"{context}: evidence bundle exceeds {MAX_BUNDLE_BYTES} bytes"
            )
            return errors, None

        if not isinstance(declared_size, int) or isinstance(declared_size, bool):
            errors.append(f"{context}.sizeBytes: required integer")
            failed = True
            continue
        if declared_size != actual_size:
            errors.append(
                f"{context}.sizeBytes: declared {declared_size}, "
                f"measured {actual_size}"
            )
            failed = True
            continue
        if not isinstance(declared_sha, str) or declared_sha != actual_sha:
            errors.append(
                f"{context}.sha256: declared {str(declared_sha)[:16]}..., "
                f"measured {actual_sha[:16]}..."
            )
            failed = True
            continue

        measured.append((declared_path, actual_sha))

    # Anything on disk that the record does not account for could hide
    # evidence, so the bundle is refused rather than partially trusted.
    try:
        present = safe_fs.iter_bundle_files(
            row_root,
            max_files=MAX_BUNDLE_FILES,
            max_depth=MAX_BUNDLE_DEPTH,
            max_total_bytes=MAX_BUNDLE_BYTES,
        )
    except safe_fs.FileSecurityError as exc:
        errors.append(f"artifact bundle for row {row_id!r}: {exc}")
        return errors, None

    declared_keys = set(seen)
    for path in present:
        relative = path.relative_to(row_root).as_posix()
        if safe_fs.case_collision_key(relative) not in declared_keys:
            errors.append(
                f"undeclared file in evidence bundle: {row_id}/{relative}"
            )
            failed = True

    if failed or errors:
        return errors, None
    return errors, bundle_digest(measured)


# ── Dependency closure ─────────────────────────────────────────────────────


def check_dependency_closure(
    row_id: str,
    deps: Any,
    authority: da.Authority,
) -> list[str]:
    """Match a declared closure against the external dependency authority.

    A fabricated list fails here: every entry must name an identity the
    authority already knows, at the version the authority records, and every
    dependency the authority marks required for this row must be present.
    """
    errors: list[str] = []
    if not isinstance(deps, list):
        return ["dependencyClosure must be a list"]
    if not deps:
        return ["dependencyClosure is empty"]
    if len(deps) > da.MAX_THIRD_PARTY_ENTRIES + da.MAX_PLATFORM_ENTRIES:
        return [f"dependencyClosure declares {len(deps)} entries, which is out of bounds"]

    seen: dict[tuple[str, str], int] = {}
    names_seen: dict[str, tuple[str, str]] = {}
    present: set[tuple[str, str]] = set()

    for index, dep in enumerate(deps):
        context = f"dependencyClosure[{index}]"
        if not isinstance(dep, dict):
            errors.append(f"{context}: must be an object")
            continue
        name = dep.get("name")
        version = dep.get("version")
        source = dep.get("source")
        if not isinstance(name, str) or not name:
            errors.append(f"{context}.name: required non-empty string")
            continue
        if not isinstance(version, str) or not version:
            errors.append(f"{context}.version: required non-empty string")
            continue
        if source not in da.VALID_SOURCES:
            errors.append(f"{context}.source: {source!r} is not a known source")
            continue

        key = (name.casefold(), source)
        if key in seen:
            errors.append(
                f"{context}: duplicate dependency {name!r} from {source!r} "
                f"(already declared at index {seen[key]})"
            )
            continue
        seen[key] = index

        folded = name.casefold()
        if folded in names_seen and names_seen[folded] != key:
            errors.append(
                f"{context}: dependency {name!r} declared from two sources "
                f"({names_seen[folded][1]!r} and {source!r})"
            )
            continue
        names_seen[folded] = key

        entry = authority.lookup(name, source)
        if entry is None:
            errors.append(
                f"{context}: {name!r} from {source!r} is not in the dependency "
                f"authority ({da.AUTHORITY_RELPATH})"
            )
            continue

        expected = entry.get("version")
        if isinstance(expected, str):
            if version != expected:
                errors.append(
                    f"{context}: {name!r} declared version {version!r}, "
                    f"authority records {expected!r}"
                )
                continue
        else:
            pattern = da.compile_version_pattern(entry)
            if pattern is None or not pattern.match(version):
                errors.append(
                    f"{context}: {name!r} version {version!r} does not match "
                    f"the authorised pattern {entry.get('versionPattern')!r}"
                )
                continue

        present.add(key)

    for key in authority.required_keys(row_id):
        if key not in present:
            errors.append(
                f"dependencyClosure is missing {key[0]!r} from {key[1]!r}, "
                f"which the dependency authority requires for row {row_id!r}"
            )

    return errors
