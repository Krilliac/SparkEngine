#!/usr/bin/env python3
"""Preflight portable archives and validate extracted package integrity."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import stat
import sys
import tarfile
from typing import Iterable
import unicodedata
import zipfile
import zlib


TWO_GIB = 2 * 1024 * 1024 * 1024
LIBRARY_WARNING_BYTES = int(TWO_GIB * 0.9)
MAX_ARCHIVE_MEMBERS = 100_000
MAX_TOTAL_EXPANDED_BYTES = 16 * 1024 * 1024 * 1024
MAX_ARCHIVE_CONTAINER_OVERHEAD_BYTES = 512 * 1024 * 1024
STATIC_LIBRARY_NAMES = ("SparkEngineLib.lib", "libSparkEngineLib.a")
WINDOWS_REPARSE_POINT = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)

FORBIDDEN_PACKAGE_PATHS = frozenset(
    path.casefold()
    for path in (
        "bin/Assets/Textures/MMOFPS/fx/blob_shadow.png",
        "tools/accept_lit.cfg",
    )
)
FORBIDDEN_PACKAGE_PREFIXES = tuple(
    path.casefold()
    for path in (
        "tools/assetgen/",
    )
)
FORBIDDEN_TOP_LEVEL_DIRECTORIES = frozenset(
    name.casefold()
    for name in (
        ".git",
        ".github",
        ".pytest_cache",
        "_CPack_Packages",
        "build",
        "CMakeFiles",
        "LivePackages",
        "LiveProjects",
        "package-extract",
        "Saves",
        "Screenshots",
        "Temp",
        "TestScreenshots",
    )
)

FORBIDDEN_TEMPLATE_DIRECTORIES = frozenset(
    name.casefold()
    for name in (
        ".cache",
        ".idea",
        ".vs",
        "__pycache__",
        "build",
        "cmakefiles",
        "logs",
        "out",
        "saved",
        "saves",
        "screenshots",
        "temp",
        "testresults",
    )
)
FORBIDDEN_TEMPLATE_FILENAMES = frozenset(
    name.casefold()
    for name in (
        ".ninja_deps",
        ".ninja_log",
        "build.ninja",
        "cmake_install.cmake",
        "cmakecache.txt",
        "install_manifest.txt",
        "makefile",
        "mmo_data.db",
        "spark_trace.json",
    )
)
FORBIDDEN_TEMPLATE_SUFFIXES = (
    ".dmp",
    ".idb",
    ".ilk",
    ".lastbuildstate",
    ".log",
    ".pch",
    ".pdb",
    ".sln",
    ".tlog",
    ".tmp",
    ".trace",
    ".vcxproj",
    ".vcxproj.filters",
)


class ValidationError(RuntimeError):
    """The extracted package violates a release invariant."""


def _format_bytes(size: int) -> str:
    return f"{size:,} bytes ({size / (1024 * 1024):,.1f} MiB)"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _is_link_like(path: Path) -> bool:
    if path.is_symlink():
        return True
    is_junction = getattr(path, "is_junction", None)
    if callable(is_junction) and is_junction():
        return True
    try:
        attributes = getattr(path.lstat(), "st_file_attributes", 0)
    except FileNotFoundError:
        return False
    return bool(attributes & WINDOWS_REPARSE_POINT)


def _raise_walk_error(error: OSError) -> None:
    raise error


def _regular_files(root: Path) -> Iterable[Path]:
    for directory, directory_names, file_names in os.walk(
        root, followlinks=False, onerror=_raise_walk_error
    ):
        directory_names.sort(key=str.casefold)
        file_names.sort(key=str.casefold)
        directory_path = Path(directory)
        for directory_name in directory_names:
            path = directory_path / directory_name
            if _is_link_like(path):
                raise ValidationError(f"Link-like package directory is forbidden: {path}")
        for file_name in file_names:
            path = directory_path / file_name
            if _is_link_like(path):
                raise ValidationError(f"Link-like package file is forbidden: {path}")
            if not path.is_file():
                raise ValidationError(f"Non-regular package entry is forbidden: {path}")
            yield path


def _check_file_limit(path: Path, size: int, limit: int = TWO_GIB) -> None:
    if size >= limit:
        raise ValidationError(
            f"Package file must be smaller than {_format_bytes(limit)}: "
            f"{path} is {_format_bytes(size)}"
        )


def _normalize_archive_destination(name: str) -> str:
    if not name or "\0" in name:
        raise ValidationError("Archive member has an empty or NUL-containing path")

    portable_name = name.replace("\\", "/")
    if portable_name.startswith("//"):
        raise ValidationError(f"UNC archive member path is forbidden: {name!r}")
    if portable_name.startswith("/"):
        raise ValidationError(f"Absolute archive member path is forbidden: {name!r}")
    if re.match(r"^[A-Za-z]:", portable_name):
        raise ValidationError(f"Drive-qualified archive member path is forbidden: {name!r}")

    components: list[str] = []
    for component in portable_name.split("/"):
        if component in ("", "."):
            continue
        if component == ".." or component.rstrip(" .") == "..":
            raise ValidationError(f"Traversal archive member path is forbidden: {name!r}")
        if component.rstrip(" .") != component:
            raise ValidationError(
                f"Archive member path has a platform-ambiguous trailing dot/space: {name!r}"
            )
        components.append(unicodedata.normalize("NFC", component))

    if not components:
        raise ValidationError(f"Archive member has no destination path: {name!r}")
    return "/".join(components)


class _ArchiveMemberTracker:
    def __init__(self) -> None:
        self.member_count = 0
        self.total_expanded_bytes = 0
        self.destinations: dict[str, tuple[str, str]] = {}
        self.required_directories: dict[str, str] = {}

    def add(self, name: str, kind: str, size: int) -> str:
        self.member_count += 1
        if self.member_count > MAX_ARCHIVE_MEMBERS:
            raise ValidationError(
                f"Archive has more than {MAX_ARCHIVE_MEMBERS:,} members"
            )
        if size < 0:
            raise ValidationError(f"Archive member has a negative size: {name!r}")
        if size >= TWO_GIB:
            raise ValidationError(
                f"Archive member must be smaller than {_format_bytes(TWO_GIB)}: "
                f"{name!r} is {_format_bytes(size)}"
            )
        self.total_expanded_bytes += size
        if self.total_expanded_bytes > MAX_TOTAL_EXPANDED_BYTES:
            raise ValidationError(
                "Archive expanded size exceeds "
                f"{_format_bytes(MAX_TOTAL_EXPANDED_BYTES)}"
            )

        destination = _normalize_archive_destination(name)
        folded = destination.casefold()
        previous = self.destinations.get(folded)
        if previous is not None:
            collision = "duplicate" if previous[0] == destination else "case-colliding"
            raise ValidationError(
                f"Archive has {collision} destinations: {previous[0]!r} and {destination!r}"
            )

        components = destination.split("/")
        for index in range(1, len(components)):
            ancestor = "/".join(components[:index])
            ancestor_folded = ancestor.casefold()
            previous_ancestor = self.destinations.get(ancestor_folded)
            if previous_ancestor is not None:
                if previous_ancestor[1] != "directory":
                    raise ValidationError(
                        "Archive destination is nested below a non-directory member: "
                        f"{destination!r} below {previous_ancestor[0]!r}"
                    )
                if previous_ancestor[0] != ancestor:
                    raise ValidationError(
                        "Archive has case-colliding destination hierarchy: "
                        f"{previous_ancestor[0]!r} and {ancestor!r}"
                    )
            required_spelling = self.required_directories.get(ancestor_folded)
            if required_spelling is not None and required_spelling != ancestor:
                raise ValidationError(
                    "Archive has case-colliding destination hierarchy: "
                    f"{required_spelling!r} and {ancestor!r}"
                )
            self.required_directories[ancestor_folded] = ancestor

        required_spelling = self.required_directories.get(folded)
        if required_spelling is not None:
            if kind != "directory":
                raise ValidationError(
                    f"Archive file destination conflicts with a directory: {destination!r}"
                )
            if required_spelling != destination:
                raise ValidationError(
                    "Archive has case-colliding destination hierarchy: "
                    f"{required_spelling!r} and {destination!r}"
                )

        self.destinations[folded] = (destination, kind)
        return destination


def _zip_member_kind(member: zipfile.ZipInfo) -> str:
    unix_type = stat.S_IFMT((member.external_attr >> 16) & 0xFFFF)
    is_directory = member.is_dir() or bool(member.external_attr & 0x10)
    if is_directory:
        if unix_type not in (0, stat.S_IFDIR):
            raise ValidationError(
                f"ZIP directory has a non-directory entry type: {member.filename!r}"
            )
        return "directory"
    if unix_type not in (0, stat.S_IFREG):
        raise ValidationError(
            f"ZIP link/device/special entry is forbidden: {member.filename!r}"
        )
    return "file"


def _consume_member(stream: object, expected_size: int, description: str) -> None:
    actual_size = 0
    while True:
        chunk = stream.read(1024 * 1024)  # type: ignore[attr-defined]
        if not chunk:
            break
        actual_size += len(chunk)
        if actual_size > expected_size:
            raise ValidationError(
                f"Malformed archive member expands beyond its declared size: {description}"
            )
    if actual_size != expected_size:
        raise ValidationError(
            f"Malformed archive member size for {description}: "
            f"declared={expected_size:,}, actual={actual_size:,}"
        )


def _validate_zip_archive(archive_path: Path) -> _ArchiveMemberTracker:
    tracker = _ArchiveMemberTracker()
    try:
        with zipfile.ZipFile(archive_path, "r") as archive:
            for member in archive.infolist():
                kind = _zip_member_kind(member)
                if member.flag_bits & 0x1:
                    raise ValidationError(
                        f"Encrypted ZIP entry is forbidden: {member.filename!r}"
                    )
                tracker.add(member.filename, kind, member.file_size)
                if kind == "directory":
                    if member.file_size != 0:
                        raise ValidationError(
                            f"ZIP directory has a non-zero size: {member.filename!r}"
                        )
                    continue
                with archive.open(member, "r") as stream:
                    _consume_member(stream, member.file_size, repr(member.filename))
    except ValidationError:
        raise
    except (
        EOFError,
        NotImplementedError,
        OSError,
        RuntimeError,
        ValueError,
        zipfile.BadZipFile,
        zipfile.LargeZipFile,
        zlib.error,
    ) as error:
        raise ValidationError(f"Malformed ZIP archive: {error}") from error
    return tracker


def _validate_tar_gz_archive(archive_path: Path) -> _ArchiveMemberTracker:
    tracker = _ArchiveMemberTracker()
    try:
        with tarfile.open(archive_path, mode="r:gz") as archive:
            regular_members: list[tarfile.TarInfo] = []
            for member in archive.getmembers():
                if member.isdir():
                    kind = "directory"
                elif member.isfile():
                    kind = "file"
                elif member.issym() or member.islnk():
                    raise ValidationError(
                        f"TAR symlink/hardlink entry is forbidden: {member.name!r}"
                    )
                else:
                    raise ValidationError(
                        f"TAR device/special entry is forbidden: {member.name!r}"
                    )

                tracker.add(member.name, kind, member.size)
                if kind == "directory":
                    if member.size != 0:
                        raise ValidationError(
                            f"TAR directory has a non-zero size: {member.name!r}"
                        )
                    continue
                regular_members.append(member)

            max_container_size = (
                tracker.total_expanded_bytes + MAX_ARCHIVE_CONTAINER_OVERHEAD_BYTES
            )
            if archive.fileobj.tell() > max_container_size:
                raise ValidationError(
                    "tar.gz container overhead exceeds "
                    f"{_format_bytes(MAX_ARCHIVE_CONTAINER_OVERHEAD_BYTES)}"
                )

            for member in regular_members:
                stream = archive.extractfile(member)
                if stream is None:
                    raise ValidationError(
                        f"Malformed TAR member has no readable payload: {member.name!r}"
                    )
                with stream:
                    _consume_member(stream, member.size, repr(member.name))

            while archive.fileobj.read(1024 * 1024):
                if archive.fileobj.tell() > max_container_size:
                    raise ValidationError(
                        "tar.gz container overhead exceeds "
                        f"{_format_bytes(MAX_ARCHIVE_CONTAINER_OVERHEAD_BYTES)}"
                    )
    except ValidationError:
        raise
    except (EOFError, OSError, tarfile.TarError, ValueError, zlib.error) as error:
        raise ValidationError(f"Malformed tar.gz archive: {error}") from error
    return tracker


def validate_archive(archive_path: Path) -> None:
    archive_path = archive_path.absolute()
    if _is_link_like(archive_path):
        raise ValidationError(f"Portable archive must not be link-like: {archive_path}")
    if not archive_path.is_file():
        raise ValidationError(f"Portable archive does not exist: {archive_path}")
    archive_path = archive_path.resolve(strict=True)
    _check_file_limit(archive_path, archive_path.stat().st_size)

    folded_name = archive_path.name.casefold()
    expected_format = None
    if folded_name.endswith(".tar.gz"):
        expected_format = "tar.gz"
    elif folded_name.endswith(".zip"):
        expected_format = "ZIP"
    else:
        raise ValidationError(
            f"Portable archive must use a .zip or .tar.gz extension: {archive_path.name}"
        )

    with archive_path.open("rb") as stream:
        signature = stream.read(4)
    detected_format = None
    if signature.startswith((b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08")):
        detected_format = "ZIP"
    elif signature.startswith(b"\x1f\x8b"):
        detected_format = "tar.gz"

    if detected_format is not None and detected_format != expected_format:
        raise ValidationError(
            "Archive format/extension mismatch: "
            f"{archive_path.name} contains {detected_format} data"
        )
    if detected_format is None:
        raise ValidationError(
            f"Malformed {expected_format} archive: unrecognized file signature"
        )

    tracker = (
        _validate_zip_archive(archive_path)
        if expected_format == "ZIP"
        else _validate_tar_gz_archive(archive_path)
    )
    print(
        f"Archive member preflight passed: {tracker.member_count} members, "
        f"expanded={_format_bytes(tracker.total_expanded_bytes)}, archive={archive_path}"
    )


def _check_library_boundary(path: Path, size: int) -> str:
    _check_file_limit(path, size)
    headroom = TWO_GIB - size
    level = "WARNING" if size >= LIBRARY_WARNING_BYTES else "OK"
    return (
        f"{level}: {path.name} is {_format_bytes(size)}; "
        f"headroom below 2 GiB is {_format_bytes(headroom)}"
    )


def _find_static_libraries(root: Path) -> dict[str, Path]:
    found: dict[str, Path] = {}
    for name in STATIC_LIBRARY_NAMES:
        candidate = root / "lib" / name
        if candidate.is_file() and not candidate.is_symlink():
            found[name] = candidate
    return found


def _validate_template_hygiene(template_root: Path) -> None:
    if not template_root.is_dir():
        raise ValidationError(f"Extracted template root is missing: {template_root}")

    violations: list[str] = []
    for directory, directory_names, file_names in os.walk(template_root):
        directory_path = Path(directory)
        retained_directories: list[str] = []
        for name in sorted(directory_names, key=str.casefold):
            path = directory_path / name
            if name.casefold() in FORBIDDEN_TEMPLATE_DIRECTORIES:
                relative = path.relative_to(template_root).as_posix()
                violations.append(f"directory: {relative}")
            else:
                retained_directories.append(name)
        directory_names[:] = retained_directories

        for name in sorted(file_names, key=str.casefold):
            path = directory_path / name
            if not path.is_file():
                continue
            folded_name = name.casefold()
            if folded_name in FORBIDDEN_TEMPLATE_FILENAMES or folded_name.endswith(
                FORBIDDEN_TEMPLATE_SUFFIXES
            ):
                relative = path.relative_to(template_root).as_posix()
                violations.append(f"file: {relative}")

    if violations:
        joined = "\n  ".join(violations)
        raise ValidationError(f"Forbidden build/runtime debris found in templates:\n  {joined}")


def _validate_package_hygiene(package_root: Path) -> None:
    violations: list[str] = []
    for child in package_root.iterdir():
        if child.is_dir() and child.name.casefold() in FORBIDDEN_TOP_LEVEL_DIRECTORIES:
            violations.append(f"top-level directory: {child.name}")

    for directory, directory_names, _file_names in os.walk(package_root):
        directory_path = Path(directory)
        for name in directory_names:
            relative = (directory_path / name).relative_to(package_root).as_posix()
            if f"{relative.casefold()}/".startswith(FORBIDDEN_PACKAGE_PREFIXES):
                violations.append(f"forbidden path: {relative}/")

    for path in _regular_files(package_root):
        relative = path.relative_to(package_root).as_posix()
        folded = relative.casefold()
        if folded in FORBIDDEN_PACKAGE_PATHS or folded.startswith(FORBIDDEN_PACKAGE_PREFIXES):
            violations.append(f"forbidden path: {relative}")

    if violations:
        joined = "\n  ".join(sorted(violations, key=str.casefold))
        raise ValidationError(f"Forbidden package content found:\n  {joined}")


def validate_package(package_root: Path, stage_root: Path | None, archive: Path | None) -> None:
    package_root = package_root.absolute()
    if _is_link_like(package_root):
        raise ValidationError(f"Extracted package root must not be link-like: {package_root}")
    if not package_root.is_dir():
        raise ValidationError(f"Extracted package root does not exist: {package_root}")
    package_root = package_root.resolve(strict=True)

    if archive is not None:
        archive = archive.absolute()
        if _is_link_like(archive):
            raise ValidationError(f"Portable archive must not be link-like: {archive}")
        if not archive.is_file():
            raise ValidationError(f"Portable archive does not exist: {archive}")
        archive = archive.resolve(strict=True)
        _check_file_limit(archive, archive.stat().st_size)

    template_root = package_root / "share" / "SparkEngine" / "templates"
    _validate_package_hygiene(package_root)
    _validate_template_hygiene(template_root)

    file_count = 0
    for path in _regular_files(package_root):
        size = path.stat().st_size
        _check_file_limit(path, size)
        file_count += 1

    package_libraries = _find_static_libraries(package_root)
    for library in package_libraries.values():
        print(_check_library_boundary(library, library.stat().st_size))

    if stage_root is not None:
        stage_root = stage_root.resolve()
        if not stage_root.is_dir():
            raise ValidationError(f"Staged package root does not exist: {stage_root}")
        staged_libraries = _find_static_libraries(stage_root)
        if not staged_libraries:
            raise ValidationError(
                "Staged SDK has no SparkEngine static library to compare with the archive"
            )
        if set(staged_libraries) != set(package_libraries):
            raise ValidationError(
                "Staged and extracted static-library inventories differ: "
                f"staged={sorted(staged_libraries)}, extracted={sorted(package_libraries)}"
            )
        for name, staged_library in staged_libraries.items():
            extracted_library = package_libraries[name]
            staged_hash = _sha256(staged_library)
            extracted_hash = _sha256(extracted_library)
            if staged_hash != extracted_hash:
                raise ValidationError(
                    f"Staged/extracted SHA-256 mismatch for {name}: "
                    f"staged={staged_hash}, extracted={extracted_hash}"
                )
            print(f"OK: staged/extracted SHA-256 match for {name}: {extracted_hash}")

    print(
        f"Extracted package content validation passed: {file_count} files, "
        f"templates={template_root}"
    )


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--package-root", type=Path)
    mode.add_argument("--preflight-archive", type=Path)
    parser.add_argument("--stage-root", type=Path)
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args(argv)
    if args.preflight_archive is not None and (
        args.stage_root is not None or args.archive is not None
    ):
        parser.error("--stage-root and --archive are only valid with --package-root")
    return args


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.preflight_archive is not None:
            validate_archive(args.preflight_archive)
        else:
            validate_package(args.package_root, args.stage_root, args.archive)
    except (OSError, ValidationError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
