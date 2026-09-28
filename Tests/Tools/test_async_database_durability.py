#!/usr/bin/env python3
"""Source contract for durable AsyncDatabase KV and TERRAFRONT store revision publication."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "SparkEngine" / "Source" / "Engine" / "Persistence" / "AsyncDatabase.cpp"
DURABILITY_SOURCE = ROOT / "SparkEngine" / "Source" / "Utils" / "SaveFileDurability.cpp"
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
    @staticmethod
    def section(source: str, start_token: str, end_token: str) -> str:
        start = source.index(start_token)
        return source[start : source.index(end_token, start)]

    def test_flush_publishes_through_the_shared_durable_writer(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        flush = self.section(source, "bool SQLiteConnection::FlushToDisk()", "bool SQLiteConnection::LoadFromDisk()")
        self.assertIn("SaveFileDurability::WriteFileAtomically(", flush)
        # SEC2: the store must never be staged through a path-based, link-following open.
        for token in ("std::ofstream", "MoveFileEx", "filesystem::rename", '".tmp"'):
            self.assertNotIn(token, flush)

    def test_staging_is_exclusive_and_flushed_before_atomic_replace(self) -> None:
        durability = DURABILITY_SOURCE.read_text(encoding="utf-8")
        create = self.section(durability, "bool TryCreate(", "bool VerifyFreshRegularFile(")
        self.assertIn("CREATE_NEW", create)
        self.assertIn("FILE_FLAG_OPEN_REPARSE_POINT", create)
        self.assertIn("O_EXCL", create)
        self.assertIn("O_NOFOLLOW", create)

        write = self.section(durability, "bool WriteStagingFile(", "ReplaceOutcome ReplaceFileAtomically(")
        self.assertLess(write.index("CreateStaging("), write.index("FlushAndClose("))

        replace = self.section(durability, "ReplaceOutcome ReplaceFileAtomically(", "bool CopyFileAtomically(")
        self.assertIn("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH", replace)
        posix = replace[replace.index("#else") :]
        directory_open = posix.index("O_DIRECTORY")
        rename = posix.index("std::filesystem::rename(temporary, destination")
        directory_sync = posix.index("::fsync(directoryFile)")
        self.assertLess(directory_open, rename)
        self.assertLess(rename, directory_sync)
        # Once the rename lands the commit is visible, so it must never be reported as uncommitted.
        self.assertNotIn("ReplaceOutcome::NotCommitted", posix[directory_sync:])


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
