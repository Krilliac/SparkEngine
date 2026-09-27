/**
 * @file TestDocumentDurableWrite.cpp
 * @brief SAVE-230: SaveFileDurability::WriteFileAtomically, the whole-document writer behind
 *        prefab and project-file saves.
 *
 * A failed write must leave the destination and its retained `.bak` byte-identical, and the
 * `.bak` refresh must stage and rename (a new file) rather than copy in place, which would
 * truncate the previous-good copy first.
 */

#include "TestFramework.h"
#include "Engine/SaveSystem/SaveFileDurability.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <sys/stat.h>
#endif

namespace
{
    namespace fs = std::filesystem;
    namespace Durability = Spark::SaveFileDurability;

    class ScratchDirectory
    {
      public:
        explicit ScratchDirectory(const char* tag)
        {
            static std::atomic<unsigned int> sequence{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = fs::temp_directory_path() / ("spark-document-write-" + std::string(tag) + "-" +
                                                  std::to_string(stamp) + "-" + std::to_string(sequence++));
            fs::create_directories(m_path);
        }
        ~ScratchDirectory()
        {
            std::error_code ec;
            fs::remove_all(m_path, ec);
        }
        ScratchDirectory(const ScratchDirectory&) = delete;
        ScratchDirectory& operator=(const ScratchDirectory&) = delete;

        const fs::path& Path() const { return m_path; }

      private:
        fs::path m_path;
    };

    std::string ReadBytes(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void WriteBytes(const fs::path& path, const std::string& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << bytes;
    }

    /// Volume + file identity. A rename-based refresh produces a new file; an in-place copy
    /// keeps the old identity.
    std::pair<std::uint64_t, std::uint64_t> FileIdentity(const fs::path& path)
    {
#if defined(_WIN32)
        const HANDLE file = ::CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return {0, 0};
        BY_HANDLE_FILE_INFORMATION info{};
        const bool ok = ::GetFileInformationByHandle(file, &info) != FALSE;
        ::CloseHandle(file);
        if (!ok)
            return {0, 0};
        const std::uint64_t index =
            (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | static_cast<std::uint64_t>(info.nFileIndexLow);
        return {static_cast<std::uint64_t>(info.dwVolumeSerialNumber), index};
#else
        struct stat info
        {
        };
        if (::stat(path.c_str(), &info) != 0)
            return {0, 0};
        return {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino)};
#endif
    }

    fs::path StagingPathFor(const fs::path& destination)
    {
        fs::path staging = destination;
        staging += ".tmp";
        return staging;
    }
} // namespace

TEST(DocumentWrite_ReplacesContentAndRetainsPreviousAsBackup)
{
    ScratchDirectory scratch("replace");
    const fs::path document = scratch.Path() / "Level.sparkproject";
    const fs::path backup = Durability::BackupPathFor(document);
    EXPECT_TRUE(backup == scratch.Path() / "Level.sparkproject.bak");

    std::error_code error;
    ASSERT_TRUE(Durability::WriteFileAtomically(document, "first\r\nrevision\n", true, error));
    EXPECT_FALSE(error);
    // Binary mode: bytes land exactly as given, including a CR the text mode would alter.
    EXPECT_EQ(ReadBytes(document), std::string("first\r\nrevision\n"));
    EXPECT_FALSE(fs::exists(backup));

    ASSERT_TRUE(Durability::WriteFileAtomically(document, "second revision\n", true, error));
    EXPECT_EQ(ReadBytes(document), std::string("second revision\n"));
    EXPECT_EQ(ReadBytes(backup), std::string("first\r\nrevision\n"));

    EXPECT_FALSE(fs::exists(StagingPathFor(document)));
    EXPECT_FALSE(fs::exists(StagingPathFor(backup)));
}

TEST(DocumentWrite_BlockedStagingLeavesDestinationAndBackupUntouched)
{
    ScratchDirectory scratch("blocked");
    const fs::path document = scratch.Path() / "Crate.sparkprefab";
    const fs::path backup = Durability::BackupPathFor(document);
    WriteBytes(document, "current good document\n");
    WriteBytes(backup, "previous good document\n");

    // A non-empty directory on the staging name makes the staging open fail.
    const fs::path staging = StagingPathFor(document);
    fs::create_directories(staging);
    WriteBytes(staging / "occupant.txt", "x");

    std::error_code error;
    EXPECT_FALSE(Durability::WriteFileAtomically(document, "replacement\n", true, error));
    EXPECT_TRUE(static_cast<bool>(error));
    EXPECT_EQ(ReadBytes(document), std::string("current good document\n"));
    EXPECT_EQ(ReadBytes(backup), std::string("previous good document\n"));
    EXPECT_TRUE(fs::is_directory(staging));
    EXPECT_TRUE(fs::exists(staging / "occupant.txt"));
}

TEST(DocumentWrite_NoBackupWhenDisabledOrDestinationAbsent)
{
    ScratchDirectory scratch("nobackup");
    const fs::path document = scratch.Path() / "Settings.json";
    const fs::path backup = Durability::BackupPathFor(document);

    std::error_code error;
    ASSERT_TRUE(Durability::WriteFileAtomically(document, "one", true, error));
    EXPECT_FALSE(fs::exists(backup));

    ASSERT_TRUE(Durability::WriteFileAtomically(document, "two", false, error));
    EXPECT_EQ(ReadBytes(document), std::string("two"));
    EXPECT_FALSE(fs::exists(backup));

    // An existing retained copy is not touched by a write that does not retain one.
    WriteBytes(backup, "kept");
    ASSERT_TRUE(Durability::WriteFileAtomically(document, "three", false, error));
    EXPECT_EQ(ReadBytes(document), std::string("three"));
    EXPECT_EQ(ReadBytes(backup), std::string("kept"));
}

TEST(DocumentWrite_BackupRefreshIsRenameNotInPlace)
{
    ScratchDirectory scratch("rename");
    const fs::path document = scratch.Path() / "Hero.sparkprefab";
    const fs::path backup = Durability::BackupPathFor(document);

    std::error_code error;
    ASSERT_TRUE(Durability::WriteFileAtomically(document, "A", true, error));
    ASSERT_TRUE(Durability::WriteFileAtomically(document, "B", true, error));
    ASSERT_EQ(ReadBytes(backup), std::string("A"));
    const auto before = FileIdentity(backup);
    ASSERT_TRUE(before.second != 0);

    ASSERT_TRUE(Durability::WriteFileAtomically(document, "C", true, error));
    EXPECT_EQ(ReadBytes(backup), std::string("B"));
    EXPECT_EQ(ReadBytes(document), std::string("C"));
    const auto after = FileIdentity(backup);
    ASSERT_TRUE(after.second != 0);
    // Truncate-and-copy in place would keep the .bak's identity and, if interrupted,
    // destroy the only previous-good copy.
    EXPECT_TRUE(before != after);
}
