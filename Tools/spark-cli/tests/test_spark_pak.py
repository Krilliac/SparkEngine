"""Focused security and behavior tests for read-only SparkPak tooling."""

import contextlib
import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from types import SimpleNamespace


CLI_PATH = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CLI_PATH))

import spark_cli  # noqa: E402
from spark_pak import (  # noqa: E402
    ENTRY,
    HEADER,
    MAX_ENTRY_COUNT,
    SPARK_PAK_MAGIC,
    SparkPakArchive,
    fnv1a64,
)


def make_archive(entries, compress_toc=False):
    """Return a valid SparkPak byte stream from (path, data, compression) tuples."""
    encoded_entries = []
    data_offset = HEADER.size
    for path, original, compression in entries:
        encoded = zlib.compress(original) if compression == 1 else original
        encoded_entries.append((path, original, compression, encoded, data_offset))
        data_offset += len(encoded)

    toc = bytearray()
    for path, original, compression, encoded, offset in encoded_entries:
        path_bytes = path.encode("utf-8")
        toc.extend(
            ENTRY.pack(
                fnv1a64(path_bytes), offset, len(encoded), len(original), compression, len(path_bytes)
            )
        )
        toc.extend(path_bytes)
    encoded_toc = zlib.compress(toc) if compress_toc else bytes(toc)
    header = HEADER.pack(
        SPARK_PAK_MAGIC, 1, len(entries), 0, data_offset, len(encoded_toc), len(toc)
    )
    return header + b"".join(item[3] for item in encoded_entries) + encoded_toc


class SparkPakCliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, name, entries, compress_toc=False):
        path = self.root / name
        path.write_bytes(make_archive(entries, compress_toc))
        return path

    def run_command(self, **values):
        defaults = {
            "pak_command": "list",
            "archive": None,
            "left": None,
            "right": None,
            "format": "text",
        }
        defaults.update(values)
        output = io.StringIO()
        errors = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            result = spark_cli.cmd_pak(SimpleNamespace(**defaults))
        return result, output.getvalue(), errors.getvalue()

    def test_list_and_inspect_are_deterministic(self):
        archive = self.write(
            "content.spk",
            [("z/data.bin", b"z", 0), ("a/data.bin", b"a" * 64, 1)],
            compress_toc=True,
        )
        result, output, errors = self.run_command(archive=archive)
        self.assertEqual((result, errors), (0, ""))
        self.assertEqual(output.splitlines(), ["a/data.bin", "z/data.bin"])

        result, output, errors = self.run_command(
            pak_command="inspect", archive=archive, format="json"
        )
        self.assertEqual((result, errors), (0, ""))
        metadata = json.loads(output)
        self.assertEqual(metadata["fileCount"], 2)
        self.assertEqual([entry["path"] for entry in metadata["entries"]], ["a/data.bin", "z/data.bin"])

    def test_verify_hashes_decompressed_content(self):
        archive = self.write("content.spk", [("data.bin", b"repeat" * 100, 1)])
        result, output, errors = self.run_command(
            pak_command="verify", archive=archive, format="json"
        )
        self.assertEqual((result, errors), (0, ""))
        report = json.loads(output)
        self.assertTrue(report["verified"])
        self.assertEqual(report["files"][0]["sha256"], hashlib.sha256(b"repeat" * 100).hexdigest())

    def test_diff_reports_content_changes_with_stable_exit_code(self):
        left = self.write("left.spk", [("same", b"1", 0), ("gone", b"2", 0)])
        right = self.write("right.spk", [("same", b"changed", 0), ("new", b"3", 0)])
        result, output, errors = self.run_command(pak_command="diff", left=left, right=right)
        self.assertEqual((result, errors), (1, ""))
        self.assertEqual(
            json.loads(output),
            {"added": ["new"], "changed": ["same"], "removed": ["gone"], "unchanged": []},
        )

        identical = self.write("identical.spk", [("same", b"1", 0), ("gone", b"2", 0)])
        result, output, errors = self.run_command(pak_command="diff", left=left, right=identical)
        self.assertEqual((result, errors), (0, ""))
        self.assertEqual(json.loads(output)["unchanged"], ["gone", "same"])

    def test_rejects_traversal_hash_mismatch_and_overlaps(self):
        traversal = self.write("traversal.spk", [("../escape", b"x", 0)])
        with self.assertRaisesRegex(Exception, "unsafe virtual path"):
            SparkPakArchive(traversal)

        valid = bytearray(make_archive([("safe", b"x", 0)]))
        toc_offset = HEADER.unpack_from(valid)[4]
        struct.pack_into("<Q", valid, toc_offset, 0)
        mismatch = self.root / "mismatch.spk"
        mismatch.write_bytes(valid)
        with self.assertRaisesRegex(Exception, "path hash does not match"):
            SparkPakArchive(mismatch)

        overlap = bytearray(make_archive([("one", b"abc", 0), ("two", b"def", 0)]))
        toc_offset = HEADER.unpack_from(overlap)[4]
        second_record = toc_offset + ENTRY.size + len("one")
        struct.pack_into("<Q", overlap, second_record + 8, HEADER.size + 1)
        overlapping = self.root / "overlap.spk"
        overlapping.write_bytes(overlap)
        with self.assertRaisesRegex(Exception, "entries overlap"):
            SparkPakArchive(overlapping)

    def test_rejects_trailing_toc_and_unknown_compression(self):
        valid = make_archive([("safe", b"x", 0)])
        trailing = bytearray(valid + b"x")
        fields = list(HEADER.unpack_from(trailing))
        fields[5] += 1
        fields[6] += 1
        HEADER.pack_into(trailing, 0, *fields)
        trailing_path = self.root / "trailing.spk"
        trailing_path.write_bytes(trailing)
        with self.assertRaisesRegex(Exception, "trailing bytes"):
            SparkPakArchive(trailing_path)

        unknown = bytearray(make_archive([("safe", b"x", 0)]))
        toc_offset = HEADER.unpack_from(unknown)[4]
        unknown[toc_offset + 24] = 99
        unknown_path = self.root / "unknown.spk"
        unknown_path.write_bytes(unknown)
        with self.assertRaisesRegex(Exception, "unknown compression"):
            SparkPakArchive(unknown_path)

    def test_rejects_hostile_entry_count_before_metadata_allocation(self):
        archive = self.root / "entry-count.spk"
        archive.write_bytes(
            HEADER.pack(
                SPARK_PAK_MAGIC,
                1,
                MAX_ENTRY_COUNT + 1,
                0,
                HEADER.size,
                1,
                1,
            )
            + b"x"
        )
        with self.assertRaisesRegex(Exception, "entry count exceeds"):
            SparkPakArchive(archive)

        impossible = self.root / "impossible-entry-count.spk"
        impossible.write_bytes(
            HEADER.pack(SPARK_PAK_MAGIC, 1, 2, 0, HEADER.size, ENTRY.size, ENTRY.size)
            + bytes(ENTRY.size)
        )
        with self.assertRaisesRegex(Exception, "cannot fit"):
            SparkPakArchive(impossible)

    def test_rejects_aliased_paths_and_toc_expansion_beyond_declared_size(self):
        for index, unsafe in enumerate(("a//b", "a/./b", "a/../b")):
            path = self.write(f"alias-{index}.spk", [(unsafe, b"x", 0)])
            with self.subTest(path=unsafe), self.assertRaisesRegex(Exception, "unsafe virtual path"):
                SparkPakArchive(path)

        compressed = bytearray(make_archive([("safe", b"x", 0)], compress_toc=True))
        fields = list(HEADER.unpack_from(compressed))
        fields[6] -= 1
        HEADER.pack_into(compressed, 0, *fields)
        bomb = self.root / "toc-expansion.spk"
        bomb.write_bytes(compressed)
        with self.assertRaisesRegex(Exception, "expands beyond its declared size"):
            SparkPakArchive(bomb)

    def test_rejects_link_input_when_supported(self):
        archive = self.write("real.spk", [("safe", b"x", 0)])
        link = self.root / "linked.spk"
        try:
            link.symlink_to(archive)
        except OSError as exc:
            self.skipTest(f"file symlinks are unavailable: {exc}")
        result, _output, errors = self.run_command(archive=link)
        self.assertEqual(result, 5)
        self.assertIn("link or reparse point", errors)

    def test_zstd_verify_fails_explicitly_without_writing(self):
        archive = self.write("zstd.spk", [("content", b"not-zstd-needed-for-metadata", 2)])
        before = archive.read_bytes()
        result, _output, errors = self.run_command(pak_command="verify", archive=archive)
        self.assertEqual(result, 4)
        self.assertIn("zstd", errors)
        self.assertEqual(archive.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
