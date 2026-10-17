#!/usr/bin/env python3
"""Generate the deterministic seed corpus for SparkBuild's in-process ZIP listing.

SparkBuild::ArchiveExtraction::ListZipMembers reads a downloaded archive's
end-of-central-directory record (and its ZIP64 locator and record), the
central directory, and every entry's local header name. The seeds below are
hand-built stored (uncompressed) archives in APPNOTE 6.3.x layout:

  local header   30 bytes + name + data
  central header 46 bytes + name + extra + comment
  ZIP64 record   56 bytes, ZIP64 locator 20 bytes (only in zip64-valid.zip)
  end record     22 bytes + comment

Every timestamp is zero and every CRC is computed, so the output is
byte-identical on every host. The committed bytes are pinned by
tools/fuzz-policy/corpus-manifest.json; this script records how each seed was
built so a reviewer can regenerate and diff.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "sparkbuild-archive-download"

LOCAL_SIGNATURE = 0x04034B50
CENTRAL_SIGNATURE = 0x02014B50
END_SIGNATURE = 0x06054B50
ZIP64_END_SIGNATURE = 0x06064B50
ZIP64_LOCATOR_SIGNATURE = 0x07064B50
ZIP64_EXTRA_ID = 0x0001
MAX_COMMENT_BYTES = 0xFFFF


def local_header(name: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(data) & 0xFFFFFFFF
    return struct.pack(
        "<IHHHHHIIIHH", LOCAL_SIGNATURE, 20, 0, 0, 0, 0, crc, len(data), len(data), len(name), 0
    ) + name + data


def central_header(name: bytes, data: bytes, local_offset: int, extra: bytes = b"") -> bytes:
    crc = zlib.crc32(data) & 0xFFFFFFFF
    return struct.pack(
        "<IHHHHHHIIIHHHHHII",
        CENTRAL_SIGNATURE,
        20,
        20,
        0,
        0,
        0,
        0,
        crc,
        len(data),
        len(data),
        len(name),
        len(extra),
        0,
        0,
        0,
        0,
        local_offset,
    ) + name + extra


def end_record(entries: int, directory_size: int, directory_offset: int, comment: bytes = b"") -> bytes:
    return struct.pack(
        "<IHHHHIIH", END_SIGNATURE, 0, 0, entries, entries, directory_size, directory_offset, len(comment)
    ) + comment


def archive(
    members: list[tuple[bytes, bytes]],
    *,
    local_names: list[bytes] | None = None,
    claimed_entries: int | None = None,
    directory_offset_delta: int = 0,
    comment: bytes = b"",
) -> bytes:
    """A single-volume stored archive; the keyword arguments inject one defect each."""
    body = b""
    offsets = []
    for index, (name, data) in enumerate(members):
        offsets.append(len(body))
        stored_name = local_names[index] if local_names else name
        body += local_header(stored_name, data)
    directory = b"".join(central_header(name, data, offset) for (name, data), offset in zip(members, offsets))
    entries = len(members) if claimed_entries is None else claimed_entries
    return body + directory + end_record(entries, len(directory), len(body) + directory_offset_delta, comment)


def zip64_archive(members: list[tuple[bytes, bytes]]) -> bytes:
    """Every local offset lives in a ZIP64 extra field and the directory is located via ZIP64 records."""
    body = b""
    offsets = []
    for name, data in members:
        offsets.append(len(body))
        body += local_header(name, data)
    directory = b"".join(
        central_header(name, data, 0xFFFFFFFF, struct.pack("<HHQ", ZIP64_EXTRA_ID, 8, offset))
        for (name, data), offset in zip(members, offsets)
    )
    zip64_record_offset = len(body) + len(directory)
    zip64_record = struct.pack(
        "<IQHHIIQQQQ",
        ZIP64_END_SIGNATURE,
        44,
        45,
        45,
        0,
        0,
        len(members),
        len(members),
        len(directory),
        len(body),
    )
    locator = struct.pack("<IIQI", ZIP64_LOCATOR_SIGNATURE, 0, zip64_record_offset, 1)
    end = struct.pack("<IHHHHIIH", END_SIGNATURE, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0)
    return body + directory + zip64_record + locator + end


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    two_members = [(b"pkg/bin/a.txt", b"alpha\n"), (b"pkg/b.txt", b"bravo\n")]

    # The maximum comment, with a decoy end record planted inside it: the
    # reader must pick the record whose comment length runs exactly to EOF.
    decoy = end_record(1, 46 + 9, 0)
    comment = (decoy + b"C" * MAX_COMMENT_BYTES)[:MAX_COMMENT_BYTES]

    seeds = {
        "valid-2-members.zip": archive(two_members),
        "zip64-valid.zip": zip64_archive([(b"z64/a.txt", b"one\n"), (b"z64/b.txt", b"two\n")]),
        "traversal-dotdot.zip": archive([(b"../evil.txt", b"pwned")]),
        "backslash-name.zip": archive([(b"dir\\..\\..\\evil.txt", b"pwned")]),
        "drive-colon-name.zip": archive([(b"C:evil.txt", b"pwned")]),
        # Same length, different bytes: extractors that trust the local copy
        # would write '../evil.txt!!' while the central copy looks safe.
        "cd-local-name-mismatch.zip": archive([(b"safe/name.txt", b"pwned")], local_names=[b"../evil.txt!!"]),
        "entry-count-overclaim.zip": archive(two_members, claimed_entries=0xFFFE),
        "cd-offset-past-eof.zip": archive(two_members, directory_offset_delta=0x10000),
        "comment-65535.zip": archive([(b"readme.txt", b"hello\n")], comment=comment),
    }

    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in seeds.items():
        (args.output / name).write_bytes(payload)
    total = sum(len(payload) for payload in seeds.values())
    print(f"generated {len(seeds)} ZIP seeds ({total} bytes) in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
