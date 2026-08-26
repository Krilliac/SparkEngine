"""Strictly read-only SparkPak inspection helpers for spark-cli."""

from __future__ import annotations

import hashlib
import json
import os
import stat
import struct
import zlib
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath


SPARK_PAK_MAGIC = 0x314B5053
SPARK_PAK_VERSION = 1
HEADER = struct.Struct("<IIIIQII")
ENTRY = struct.Struct("<QQIIBH")
MAX_ARCHIVE_BYTES = 8 * 1024 * 1024 * 1024
MAX_TOC_BYTES = 256 * 1024 * 1024
MAX_ENTRY_BYTES = 2 * 1024 * 1024 * 1024
# Keep decoded metadata comfortably bounded. The previous ten-million-entry
# ceiling could turn a valid-size hostile TOC into several gigabytes of Python
# objects even though the raw TOC itself was capped.
MAX_ENTRY_COUNT = 250_000
READ_CHUNK_BYTES = 1024 * 1024
COMPRESSION_NAMES = {0: "stored", 1: "deflate", 2: "zstd"}


class SparkPakError(Exception):
    """A bounded archive error with a stable CLI exit code."""

    def __init__(self, message, exit_code=3):
        super().__init__(message)
        self.exit_code = exit_code


@dataclass(frozen=True)
class PakEntry:
    path: str
    path_hash: int
    data_offset: int
    compressed_size: int
    original_size: int
    compression: int


def fnv1a64(value):
    result = 14695981039346656037
    for byte in value:
        result ^= byte
        result = (result * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return result


def _is_link_like(path):
    try:
        metadata = path.lstat()
    except OSError as exc:
        raise SparkPakError(f"Cannot inspect archive '{path}': {exc}", 5) from exc
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    attributes = getattr(metadata, "st_file_attributes", 0)
    return stat.S_ISLNK(metadata.st_mode) or bool(reparse_flag and attributes & reparse_flag)


def _validate_virtual_path(path):
    if not path or "\\" in path or "\x00" in path:
        raise SparkPakError(f"Archive contains an invalid virtual path: {path!r}")
    if any(part in {"", ".", ".."} for part in path.split("/")):
        raise SparkPakError(f"Archive contains an unsafe virtual path: {path!r}")
    logical = PurePosixPath(path)
    if logical.is_absolute():
        raise SparkPakError(f"Archive contains an unsafe virtual path: {path!r}")
    if logical.parts and ":" in logical.parts[0]:
        raise SparkPakError(f"Archive contains an unsafe virtual path: {path!r}")


class SparkPakArchive:
    """Validated SparkPak metadata plus bounded streaming content verification."""

    def __init__(self, path):
        self.path = Path(path)
        self.file_size = 0
        self.version = 0
        self.toc_offset = 0
        self.toc_size = 0
        self.toc_raw_size = 0
        self.entries = []
        self._load()

    def _open(self):
        if _is_link_like(self.path):
            raise SparkPakError(f"Archive path is a link or reparse point: {self.path}", 5)
        try:
            handle = self.path.open("rb")
            metadata = os.fstat(handle.fileno())
        except OSError as exc:
            raise SparkPakError(f"Cannot open archive '{self.path}': {exc}", 5) from exc
        if not stat.S_ISREG(metadata.st_mode):
            handle.close()
            raise SparkPakError(f"Archive is not a regular file: {self.path}", 5)
        if metadata.st_size > MAX_ARCHIVE_BYTES:
            handle.close()
            raise SparkPakError(f"Archive exceeds the {MAX_ARCHIVE_BYTES}-byte inspection limit")
        return handle, metadata.st_size

    def _load(self):
        handle, self.file_size = self._open()
        with handle:
            header_bytes = handle.read(HEADER.size)
            if len(header_bytes) != HEADER.size:
                raise SparkPakError("Archive header is truncated")
            magic, self.version, count, _reserved, self.toc_offset, self.toc_size, self.toc_raw_size = (
                HEADER.unpack(header_bytes)
            )
            if magic != SPARK_PAK_MAGIC:
                raise SparkPakError("Archive magic is not SPK1")
            if self.version != SPARK_PAK_VERSION:
                raise SparkPakError(f"Unsupported SparkPak version: {self.version}")
            if count > MAX_ENTRY_COUNT:
                raise SparkPakError("Archive entry count exceeds the inspection limit")
            if not self.toc_size or not self.toc_raw_size:
                raise SparkPakError("Archive TOC is empty")
            if self.toc_size > MAX_TOC_BYTES or self.toc_raw_size > MAX_TOC_BYTES:
                raise SparkPakError("Archive TOC exceeds the inspection limit")
            if count > self.toc_raw_size // ENTRY.size:
                raise SparkPakError("Archive entry count cannot fit in its declared TOC")
            if self.toc_offset < HEADER.size or self.toc_offset >= self.file_size:
                raise SparkPakError("Archive TOC offset is outside the file")
            if self.toc_size > self.file_size - self.toc_offset:
                raise SparkPakError("Archive TOC is truncated")

            handle.seek(self.toc_offset)
            encoded_toc = handle.read(self.toc_size)
            if len(encoded_toc) != self.toc_size:
                raise SparkPakError("Archive TOC is truncated")
            if self.toc_size == self.toc_raw_size:
                toc = encoded_toc
            else:
                try:
                    inflater = zlib.decompressobj()
                    toc = bytearray()
                    for offset in range(0, len(encoded_toc), READ_CHUNK_BYTES):
                        remaining = self.toc_raw_size - len(toc)
                        toc.extend(
                            inflater.decompress(
                                encoded_toc[offset:offset + READ_CHUNK_BYTES], remaining + 1
                            )
                        )
                        if len(toc) > self.toc_raw_size or inflater.unconsumed_tail:
                            raise SparkPakError("Archive TOC expands beyond its declared size")
                    toc.extend(inflater.flush(self.toc_raw_size - len(toc) + 1))
                except zlib.error as exc:
                    raise SparkPakError(f"Archive TOC decompression failed: {exc}") from exc
                if inflater.unused_data or inflater.unconsumed_tail or not inflater.eof:
                    raise SparkPakError("Archive TOC compressed stream is malformed")
            if len(toc) != self.toc_raw_size:
                raise SparkPakError("Archive TOC size does not match its header")
            self.entries = self._parse_entries(toc, count)

    def _parse_entries(self, toc, count):
        cursor = 0
        entries = []
        paths = set()
        hashes = set()
        for _ in range(count):
            if len(toc) - cursor < ENTRY.size:
                raise SparkPakError("Archive TOC entry is truncated")
            values = ENTRY.unpack_from(toc, cursor)
            cursor += ENTRY.size
            path_hash, data_offset, compressed_size, original_size, compression, path_length = values
            if path_length > len(toc) - cursor:
                raise SparkPakError("Archive TOC path is truncated")
            path_bytes = toc[cursor:cursor + path_length]
            cursor += path_length
            try:
                path = path_bytes.decode("utf-8")
            except UnicodeDecodeError as exc:
                raise SparkPakError("Archive contains a non-UTF-8 virtual path") from exc
            _validate_virtual_path(path)
            if path_hash != fnv1a64(path_bytes):
                raise SparkPakError(f"Archive path hash does not match: {path}")
            if path in paths or path_hash in hashes:
                raise SparkPakError(f"Archive contains a duplicate path or path hash: {path}")
            if compression not in COMPRESSION_NAMES:
                raise SparkPakError(f"Archive entry uses an unknown compression method: {path}")
            if compressed_size > MAX_ENTRY_BYTES or original_size > MAX_ENTRY_BYTES:
                raise SparkPakError(f"Archive entry exceeds the inspection limit: {path}")
            if data_offset < HEADER.size or data_offset > self.toc_offset:
                raise SparkPakError(f"Archive entry offset is outside the data region: {path}")
            if compressed_size > self.toc_offset - data_offset:
                raise SparkPakError(f"Archive entry overlaps the TOC: {path}")
            if compression == 0 and compressed_size != original_size:
                raise SparkPakError(f"Stored archive entry has inconsistent sizes: {path}")
            paths.add(path)
            hashes.add(path_hash)
            entries.append(PakEntry(path, path_hash, data_offset, compressed_size, original_size, compression))
        if cursor != len(toc):
            raise SparkPakError("Archive TOC contains trailing bytes")
        # Reuse the entry list for interval validation rather than allocating a
        # second O(entry-count) list of tuples and path references.
        entries.sort(key=lambda entry: entry.data_offset)
        previous = None
        for current in entries:
            if not current.compressed_size:
                continue
            if previous and current.data_offset < previous.data_offset + previous.compressed_size:
                raise SparkPakError(f"Archive entries overlap: {previous.path} and {current.path}")
            previous = current
        return sorted(entries, key=lambda entry: entry.path)

    def metadata(self):
        return {
            "path": str(self.path),
            "fileSize": self.file_size,
            "version": self.version,
            "fileCount": len(self.entries),
            "tocOffset": self.toc_offset,
            "tocSize": self.toc_size,
            "tocRawSize": self.toc_raw_size,
            "entries": [
                {
                    **asdict(entry),
                    "path_hash": f"{entry.path_hash:016x}",
                    "compression": COMPRESSION_NAMES[entry.compression],
                }
                for entry in self.entries
            ],
        }

    def hashes(self):
        handle, current_size = self._open()
        if current_size != self.file_size:
            handle.close()
            raise SparkPakError("Archive changed while it was being inspected", 5)
        results = {}
        with handle:
            for entry in self.entries:
                results[entry.path] = self._hash_entry(handle, entry)
        return results

    @staticmethod
    def _hash_entry(handle, entry):
        if entry.compression == 2:
            raise SparkPakError(
                f"Cannot verify zstd entry without an optional zstd provider: {entry.path}", 4
            )
        handle.seek(entry.data_offset)
        remaining = entry.compressed_size
        digest = hashlib.sha256()
        output_size = 0
        inflater = zlib.decompressobj() if entry.compression == 1 else None
        while remaining:
            chunk = handle.read(min(remaining, READ_CHUNK_BYTES))
            if not chunk:
                raise SparkPakError(f"Archive entry data is truncated: {entry.path}")
            remaining -= len(chunk)
            if inflater:
                try:
                    output = inflater.decompress(chunk, entry.original_size - output_size + 1)
                except zlib.error as exc:
                    raise SparkPakError(f"Archive entry decompression failed: {entry.path}: {exc}") from exc
            else:
                output = chunk
            digest.update(output)
            output_size += len(output)
            if output_size > entry.original_size:
                raise SparkPakError(f"Archive entry expands beyond its declared size: {entry.path}")
        if inflater:
            try:
                output = inflater.flush(entry.original_size - output_size + 1)
            except zlib.error as exc:
                raise SparkPakError(f"Archive entry decompression failed: {entry.path}: {exc}") from exc
            digest.update(output)
            output_size += len(output)
            if not inflater.eof or inflater.unused_data or inflater.unconsumed_tail:
                raise SparkPakError(f"Archive entry compressed stream is malformed: {entry.path}")
        if output_size != entry.original_size:
            raise SparkPakError(f"Archive entry size does not match its TOC record: {entry.path}")
        return digest.hexdigest()


def inspect_text(archive):
    lines = [
        f"Archive: {archive.path}",
        f"Version: {archive.version}",
        f"Files: {len(archive.entries)}",
        f"TOC: offset={archive.toc_offset} stored={archive.toc_size} raw={archive.toc_raw_size}",
    ]
    for entry in archive.entries:
        lines.append(
            f"{entry.path}  {COMPRESSION_NAMES[entry.compression]}  "
            f"{entry.compressed_size}/{entry.original_size}  {entry.path_hash:016x}"
        )
    return "\n".join(lines)


def stable_json(value):
    return json.dumps(value, indent=2, sort_keys=True) + "\n"


def diff_archives(left, right):
    left_hashes = left.hashes()
    right_hashes = right.hashes()
    left_paths = set(left_hashes)
    right_paths = set(right_hashes)
    shared = left_paths & right_paths
    return {
        "added": sorted(right_paths - left_paths),
        "removed": sorted(left_paths - right_paths),
        "changed": sorted(path for path in shared if left_hashes[path] != right_hashes[path]),
        "unchanged": sorted(path for path in shared if left_hashes[path] == right_hashes[path]),
    }
