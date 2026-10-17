#!/usr/bin/env python3
"""Keep the macOS minimum version identical in CMake, CI, docs and built images.

PLT-220: the root CMakeLists.txt declares ``SPARK_MACOS_MIN_VERSION``, the one
canonical macOS deployment target. This tool fails when any of these disagree
with it:

* every ``CMAKE_OSX_DEPLOYMENT_TARGET`` value in ``.github/workflows/*.yml`` and
  in ``CMakePresets.json`` cache variables;
* the declared public documentation anchors (README, wiki Home, System
  Requirements, Mac compatibility analysis);
* the macOS CI runner label quoted by the documentation anchors, which must be
  the single ``macos-*`` label the workflows use;
* with ``--binary``, the minimum OS version recorded in each Mach-O image
  (``LC_BUILD_VERSION`` minos, or legacy ``LC_VERSION_MIN_MACOSX``), including
  every slice of a universal (fat) image.

A missing anchor, an empty CI value set, or an image without a version load
command is an error: a check that finds nothing to compare never passes.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path
from typing import Any, Iterable

REPO_ROOT = Path(__file__).resolve().parents[1]

Version = tuple[int, ...]

_CANONICAL = re.compile(r'^\s*set\(\s*SPARK_MACOS_MIN_VERSION\s+"(\d+(?:\.\d+){0,2})"\s*\)', re.MULTILINE)
_WORKFLOW_TARGET = re.compile(r"CMAKE_OSX_DEPLOYMENT_TARGET\s*[=:]\s*[\"']?(\d+(?:\.\d+){0,2})")
_WORKFLOW_RUNNER = re.compile(r"(?:runs-on|os)\s*:\s*[\"']?(macos-[A-Za-z0-9.-]+)")

# (file, line selector, value patterns). Every selector must match at least one
# line, and every value pattern must match each selected line.
DOC_VERSION_ANCHORS: tuple[tuple[str, str, tuple[str, ...]], ...] = (
    ("README.md", r"^\| OS \(build floor", (r"macOS (\d+(?:\.\d+)*)\+",)),
    ("wiki/Home.md", r"^\| macOS \d", (r"^\| macOS (\d+(?:\.\d+)*)\+",)),
    (
        "wiki/platform/System-Requirements.md",
        r"^\| \*\*OS Version\*\* \| macOS",
        (r"^\| \*\*OS Version\*\* \| macOS (\d+(?:\.\d+)*)", r"CMAKE_OSX_DEPLOYMENT_TARGET=(\d+(?:\.\d+)*)"),
    ),
    (
        "wiki/research/Mac-Compatibility-Analysis.md",
        r"CMAKE_OSX_DEPLOYMENT_TARGET=",
        (r"CMAKE_OSX_DEPLOYMENT_TARGET=(\d+(?:\.\d+)*)",),
    ),
    (
        "wiki/research/Mac-Compatibility-Analysis.md",
        r"^\| \*\*macOS\*\* \|",
        (r"^\| \*\*macOS\*\* \| macOS (\d+(?:\.\d+)*)",),
    ),
)
DOC_RUNNER_ANCHORS: tuple[tuple[str, str, str], ...] = (
    ("wiki/platform/System-Requirements.md", r"^\| macOS CI runner \|", r"`(macos-[A-Za-z0-9.-]+)`"),
    ("wiki/advanced/Testing.md", r"^\| `build-macos` \|", r"^\| `build-macos` \| (macos-[A-Za-z0-9.-]+) \|"),
)

# Mach-O constants (mach-o/loader.h, mach-o/fat.h).
MH_MAGIC = 0xFEEDFACE
MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF
LC_VERSION_MIN_MACOSX = 0x24
LC_BUILD_VERSION = 0x32
PLATFORM_MACOS = 1
_MAX_FAT_ARCHS = 64


class MachOError(ValueError):
    """The file is not a well-formed Mach-O image with one macOS minimum version."""


def parse_version(text: str) -> Version:
    """Parse ``13.3`` / ``13.3.0`` into a comparable tuple without trailing zeros."""
    if not re.fullmatch(r"\d+(?:\.\d+){0,2}", text):
        raise ValueError(f"not a macOS version: {text!r}")
    parts = [int(part) for part in text.split(".")]
    while len(parts) > 1 and parts[-1] == 0:
        parts.pop()
    return tuple(parts)


def format_version(version: Version) -> str:
    return ".".join(str(part) for part in version)


def canonical(repo: Path) -> Version:
    """Return SPARK_MACOS_MIN_VERSION from the root CMakeLists.txt."""
    text = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
    matches = _CANONICAL.findall(text)
    if len(matches) != 1:
        raise ValueError(f"CMakeLists.txt must set SPARK_MACOS_MIN_VERSION exactly once (found {len(matches)})")
    return parse_version(matches[0])


def _workflow_files(repo: Path) -> list[Path]:
    return sorted((repo / ".github" / "workflows").glob("*.yml"))


def _preset_values(node: Any, location: str) -> Iterable[tuple[str, str]]:
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "CMAKE_OSX_DEPLOYMENT_TARGET":
                raw = value.get("value") if isinstance(value, dict) else value
                yield location, str(raw)
            else:
                yield from _preset_values(value, location)
    elif isinstance(node, list):
        for item in node:
            yield from _preset_values(item, location)


def ci_values(repo: Path) -> list[tuple[str, str]]:
    """Return (location, raw value) for every deployment target CI or presets pass."""
    values: list[tuple[str, str]] = []
    for workflow in _workflow_files(repo):
        relative = workflow.relative_to(repo).as_posix()
        for number, line in enumerate(workflow.read_text(encoding="utf-8").splitlines(), start=1):
            for match in _WORKFLOW_TARGET.finditer(line):
                values.append((f"{relative}:{number}", match.group(1)))
    presets = repo / "CMakePresets.json"
    if presets.is_file():
        values.extend(_preset_values(json.loads(presets.read_text(encoding="utf-8")), "CMakePresets.json"))
    return values


def ci_runners(repo: Path) -> set[str]:
    """Return every macOS runner label used by the workflows."""
    runners: set[str] = set()
    for workflow in _workflow_files(repo):
        runners.update(_WORKFLOW_RUNNER.findall(workflow.read_text(encoding="utf-8")))
    return runners


def _anchor_lines(repo: Path, relative: str, selector: str) -> list[tuple[int, str]]:
    path = repo / relative
    if not path.is_file():
        return []
    pattern = re.compile(selector)
    return [
        (number, line)
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1)
        if pattern.search(line)
    ]


def doc_values(repo: Path) -> tuple[list[tuple[str, str]], list[str]]:
    """Return (location, raw value) per documentation anchor, plus anchor errors."""
    values: list[tuple[str, str]] = []
    errors: list[str] = []
    for relative, selector, patterns in DOC_VERSION_ANCHORS:
        lines = _anchor_lines(repo, relative, selector)
        if not lines:
            errors.append(f"{relative}: no line matches the macOS version anchor {selector!r}")
        for number, line in lines:
            for pattern in patterns:
                match = re.search(pattern, line)
                if match is None:
                    errors.append(f"{relative}:{number}: anchor line lacks a value for {pattern!r}")
                else:
                    values.append((f"{relative}:{number}", match.group(1)))
    return values, errors


def doc_runners(repo: Path) -> tuple[list[tuple[str, str]], list[str]]:
    values: list[tuple[str, str]] = []
    errors: list[str] = []
    for relative, selector, pattern in DOC_RUNNER_ANCHORS:
        lines = _anchor_lines(repo, relative, selector)
        if not lines:
            errors.append(f"{relative}: no line matches the macOS runner anchor {selector!r}")
        for number, line in lines:
            match = re.search(pattern, line)
            if match is None:
                errors.append(f"{relative}:{number}: runner anchor line names no macos-* runner")
            else:
                values.append((f"{relative}:{number}", match.group(1)))
    return values, errors


def _decode_packed_version(packed: int) -> Version:
    """Decode the ``xxxx.yy.zz`` nibble encoding used by Mach-O version fields."""
    return parse_version(f"{packed >> 16}.{(packed >> 8) & 0xFF}.{packed & 0xFF}")


def _thin_min_version(data: bytes, base: int, end: int) -> Version:
    if end - base < 28:
        raise MachOError("truncated Mach-O header")
    magic_le = struct.unpack_from("<I", data, base)[0]
    if magic_le in (MH_MAGIC, MH_MAGIC_64):
        order = "<"
    elif struct.unpack_from(">I", data, base)[0] in (MH_MAGIC, MH_MAGIC_64):
        order = ">"
    else:
        raise MachOError(f"not a Mach-O image (magic 0x{magic_le:08x})")
    magic = struct.unpack_from(order + "I", data, base)[0]
    header_size = 32 if magic == MH_MAGIC_64 else 28
    if end - base < header_size:
        raise MachOError("truncated Mach-O header")
    ncmds, sizeofcmds = struct.unpack_from(order + "II", data, base + 16)
    if base + header_size + sizeofcmds > end:
        raise MachOError("load commands extend past the end of the image")

    found: set[Version] = set()
    offset = base + header_size
    for _ in range(ncmds):
        if offset + 8 > end:
            raise MachOError("truncated load command")
        cmd, cmdsize = struct.unpack_from(order + "II", data, offset)
        if cmdsize < 8 or offset + cmdsize > end:
            raise MachOError(f"malformed load command size {cmdsize}")
        if cmd == LC_BUILD_VERSION:
            if cmdsize < 24:
                raise MachOError("truncated LC_BUILD_VERSION")
            platform, minos = struct.unpack_from(order + "II", data, offset + 8)
            if platform != PLATFORM_MACOS:
                raise MachOError(f"LC_BUILD_VERSION targets platform {platform}, not macOS")
            found.add(_decode_packed_version(minos))
        elif cmd == LC_VERSION_MIN_MACOSX:
            if cmdsize < 16:
                raise MachOError("truncated LC_VERSION_MIN_MACOSX")
            found.add(_decode_packed_version(struct.unpack_from(order + "I", data, offset + 8)[0]))
        offset += cmdsize

    if not found:
        raise MachOError("image has no LC_BUILD_VERSION or LC_VERSION_MIN_MACOSX load command")
    if len(found) != 1:
        raise MachOError("image records conflicting minimum versions: " + ", ".join(map(format_version, sorted(found))))
    return found.pop()


def macho_min_version_bytes(data: bytes) -> Version:
    """Return the single minimum macOS version recorded in a thin or fat image."""
    if len(data) < 8:
        raise MachOError("truncated image")
    magic = struct.unpack_from(">I", data, 0)[0]
    if magic not in (FAT_MAGIC, FAT_MAGIC_64):
        return _thin_min_version(data, 0, len(data))

    nfat = struct.unpack_from(">I", data, 4)[0]
    if nfat == 0 or nfat > _MAX_FAT_ARCHS:
        raise MachOError(f"implausible universal slice count {nfat}")
    entry = "IIQQII" if magic == FAT_MAGIC_64 else "IIIII"
    entry_size = struct.calcsize(">" + entry)
    if 8 + nfat * entry_size > len(data):
        raise MachOError("truncated universal header")
    versions: set[Version] = set()
    for index in range(nfat):
        fields = struct.unpack_from(">" + entry, data, 8 + index * entry_size)
        offset, size = fields[2], fields[3]
        if size == 0 or offset + size > len(data):
            raise MachOError(f"universal slice {index} lies outside the file")
        versions.add(_thin_min_version(data, offset, offset + size))
    if len(versions) != 1:
        raise MachOError(
            "universal slices record different minimum versions: " + ", ".join(map(format_version, sorted(versions)))
        )
    return versions.pop()


def macho_min_version(path: Path) -> Version:
    return macho_min_version_bytes(path.read_bytes())


def parity_errors(repo: Path, binaries: Iterable[Path] = ()) -> list[str]:
    """Return every disagreement with the canonical macOS minimum version."""
    try:
        expected = canonical(repo)
    except (OSError, ValueError) as exc:
        return [str(exc)]
    shown = format_version(expected)
    errors: list[str] = []

    ci = ci_values(repo)
    if not ci:
        errors.append("no CMAKE_OSX_DEPLOYMENT_TARGET value found in workflows or presets")
    docs, doc_errors = doc_values(repo)
    errors.extend(doc_errors)
    for location, raw in ci + docs:
        try:
            value = parse_version(raw)
        except ValueError as exc:
            errors.append(f"{location}: {exc}")
            continue
        if value != expected:
            errors.append(f"{location}: macOS minimum {raw} != SPARK_MACOS_MIN_VERSION {shown}")

    runners = ci_runners(repo)
    if len(runners) != 1:
        errors.append(f"workflows must use exactly one macOS runner label, found {sorted(runners)}")
    runner_docs, runner_errors = doc_runners(repo)
    errors.extend(runner_errors)
    for location, runner in runner_docs:
        if runner not in runners:
            errors.append(f"{location}: documents runner {runner} but workflows use {sorted(runners)}")

    for binary in binaries:
        try:
            value = macho_min_version(binary)
        except (OSError, MachOError) as exc:
            errors.append(f"{binary}: {exc}")
            continue
        if value != expected:
            errors.append(f"{binary}: Mach-O minimum {format_version(value)} != SPARK_MACOS_MIN_VERSION {shown}")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--binary", type=Path, nargs="+", default=[], help="Mach-O images to check")
    args = parser.parse_args(argv)

    errors = parity_errors(args.repo_root.resolve(), args.binary)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print(
        f"macOS minimum {format_version(canonical(args.repo_root.resolve()))} consistent across CMake, CI, "
        f"docs and {len(args.binary)} binary image(s)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
