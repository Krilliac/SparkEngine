#!/usr/bin/env python3
"""Generate the small deterministic seed corpus for the production SparkPak (.spk) reader.

The layout mirrors SparkEngine/Source/Core/SparkPak.h: a 32-byte header
(magic, version, fileCount, reserved, tocOffset, tocSize, tocRawSize), the entry
data blobs, then the table of contents. A TOC is deflated (zlib, as
SparkPakWriter's mz_compress writes it) unless tocSize == tocRawSize. Each TOC
entry is pathHash:u64, dataOffset:u64, compressedSize:u32, originalSize:u32,
compression:u8, pathLength:u16, then the path bytes.

The committed bytes are pinned by tools/fuzz-policy/corpus-manifest.json; this
script records how each seed was built so a reviewer can regenerate and diff.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "sparkpak-reader"

MAGIC = 0x314B5053  # "SPK1"
VERSION = 1
HEADER_BYTES = 32
STORED, DEFLATE, ZSTD = 0, 1, 2
ZSTD_STUB_MAGIC = 0x5A535442  # "ZSTB", ThirdParty/Utils/zstd/zstd.h
MAX_TOC_BYTES = 256 * 1024 * 1024
MAX_DECOMPRESSED_ENTRY_BYTES = 256 * 1024 * 1024


def fnv1a(path: str) -> int:
    value = 14695981039346656037
    for byte in path.encode("utf-8"):
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def header(file_count: int, toc_offset: int, toc_size: int, toc_raw_size: int) -> bytes:
    return struct.pack("<IIIIQII", MAGIC, VERSION, file_count, 0, toc_offset, toc_size, toc_raw_size)


def toc_entry(path: str, offset: int, compressed: int, original: int, compression: int, path_hash: int | None = None) -> bytes:
    encoded = path.encode("utf-8")
    hashed = fnv1a(path) if path_hash is None else path_hash
    return struct.pack("<QQIIBH", hashed, offset, compressed, original, compression, len(encoded)) + encoded


def archive(
    blobs: list[bytes],
    entries: list[tuple[str, int, int, int, int, int | None]],
    *,
    deflate_toc: bool,
    file_count: int | None = None,
    toc_raw_size: int | None = None,
) -> bytes:
    """entries: (path, blob index, compressed size, original size, compression, hash override)."""
    offsets = []
    cursor = HEADER_BYTES
    for blob in blobs:
        offsets.append(cursor)
        cursor += len(blob)
    raw = b"".join(
        toc_entry(path, offsets[blob], compressed, original, compression, path_hash)
        for path, blob, compressed, original, compression, path_hash in entries
    )
    toc = zlib.compress(raw, 9) if deflate_toc else raw
    count = len(entries) if file_count is None else file_count
    declared_raw = len(raw) if toc_raw_size is None else toc_raw_size
    return header(count, cursor, len(toc), declared_raw) + b"".join(blobs) + toc


def zstd_stub(payload: bytes) -> bytes:
    return struct.pack("<II", ZSTD_STUB_MAGIC, len(payload)) + payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    readme = b"SparkPak fuzz seed: stored entry.\n"
    config = b"texture.quality = high\n" * 16
    config_deflated = zlib.compress(config, 9)
    mesh = bytes(range(64))
    mesh_zstd = zstd_stub(mesh)

    # Every codec the reader ships, with a deflated TOC.
    valid = archive(
        [readme, config_deflated, mesh_zstd],
        [
            ("docs/readme.txt", 0, len(readme), len(readme), STORED, None),
            ("config/render.ini", 1, len(config_deflated), len(config), DEFLATE, None),
            ("meshes/cube.bin", 2, len(mesh_zstd), len(mesh), ZSTD, None),
        ],
        deflate_toc=True,
    )

    # Uncompressed TOC (tocSize == tocRawSize), plus an empty entry.
    valid_stored_toc = archive(
        [readme, b""],
        [
            ("docs/readme.txt", 0, len(readme), len(readme), STORED, None),
            ("docs/empty.txt", 1, 0, 0, STORED, None),
        ],
        deflate_toc=False,
    )

    # fileCount claims far more entries than the TOC bytes hold.
    toc_count_overflow = archive(
        [readme],
        [("docs/readme.txt", 0, len(readme), len(readme), STORED, None)],
        deflate_toc=False,
        file_count=9_999_999,
    )

    # Per-entry decompression bombs: a 100-byte deflate blob that claims 2 GiB - 1,
    # one just over the 256 MB budget, beside an ordinary entry that must stay
    # readable (the budget refuses the entry, not the archive).
    bomb_payload = zlib.compress(b"\0" * 4096, 9).ljust(100, b"\0")
    entry_ratio_bomb = archive(
        [bomb_payload, readme],
        [
            ("bombs/huge.bin", 0, len(bomb_payload), 0x7FFFFFFF, DEFLATE, None),
            ("bombs/over-budget.bin", 0, len(bomb_payload), MAX_DECOMPRESSED_ENTRY_BYTES + 1, ZSTD, None),
            ("docs/readme.txt", 1, len(readme), len(readme), STORED, None),
        ],
        deflate_toc=False,
    )

    # Minimized regression: a deflated one-entry TOC whose header declares the
    # 256 MB TOC ceiling. The reader used to allocate (and zero) that buffer and
    # then mount the archive from the short stream.
    regression_toc_ratio_bomb = archive(
        [readme],
        [("docs/readme.txt", 0, len(readme), len(readme), STORED, None)],
        deflate_toc=True,
        toc_raw_size=MAX_TOC_BYTES,
    )

    # Minimized regression found by this target: a deflate entry whose declared
    # output is zero bytes. ReadFile handed miniz a null destination (the data()
    # of an empty vector), and tinfl applied an offset to it (UBSan).
    empty_output = zlib.compress(b"", 9)
    regression_deflate_empty_output = archive(
        [empty_output],
        [("docs/empty.txt", 0, len(empty_output), 0, DEFLATE, None)],
        deflate_toc=False,
    )

    # SparkPak_ProductionRejectsUnsafeTOCEntryPath's traversal name, and the
    # Windows-separator form a POSIX-only normalizer would miss.
    traversal = archive(
        [b"\x5a"],
        [("../outside.bin", 0, 1, 1, STORED, None)],
        deflate_toc=False,
    )
    traversal_backslash = archive(
        [b"\x5a"],
        [("assets\\..\\..\\outside.bin", 0, 1, 1, STORED, None)],
        deflate_toc=False,
    )

    # Hostile hashes: two entries share a stored hash, and a third's stored hash
    # names a different path, so lookups by path resolve to other entries.
    hash_collision = archive(
        [readme, mesh],
        [
            ("a/first.bin", 0, len(readme), len(readme), STORED, fnv1a("a/first.bin")),
            ("a/second.bin", 1, len(mesh), len(mesh), STORED, fnv1a("a/first.bin")),
            ("a/third.bin", 1, len(mesh), len(mesh), STORED, fnv1a("a/second.bin")),
        ],
        deflate_toc=False,
    )

    seeds = {
        "valid-mixed-codecs.spk": valid,
        "valid-stored-toc.spk": valid_stored_toc,
        "truncated-header.spk": valid[:20],
        "truncated-toc.spk": valid[:-3],
        "toc-count-overflow.spk": toc_count_overflow,
        "entry-ratio-bomb.spk": entry_ratio_bomb,
        "regression-toc-ratio-bomb.spk": regression_toc_ratio_bomb,
        "regression-deflate-empty-output.spk": regression_deflate_empty_output,
        "traversal-name.spk": traversal,
        "traversal-backslash.spk": traversal_backslash,
        "hash-collision.spk": hash_collision,
    }

    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in seeds.items():
        (args.output / name).write_bytes(payload)
    total = sum(len(payload) for payload in seeds.values())
    print(f"generated {len(seeds)} SparkPak seeds ({total} bytes) in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
