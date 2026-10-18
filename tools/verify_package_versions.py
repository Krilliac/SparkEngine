#!/usr/bin/env python3
"""Require the release version inside every packaged product (REL-100).

A vX.Y.Z tag must embed exactly X.Y.Z in every stable-v1 product and
artifact. Executable ``--version`` output, the MSI ProductVersion and the tag
are checked elsewhere; this checks the CPack archives themselves. Each
PACKAGE (ZIP or TGZ) must:

* carry ``-X.Y.Z-`` in its file name (``CPACK_PACKAGE_FILE_NAME``);
* hold every shipped Spark executable with a ``VS_VERSIONINFO`` resource whose
  fixed FileVersion and ProductVersion are X.Y.Z.0 and whose every string
  table states FileVersion and ProductVersion ``X.Y.Z.0``
  (cmake/SparkWindowsVersionInfo.rc.in). The shipped set is read from
  ``spark_attach_shipped_windows_version_info`` in
  cmake/SparkWindowsVersionInfo.cmake; any other ``Spark*.exe`` must carry the
  same resource, and an archive with no shipped executable is refused. Engine
  first-party ``SparkEngine*.dll`` and ``SparkGame*.dll`` carry the same
  resource; third-party runtime libraries are not engine-owned;
* hold exactly one ``lib/cmake/SparkEngine/SparkEngineConfigVersion.cmake``
  that sets ``PACKAGE_VERSION "X.Y.Z"``.

Usage: verify_package_versions.py --version X.Y.Z PACKAGE [PACKAGE ...]
Exit status: 0 every package matches, 1 any mismatch or unreadable package.
"""
from __future__ import annotations

import argparse
from pathlib import Path, PurePosixPath
import re
import struct
import sys
import tarfile
from typing import Callable, Iterator
import zipfile

REPO_ROOT = Path(__file__).resolve().parents[1]
VERSION_INFO_MODULE = REPO_ROOT / "cmake" / "SparkWindowsVersionInfo.cmake"
CONFIG_VERSION_SUFFIX = "lib/cmake/SparkEngine/SparkEngineConfigVersion.cmake"
RT_VERSION = 16
IMAGE_DIRECTORY_ENTRY_RESOURCE = 2
VS_FIXEDFILEINFO_SIGNATURE = 0xFEEF04BD
MAX_MEMBER_BYTES = 512 * 1024 * 1024
MAX_RESOURCE_DEPTH_ENTRIES = 4096
_VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
_SHIPPED_LIST = re.compile(r"set\(_spark_shipped_executables\s+([A-Za-z0-9_\s]+)\)")


class PackageVersionError(Exception):
    """A package does not embed exactly the release version."""


def shipped_executables(module: Path = VERSION_INFO_MODULE) -> frozenset[str]:
    """The executables CMake stamps with the version resource, as ``<name>.exe``."""
    matches = _SHIPPED_LIST.findall(module.read_text(encoding="utf-8"))
    if len(matches) != 1:
        raise PackageVersionError(f"{module.name} must declare _spark_shipped_executables exactly once")
    names = matches[0].split()
    if not names:
        raise PackageVersionError(f"{module.name} declares no shipped executables")
    return frozenset(f"{name}.exe" for name in names)


def _unpack(fmt: str, data: bytes, offset: int) -> tuple:
    if offset < 0 or offset + struct.calcsize(fmt) > len(data):
        raise PackageVersionError("PE structure points outside the file")
    return struct.unpack_from(fmt, data, offset)


def _version_resources(image: bytes) -> list[bytes]:
    """Every RT_VERSION resource leaf of a PE image (empty when it has none)."""
    if image[:2] != b"MZ":
        raise PackageVersionError("not a PE image")
    (pe_offset,) = _unpack("<I", image, 0x3C)
    if image[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise PackageVersionError("missing PE signature")
    _, section_count, _, _, _, optional_size, _ = _unpack("<HHIIIHH", image, pe_offset + 4)
    optional = pe_offset + 24
    (magic,) = _unpack("<H", image, optional)
    if magic == 0x10B:
        directories = optional + 96
    elif magic == 0x20B:
        directories = optional + 112
    else:
        raise PackageVersionError("unknown PE optional header")
    (directory_count,) = _unpack("<I", image, directories - 4)
    if directory_count <= IMAGE_DIRECTORY_ENTRY_RESOURCE:
        return []
    resource_rva, resource_size = _unpack("<II", image, directories + 8 * IMAGE_DIRECTORY_ENTRY_RESOURCE)
    if resource_rva == 0 or resource_size == 0:
        return []
    sections = [_unpack("<IIII", image, optional + optional_size + 40 * index + 8) for index in range(section_count)]

    def offset_of(rva: int, size: int) -> int:
        for virtual_size, virtual_address, raw_size, raw_pointer in sections:
            span = max(virtual_size, raw_size)
            if virtual_address <= rva and rva + size <= virtual_address + span:
                offset = raw_pointer + rva - virtual_address
                if offset + size > len(image) or rva + size > virtual_address + raw_size:
                    raise PackageVersionError("resource data lies outside the section's file bytes")
                return offset
        raise PackageVersionError("resource RVA is not inside any section")

    root = offset_of(resource_rva, 16)

    def entries(directory: int) -> Iterator[tuple[int, int]]:
        named, identified = _unpack("<HH", image, directory + 12)
        if named + identified > MAX_RESOURCE_DEPTH_ENTRIES:
            raise PackageVersionError("resource directory is implausibly large")
        for index in range(named + identified):
            yield _unpack("<II", image, directory + 16 + 8 * index)

    leaves: list[bytes] = []
    for type_id, type_target in entries(root):
        if type_id != RT_VERSION:
            continue
        if not type_target & 0x80000000:
            raise PackageVersionError("RT_VERSION entry is not a directory")
        for _, name_target in entries(root + (type_target & 0x7FFFFFFF)):
            if not name_target & 0x80000000:
                raise PackageVersionError("RT_VERSION name entry is not a directory")
            for _, language_target in entries(root + (name_target & 0x7FFFFFFF)):
                if language_target & 0x80000000:
                    raise PackageVersionError("RT_VERSION language entry is not a data leaf")
                data_rva, data_size, _, _ = _unpack("<IIII", image, root + language_target)
                start = offset_of(data_rva, data_size)
                leaves.append(image[start:start + data_size])
    return leaves


def _align4(offset: int) -> int:
    return (offset + 3) & ~3


def _block(data: bytes, offset: int, end: int) -> tuple[tuple[str, int, bytes, list], int]:
    """One VERSIONINFO block: (key, wType, value bytes, children) and the next aligned offset."""
    length, value_length, value_type = _unpack("<HHH", data, offset)
    block_end = offset + length
    if length < 6 or block_end > end:
        raise PackageVersionError("malformed VERSIONINFO block length")
    key_end = offset + 6
    while key_end + 1 < block_end and data[key_end:key_end + 2] != b"\0\0":
        key_end += 2
    key = data[offset + 6:key_end].decode("utf-16-le")
    cursor = _align4(key_end + 2)
    if value_type == 1 and value_length:
        # Text values: rc.exe counts WORDs; decode to the terminator.
        raw = data[cursor:block_end]
        text_end = next((index for index in range(0, len(raw) - 1, 2) if raw[index:index + 2] == b"\0\0"), len(raw))
        value = raw[:text_end]
        cursor = _align4(cursor + max(value_length * 2, text_end + 2))
    else:
        value = data[cursor:cursor + value_length]
        cursor = _align4(cursor + value_length)
    children: list = []
    while cursor < block_end:
        child, cursor = _block(data, cursor, block_end)
        children.append(child)
    return (key, value_type, value, children), _align4(block_end)


def version_errors(resource: bytes, version: str) -> list[str]:
    """Differences between one VS_VERSIONINFO resource and X.Y.Z.0."""
    major, minor, patch = (int(part) for part in version.split("."))
    expected_text = f"{version}.0"
    (key, _, fixed, children), _ = _block(resource, 0, len(resource))
    if key != "VS_VERSION_INFO" or len(fixed) < 52:
        return ["resource is not a VS_VERSION_INFO with fixed file information"]
    signature, _, file_ms, file_ls, product_ms, product_ls = struct.unpack_from("<IIIIII", fixed, 0)
    errors: list[str] = []
    if signature != VS_FIXEDFILEINFO_SIGNATURE:
        errors.append("VS_FIXEDFILEINFO signature is wrong")
    expected_ms, expected_ls = (major << 16) | minor, patch << 16
    for label, (ms, ls) in (("FileVersion", (file_ms, file_ls)), ("ProductVersion", (product_ms, product_ls))):
        if (ms, ls) != (expected_ms, expected_ls):
            actual = f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"
            errors.append(f"fixed {label} is {actual}, expected {expected_text}")
    tables = [table for child in children if child[0] == "StringFileInfo" for table in child[3]]
    if not tables:
        errors.append("no StringFileInfo string table")
    for table_key, _, _, strings in tables:
        values = {name: value.decode("utf-16-le") for name, _, value, _ in strings}
        for label in ("FileVersion", "ProductVersion"):
            if values.get(label) != expected_text:
                errors.append(f"string table {table_key} {label} is {values.get(label)!r}, expected {expected_text!r}")
    return errors


def _members(package: Path) -> Iterator[tuple[str, Callable[[], bytes]]]:
    """(posix member path, reader) for every regular file in a ZIP or TGZ; only inspected members are read."""
    name = package.name.lower()
    if name.endswith(".zip"):
        with zipfile.ZipFile(package) as archive:
            for info in archive.infolist():
                if not info.is_dir():
                    yield info.filename, lambda info=info: _bounded(info.filename, info.file_size, archive.read, info)
    elif name.endswith((".tgz", ".tar.gz")):
        with tarfile.open(package, "r:gz") as archive:
            for info in archive:
                if info.isfile():
                    yield info.name, lambda info=info: _bounded(
                        info.name, info.size, lambda member: archive.extractfile(member).read(), info)
    else:
        raise PackageVersionError("package must be a .zip, .tgz or .tar.gz archive")


def _bounded(name: str, size: int, read: Callable, member: object) -> bytes:
    if size > MAX_MEMBER_BYTES:
        raise PackageVersionError(f"{name} is too large to inspect")
    return read(member)


def _direct_installer(package: Path) -> Iterator[tuple[str, Callable[[], bytes]]]:
    """Yield a direct NSIS installer image for version-resource inspection."""
    if package.suffix.lower() == ".exe":
        yield package.name, lambda: _bounded(package.name, package.stat().st_size, lambda _: package.read_bytes(), None)


def package_errors(package: Path, version: str, shipped: frozenset[str]) -> list[str]:
    errors: list[str] = []
    if f"-{version}-" not in package.name:
        errors.append(f"file name does not carry -{version}-")
    config_versions: list[bytes] = []
    checked: list[str] = []
    direct_installer = package.suffix.lower() == ".exe"
    shipped_lower = {name.lower() for name in shipped}
    members = _direct_installer(package) if package.suffix.lower() == ".exe" else _members(package)
    for member, read in members:
        path = PurePosixPath(member)
        if member.endswith(CONFIG_VERSION_SUFFIX):
            config_versions.append(read())
        is_shipped_executable = path.suffix.lower() == ".exe" and (
            direct_installer or path.name.lower() in shipped_lower or path.name.lower().startswith("spark")
        )
        is_first_party_dll = path.suffix.lower() == ".dll" and (
            path.name.lower().startswith("sparkengine") or path.name.lower().startswith("sparkgame")
        )
        if not (is_shipped_executable or is_first_party_dll):
            continue
        checked.append(path.name)
        try:
            resources = _version_resources(read())
            if not resources:
                errors.append(f"{member}: no version resource")
            for resource in resources:
                errors.extend(f"{member}: {error}" for error in version_errors(resource, version))
        except PackageVersionError as error:
            errors.append(f"{member}: {error}")
    if not direct_installer and not {name.lower() for name in checked} & shipped_lower:
        errors.append("no shipped Spark executable was found")
    if direct_installer:
        if not checked:
            errors.append("installer has no version resource")
        return errors
    if len(config_versions) != 1:
        errors.append(f"expected one {CONFIG_VERSION_SUFFIX}, found {len(config_versions)}")
    else:
        # Only literal assignments: the generated file also reuses the variable
        # for its bitness-mismatch message.
        declared = re.findall(r'set\(PACKAGE_VERSION "([^"$]*)"\)', config_versions[0].decode("utf-8", "replace"))
        if declared != [version]:
            errors.append(f"SparkEngineConfigVersion.cmake sets PACKAGE_VERSION {declared}, expected [{version!r}]")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True)
    parser.add_argument("packages", nargs="+", type=Path)
    args = parser.parse_args(argv)
    if not _VERSION.fullmatch(args.version):
        print(f"--version must be X.Y.Z, got {args.version!r}", file=sys.stderr)
        return 1
    try:
        shipped = shipped_executables()
    except (OSError, PackageVersionError) as error:
        print(f"cannot read the shipped executable set: {error}", file=sys.stderr)
        return 1
    failed = False
    for package in args.packages:
        try:
            errors = package_errors(package, args.version, shipped)
        except (OSError, zipfile.BadZipFile, tarfile.TarError, PackageVersionError) as error:
            errors = [f"unreadable package: {error}"]
        for error in errors:
            print(f"{package.name}: {error}", file=sys.stderr)
        failed = failed or bool(errors)
        if not errors:
            print(f"{package.name}: every packaged product embeds {args.version}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
