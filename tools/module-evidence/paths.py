#!/usr/bin/env python3
"""Exact lexical and canonical path validation for module source trees.

A module's declared `sourceDirectory` is load-bearing evidence: it is the tree
whose contents are claimed to build the shipped shared library.  Accepting
anything other than the one exact directory lets a manifest point at an empty
subdirectory, at a sibling module, or — through a symlink or NTFS junction —
at a tree entirely outside the repository, while still reading as valid.

Validation therefore happens in four independent layers, all of which must
pass:

  1. Lexical  — the string is exactly ``GameModules/<module>/Source``.
  2. On-disk case — every segment matches the real directory entry byte for
     byte, so ``source`` is rejected on case-insensitive filesystems.
  3. Reparse — no component from the repository root down is a symlink,
     junction, or any other reparse point.
  4. Canonical — the fully resolved path is exactly the resolved expected
     path, and lies inside the resolved repository root.
"""

from __future__ import annotations

import os
import stat
from pathlib import Path, PurePosixPath

MODULE_ROOT_DIRNAME = "GameModules"
MODULE_SOURCE_DIRNAME = "Source"

# Reserved DOS device names; a path segment equal to one of these (with or
# without an extension) resolves to a device rather than a directory.
_RESERVED_SEGMENTS = frozenset(
    {"con", "prn", "aux", "nul"}
    | {f"com{i}" for i in range(1, 10)}
    | {f"lpt{i}" for i in range(1, 10)}
)


def expected_source_directory(module_name: str) -> str:
    """The one lexical form a module's sourceDirectory is permitted to take."""
    return f"{MODULE_ROOT_DIRNAME}/{module_name}/{MODULE_SOURCE_DIRNAME}"


def _is_reparse_point(path: Path) -> bool:
    """True if `path` is a symlink, NTFS junction, or other reparse point."""
    try:
        st = path.lstat()
    except OSError:
        return False
    if stat.S_ISLNK(st.st_mode):
        return True
    attrs = getattr(st, "st_file_attributes", 0)
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return bool(attrs & reparse_flag)


def check_lexical(value: str, module_name: str) -> list[str]:
    """Layer 1: the declared string must be the exact expected literal."""
    errors: list[str] = []
    if not isinstance(value, str) or not value:
        return ["sourceDirectory must be a non-empty string"]

    if "\\" in value:
        errors.append(
            f"sourceDirectory {value!r} uses backslashes — "
            f"forward-slash repository-relative paths only"
        )
    if ":" in value:
        errors.append(
            f"sourceDirectory {value!r} contains ':' — drive-qualified paths "
            f"and NTFS alternate data streams are rejected"
        )
    if value.startswith("/"):
        errors.append(f"sourceDirectory {value!r} must be repository-relative")
    if "~" in value or "$" in value or "%" in value:
        errors.append(
            f"sourceDirectory {value!r} contains a home/variable/short-name "
            f"reference — literal repository paths only"
        )

    segments = value.split("/")
    for seg in segments:
        if seg == "":
            errors.append(
                f"sourceDirectory {value!r} contains an empty path segment"
            )
        elif seg == ".":
            errors.append(
                f"sourceDirectory {value!r} contains a '.' segment — "
                f"canonical paths only"
            )
        elif seg == "..":
            errors.append(
                f"sourceDirectory {value!r} contains a '..' traversal segment"
            )
        elif seg.split(".")[0].lower() in _RESERVED_SEGMENTS:
            errors.append(
                f"sourceDirectory {value!r} contains reserved device name {seg!r}"
            )

    if errors:
        return errors

    expected = expected_source_directory(module_name)
    if value != expected:
        return [
            f"sourceDirectory {value!r} is not the exact required form "
            f"{expected!r} — the module root, a subdirectory of Source, a "
            f"case variant, or any other tree is not this module's source"
        ]
    return []


def check_on_disk(value: str, repo_root: Path) -> list[str]:
    """Layers 2-4: case, reparse points, and canonical identity on disk."""
    errors: list[str] = []
    try:
        root = repo_root.resolve(strict=True)
    except OSError as exc:
        return [f"repository root {repo_root} is not resolvable: {exc}"]

    segments = PurePosixPath(value).parts
    walked = root
    for seg in segments:
        parent = walked
        # Layer 2: the entry must exist with exactly this spelling.  Listing
        # the parent is the only portable way to learn the on-disk case on a
        # case-insensitive filesystem, where `(parent / seg).exists()` is true
        # for every case variant.
        try:
            entries = {e.name for e in os.scandir(parent)}
        except OSError as exc:
            return errors + [
                f"sourceDirectory {value!r}: cannot list {parent}: {exc}"
            ]
        if seg not in entries:
            case_match = [e for e in entries if e.lower() == seg.lower()]
            if case_match:
                return errors + [
                    f"sourceDirectory {value!r}: path segment {seg!r} does not "
                    f"match the on-disk name {case_match[0]!r} — exact case is "
                    f"required so case-aliased paths cannot stand in for the "
                    f"real source tree"
                ]
            return errors + [
                f"sourceDirectory {value!r}: path segment {seg!r} does not "
                f"exist under {parent}"
            ]

        walked = parent / seg
        # Layer 3: no component may be a reparse point.
        if _is_reparse_point(walked):
            return errors + [
                f"sourceDirectory {value!r}: component {seg!r} is a symlink, "
                f"junction or reparse point — module source evidence must be a "
                f"real in-repository directory"
            ]

    if not walked.is_dir():
        return errors + [f"sourceDirectory {value!r} is not a directory"]

    # Layer 4: canonical identity and containment.
    resolved = walked.resolve()
    expected_resolved = root.joinpath(*segments)
    if resolved != expected_resolved:
        return errors + [
            f"sourceDirectory {value!r} resolves to {resolved}, not the "
            f"expected {expected_resolved} — path escapes its declared location"
        ]
    if root not in resolved.parents:
        return errors + [
            f"sourceDirectory {value!r} resolves outside the repository root"
        ]
    return errors


def check_source_directory(value: str, module_name: str, repo_root: Path) -> list[str]:
    """Full validation of a declared module source directory."""
    errors = check_lexical(value, module_name)
    if errors:
        return errors
    return check_on_disk(value, repo_root)


def check_relative_artifact_path(value: str, label: str) -> list[str]:
    """Validate an evidence artifact path (a build output, not a source tree).

    Artifact paths need not exist at validation time — the producer creates
    them — but they must be unambiguous repository-relative literals so that a
    binding always names one predictable location.
    """
    if not isinstance(value, str) or not value:
        return [f"{label} must be a non-empty string"]
    errors: list[str] = []
    if "\\" in value:
        errors.append(f"{label} {value!r} uses backslashes — forward slashes only")
    if ":" in value:
        errors.append(f"{label} {value!r} contains ':' — drive letters and "
                      f"alternate data streams are rejected")
    if value.startswith("/"):
        errors.append(f"{label} {value!r} must be repository-relative")
    if "~" in value or "$" in value or "%" in value:
        errors.append(f"{label} {value!r} contains a home or variable reference")
    if "*" in value or "?" in value:
        errors.append(
            f"{label} {value!r} contains a glob wildcard — an evidence binding "
            f"must name one exact artifact, not a pattern that can match "
            f"whatever happens to be present"
        )
    for seg in value.split("/"):
        if seg == "":
            errors.append(f"{label} {value!r} contains an empty path segment")
        elif seg in (".", ".."):
            errors.append(f"{label} {value!r} contains a {seg!r} segment")
        elif seg.split(".")[0].lower() in _RESERVED_SEGMENTS:
            errors.append(f"{label} {value!r} contains reserved name {seg!r}")
    return errors
