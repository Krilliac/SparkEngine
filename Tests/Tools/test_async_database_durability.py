#!/usr/bin/env python3
"""Source contract for durable AsyncDatabase KV revision publication."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "SparkEngine" / "Source" / "Engine" / "Persistence" / "AsyncDatabase.cpp"


class AsyncDatabaseDurabilityContractTests(unittest.TestCase):
    def test_temp_revision_is_durably_flushed_before_atomic_replace(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        start = source.index("bool SQLiteConnection::FlushToDisk()")
        end = source.index("void SQLiteConnection::LoadFromDisk()", start)
        flush = source[start:end]

        self.assertIn("FlushFileBuffers", flush)
        self.assertIn("fsync", flush)
        self.assertIn("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH", flush)
        self.assertLess(flush.index("FlushFileBuffers"), flush.index("MoveFileExW"))
        self.assertLess(flush.index("fsync"), flush.index("std::filesystem::rename"))


if __name__ == "__main__":
    unittest.main()
