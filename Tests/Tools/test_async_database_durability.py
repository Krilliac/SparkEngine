#!/usr/bin/env python3
"""Source contract for durable AsyncDatabase KV and TERRAFRONT store revision publication."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "SparkEngine" / "Source" / "Engine" / "Persistence" / "AsyncDatabase.cpp"
TF_SOURCE = ROOT / "GameModules" / "SparkGameMMOFPS" / "Source"
TF_SAVE_PATHS = TF_SOURCE / "Persistence" / "TFSavePaths.h"
# Every TERRAFRONT store writer and the SavePaths call it must commit through.
TF_WRITERS = {
    "Persistence/TFDatabase.cpp": "WriteDurableReplace(m_path,",
    "Persistence/TFOutfitStoreDisk.cpp": "WriteDurableReplace(m_path,",
    "Persistence/TFWorldSave.h": "WriteDurableReplace(path,",
    "Game/TFSocialSystemStore.cpp": "WriteDurableReplace(m_storePath,",
}
# Raw file-writing or renaming APIs a TERRAFRONT source could use to bypass the durable commit.
TF_BYPASS_TOKENS = ("std::ofstream", "fopen", "AtomicReplace", "filesystem::rename", "std::rename", "MoveFileEx")


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


class TerrafrontDurabilityContractTests(unittest.TestCase):
    @staticmethod
    def durable_replace_body() -> str:
        source = TF_SAVE_PATHS.read_text(encoding="utf-8")
        start = source.index("inline bool WriteDurableReplace(")
        end = source.index("inline std::filesystem::path LegacyExecutableFile(", start)
        return source[start:end]

    def test_windows_branch_flushes_staging_before_write_through_replace(self) -> None:
        body = self.durable_replace_body()
        windows = body[body.index("#ifdef _WIN32") : body.index("#else")]
        self.assertIn("CREATE_NEW", windows)
        self.assertIn("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH", windows)
        self.assertLess(windows.index("CREATE_NEW"), windows.index("FlushFileBuffers"))
        self.assertLess(windows.index("FlushFileBuffers"), windows.index("MoveFileExW(temporary"))

    def test_posix_branch_fsyncs_staging_then_directory_around_rename(self) -> None:
        body = self.durable_replace_body()
        posix = body[body.index("#else") : body.index("#endif")]
        create = posix.index("O_EXCL")
        self.assertIn("O_NOFOLLOW", posix[create - 120 : create + 120])
        file_sync = posix.index("::fsync(fd)")
        rename = posix.index("std::filesystem::rename(temporary, destination")
        directory_open = posix.index("O_DIRECTORY")
        directory_sync = posix.index("::fsync(dirFd)")
        self.assertLess(create, file_sync)
        self.assertLess(file_sync, rename)
        self.assertLess(rename, directory_open)
        self.assertLess(directory_open, directory_sync)
        # Once the rename lands the commit is visible, so it must never be reported as failed.
        self.assertNotIn("return false", posix[directory_open:])

    def test_every_store_writer_commits_through_durable_replace(self) -> None:
        for relative, call in TF_WRITERS.items():
            with self.subTest(writer=relative):
                self.assertIn(call, (TF_SOURCE / relative).read_text(encoding="utf-8"))

    def test_no_terrafront_source_bypasses_durable_replace(self) -> None:
        offenders = []
        for path in sorted(TF_SOURCE.rglob("*")):
            if path.suffix not in {".h", ".hpp", ".cpp"} or path == TF_SAVE_PATHS:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            offenders.extend(f"{path.relative_to(ROOT)}: {token}" for token in TF_BYPASS_TOKENS if token in text)
        self.assertEqual(offenders, [], "TERRAFRONT writers must use SavePaths::WriteDurableReplace")


if __name__ == "__main__":
    unittest.main()
