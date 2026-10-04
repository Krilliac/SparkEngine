#!/usr/bin/env python3
"""Retain bounded COFF diagnostics alongside the unchanged BLD-100 comparison.

The acceptance report and exit status come from compare_build_outputs. The
separate diagnostics inspect one differing ordinary archive member per library;
they never authorize additional normalization or infer a cause from a hash.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import compare_build_outputs as comparator  # noqa: E402

MAX_LIBRARIES = 64
MAX_MEMBER_BYTES = 16 * 1024 * 1024
MAX_CHANGED_REGIONS = 32
MAX_DIAGNOSTIC_BYTES = 1024 * 1024
DIAGNOSTIC_ERRORS = (OSError, ValueError, comparator.InputError, comparator.FormatError)


def _hash(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _error_text(error: Exception) -> str:
    # Keep optional diagnostics bounded without recording native absolute paths
    # from filesystem exceptions.
    return (error.strerror if isinstance(error, OSError) and error.strerror else str(error))[:512]


def _member(path: Path, index: int, expected: dict, build_root: bytes) -> tuple[bytes, bytes] | None:
    """Read only the selected member, checking its manifest-bound digest."""
    if path.is_symlink() or not path.is_file():
        raise comparator.InputError("diagnostic library is not a regular file")
    with path.open("rb") as handle:
        handle.seek(0, os.SEEK_END)
        file_size = handle.tell()
        if comparator._read(handle, 0, 8, file_size) != comparator.AR_MAGIC:
            raise comparator.InputError("diagnostic library is not an archive")
        offset = 8
        for ordinal in range(index + 1):
            header = comparator._read(handle, offset, 60, file_size)
            if header[58:60] != b"`\n":
                raise comparator.InputError("invalid diagnostic archive member header")
            try:
                size = int(header[48:58].decode("ascii").strip())
            except ValueError as error:
                raise comparator.InputError("invalid diagnostic archive member size") from error
            if size < 0 or size > file_size - offset - 60:
                raise comparator.InputError("diagnostic archive member exceeds its file")
            if ordinal == index:
                if size != expected["size"]:
                    raise comparator.InputError("diagnostic member size differs from its manifest")
                if size > MAX_MEMBER_BYTES:
                    return None
                data = comparator._read(handle, offset + 60, size, file_size)
                normalized, _ = comparator._replace_build_root(data, build_root)
                if _hash(header + normalized) != expected["sha256"]:
                    raise comparator.InputError("diagnostic member digest differs from its manifest")
                return header, data
            offset += 60 + size + (size & 1)
    raise comparator.InputError("diagnostic archive member is missing")


def _regions(data: bytes) -> list[tuple[str, bytes]]:
    """Partition COFF bytes into sections, headers and all remaining bytes."""
    bigobj = data[:4] == b"\x00\x00\xff\xff"
    if bigobj and len(data) >= 6 and struct.unpack_from("<H", data, 4)[0] == 0:
        raise comparator.InputError("diagnostic member is a short import object, not COFF/BigObj")
    header_size = 56 if bigobj else 20
    if len(data) < header_size:
        raise comparator.InputError("truncated diagnostic COFF header")
    if bigobj:
        if data[12:28] != bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8"):
            raise comparator.InputError("unsupported diagnostic BigObj identity")
        count = struct.unpack_from("<I", data, 44)[0]
    else:
        count = struct.unpack_from("<H", data, 2)[0]
        if struct.unpack_from("<H", data, 16)[0] != 0:
            raise comparator.InputError("diagnostic member is not a COFF object")
    table_end = header_size + count * 40
    if not 0 < count <= 65535 or table_end > len(data):
        raise comparator.InputError("invalid diagnostic COFF section table")
    regions = [("<coff-header>", data[:header_size]),
               ("<section-table>", data[header_size:table_end])]
    covered = [(0, table_end)]
    for index in range(count):
        section = header_size + index * 40
        size, offset = struct.unpack_from("<II", data, section + 16)
        flags = struct.unpack_from("<I", data, section + 36)[0]
        name = data[section:section + 8].rstrip(b"\0").decode("ascii", "replace")
        if not size or (offset == 0 and flags & 0x80):
            continue
        if offset < table_end or offset > len(data) or size > len(data) - offset:
            raise comparator.InputError("diagnostic COFF section exceeds its member")
        regions.append((f"{name} (#{index})", data[offset:offset + size]))
        covered.append((offset, offset + size))
    covered.sort()
    if any(start < previous_end for (start, _), (_, previous_end) in zip(covered[1:], covered)):
        raise comparator.InputError("overlapping diagnostic COFF sections")
    outside = bytearray()
    cursor = 0
    for start, end in covered:
        outside.extend(data[cursor:start])
        cursor = end
    outside.extend(data[cursor:])
    # Includes relocations, symbols, string tables and any padding: no byte is
    # discarded merely because it is outside a section's raw-data range.
    regions.append(("<outside-sections>", bytes(outside)))
    return regions


def _describe_region(name: str, raw: bytes, normalized: bytes, root: bytes) -> dict:
    return {"name": name, "size": len(raw), "rawSha256": _hash(raw),
            "normalizedSha256": _hash(normalized), "buildRootOccurrences": raw.count(root)}


def _diagnose_member(first: tuple[bytes, bytes], second: tuple[bytes, bytes],
                     first_root: bytes, second_root: bytes) -> dict:
    descriptions = []
    for (header, data), root in ((first, first_root), (second, second_root)):
        normalized, _ = comparator._replace_build_root(data, root)
        raw_regions, normalized_regions = _regions(data), _regions(normalized)
        descriptions.append([
            _describe_region("<archive-member-header>", header, header, root),
            *[_describe_region(name, raw, adjusted, root)
              for (name, raw), (_, adjusted) in zip(raw_regions, normalized_regions)],
        ])
    left, right = descriptions
    changed = []
    for index in range(max(len(left), len(right))):
        a = left[index] if index < len(left) else None
        b = right[index] if index < len(right) else None
        if a is None or b is None or any(a[key] != b[key] for key in ("name", "size", "normalizedSha256")):
            changed.append({"first": a, "second": b})
    return {"status": "inspected", "differingRegionCount": len(changed),
            "differingRegions": changed[:MAX_CHANGED_REGIONS],
            "omittedDifferingRegions": max(0, len(changed) - MAX_CHANGED_REGIONS)}


def diagnostics(first_manifest: dict, second_manifest: dict, first_root: Path, second_root: Path,
                first_build_root: str, second_build_root: str, report: dict) -> dict:
    entries = [{entry["path"]: entry for entry in manifest["entries"]}
               for manifest in (first_manifest, second_manifest)]
    libraries = []
    eligible = [item for item in report["differing"]
                if item["path"].lower().endswith(".lib")
                and all(side[item["path"]]["kind"] == "ar" for side in entries)]
    for difference in eligible[:MAX_LIBRARIES]:
        path = difference["path"]
        a, b = (side[path] for side in entries)
        selected = next(((index, left, right) for index, (left, right) in enumerate(zip(a["sections"], b["sections"]))
                         if left["name"] == right["name"] and left["name"] not in ("<symbol-index>", "<long-names>")
                         and left != right), None)
        item = {"path": path, "status": "no-aligned-differing-member"}
        if selected is not None:
            index, left, right = selected
            item.update({"member": left["name"], "memberIndex": index})
            try:
                members = []
                for root, member, build_root in ((first_root, left, first_build_root),
                                                 (second_root, right, second_build_root)):
                    file = root / path
                    if not file.resolve().is_relative_to(root.resolve()):
                        raise comparator.InputError("diagnostic library escapes its install root")
                    members.append(_member(file, index, member, os.fsencode(build_root)))
                if any(member is None for member in members):
                    item["status"] = "member-exceeds-diagnostic-byte-limit"
                else:
                    item.update(_diagnose_member(members[0], members[1], os.fsencode(first_build_root),
                                                 os.fsencode(second_build_root)))
            except DIAGNOSTIC_ERRORS as error:
                item.update({"status": "inspection-unavailable", "error": _error_text(error)})
        libraries.append(item)
    return {"schema": "spark.windows-shipping-repro-diagnostics/1", "diagnosticOnly": True,
            "acceptanceEquivalent": report["equivalent"], "libraries": libraries,
            "omittedLibraries": max(0, len(eligible) - MAX_LIBRARIES),
            "limits": {"libraries": MAX_LIBRARIES, "memberBytes": MAX_MEMBER_BYTES,
                       "changedRegionsPerMember": MAX_CHANGED_REGIONS, "outputBytes": MAX_DIAGNOSTIC_BYTES}}


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("first-manifest", "second-manifest", "first-root", "second-root", "report", "diagnostics"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    for name in ("first-build-root", "second-build-root"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args(arguments)
    try:
        if not args.first_build_root or not args.second_build_root:
            raise comparator.InputError("diagnostic build roots must not be empty")
        first = comparator.load_manifest(args.first_manifest)
        second = comparator.load_manifest(args.second_manifest)
        report = comparator.compare_manifests(first, second)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        comparator.print_report(report, str(args.first_manifest), str(args.second_manifest))
    except DIAGNOSTIC_ERRORS as error:
        print(f"Windows Shipping reproducibility comparison: {_error_text(error)}", file=sys.stderr)
        return 2

    # The manifests and comparison above are authoritative. Unsupported members,
    # unavailable native files or a diagnostic write failure cannot replace a
    # valid equivalent/differing comparison with an input-error result.
    try:
        detail = diagnostics(first, second, args.first_root, args.second_root,
                             args.first_build_root, args.second_build_root, report)
    except DIAGNOSTIC_ERRORS as error:
        detail = {"schema": "spark.windows-shipping-repro-diagnostics/1", "diagnosticOnly": True,
                  "acceptanceEquivalent": report["equivalent"], "status": "inspection-unavailable",
                  "error": _error_text(error)}
    try:
        payload = (json.dumps(detail, indent=2, sort_keys=True) + "\n").encode("utf-8")
        if len(payload) > MAX_DIAGNOSTIC_BYTES:
            detail = {"schema": "spark.windows-shipping-repro-diagnostics/1", "diagnosticOnly": True,
                      "acceptanceEquivalent": report["equivalent"], "status": "inspection-unavailable",
                      "error": "diagnostic output exceeds its byte limit"}
            payload = (json.dumps(detail, indent=2, sort_keys=True) + "\n").encode("utf-8")
            if len(payload) > MAX_DIAGNOSTIC_BYTES:
                raise comparator.InputError("diagnostic byte limit cannot retain an unavailable result")
        args.diagnostics.parent.mkdir(parents=True, exist_ok=True)
        args.diagnostics.write_bytes(payload)
    except DIAGNOSTIC_ERRORS as error:
        print(f"Windows Shipping reproducibility diagnostics unavailable: {_error_text(error)}", file=sys.stderr)
    return 0 if report["equivalent"] else 1


if __name__ == "__main__":
    sys.exit(main())
