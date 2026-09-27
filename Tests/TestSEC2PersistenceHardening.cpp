/**
 * @file TestSEC2PersistenceHardening.cpp
 * @brief SEC2 persistence hardening: link-safe staging, fail-closed stores, honest commit
 *        reporting and transactional quick-load.
 *
 * Every test drives production code against real files:
 * - SaveFileDurability / SaveSystem / AsyncDatabase stage writes through an exclusive,
 *   no-follow create, so a hard link or symlink planted at the predictable `.tmp` name
 *   never redirects a write into its target.
 * - ReplaceFileAtomically never reports "not committed" after the rename landed, and the
 *   AsyncDatabase store acknowledges a write only after the POSIX directory sync.
 * - The AsyncDatabase KV store rolls back unpublished SET/DELETE, refuses damaged or
 *   unreadable files instead of starting empty, and admits one authority per file.
 * - SaveSystem persists every RigidBody/Light field, value-initializes loaded entities,
 *   and DeleteSave reports a surviving retained copy as a failure.
 * - The FPS quick-load validates the profile before the world is replaced.
 */

#include "TestFramework.h"

#include "Engine/ECS/Components.h"
#include "Engine/Persistence/AsyncDatabase.h"
#include "Engine/SaveSystem/SaveFileDurability.h"
#include "Engine/SaveSystem/SaveSystem.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <unordered_map>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace Spark;
using Spark::Persistence::AsyncDatabasePool;
using Spark::Persistence::SQLiteConnection;

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* kCanaryBytes = "canary: must never be written through a staging link\n";

    /// Fresh per-test scratch directory, removed (with its permissions restored) on scope exit.
    class Scratch
    {
      public:
        explicit Scratch(const char* name) : m_path(fs::temp_directory_path() / (std::string("spark_sec2_") + name))
        {
            std::error_code ignored;
            fs::permissions(m_path, fs::perms::owner_all, fs::perm_options::add, ignored);
            fs::remove_all(m_path, ignored);
            fs::create_directories(m_path);
        }
        ~Scratch()
        {
            std::error_code ignored;
            fs::permissions(m_path, fs::perms::owner_all, fs::perm_options::add, ignored);
            fs::remove_all(m_path, ignored);
        }
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;

        const fs::path& Path() const { return m_path; }
        fs::path operator/(const char* child) const { return m_path / child; }

      private:
        fs::path m_path;
    };

    std::string ReadBytes(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    void WriteBytes(const fs::path& path, const std::string& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << bytes;
    }

    fs::path WithSuffix(const fs::path& path, const char* suffix)
    {
        fs::path result = path;
        result += suffix;
        return result;
    }

    /// Plant a hard link to @p canary at @p staging; false when the filesystem cannot.
    bool PlantHardLink(const fs::path& canary, const fs::path& staging)
    {
        std::error_code error;
        fs::create_hard_link(canary, staging, error);
        return !error;
    }

    EntityID FindNamedEntity(World& world, const std::string& name)
    {
        auto&& entities = world.GetRegistry().storage<entt::entity>();
        for (auto&& [entity] : entities.each())
        {
            if (const NameComponent* component = world.GetComponent<NameComponent>(entity);
                component && component->name == name)
            {
                return entity;
            }
        }
        return entt::null;
    }

#if !defined(_WIN32)
    /// A directory the process can create and unlink in but cannot open for reading, so
    /// the directory sync that makes a rename durable cannot run.
    bool MakeDirectoryUnsyncable(const fs::path& directory)
    {
        std::error_code error;
        fs::permissions(directory, fs::perms::owner_write | fs::perms::owner_exec, fs::perm_options::replace, error);
        return !error;
    }
#endif
} // namespace

// ============================================================================
// Finding 43: SaveFileDurability / SaveSystem staging never follows a planted link
// ============================================================================

TEST(SEC2Persist_DocumentStagingIgnoresPlantedLinks)
{
    Scratch scratch("document_links");
    const fs::path canary = scratch / "canary.txt";
    const fs::path document = scratch / "Level.sparkproject";
    WriteBytes(canary, kCanaryBytes);
    WriteBytes(document, "previous\n");

    // A hard link at the staging name: a truncating path-based open writes into the
    // canary's inode. The exclusive create unlinks the link and stages a fresh file.
    if (!PlantHardLink(canary, WithSuffix(document, ".tmp")))
        SKIP_TEST("filesystem does not support hard links");
    ASSERT_TRUE(PlantHardLink(canary, WithSuffix(document, ".bak.tmp")));

    std::error_code error;
    ASSERT_TRUE(SaveFileDurability::WriteFileAtomically(document, "replacement\n", true, error));
    EXPECT_FALSE(static_cast<bool>(error));
    EXPECT_EQ(ReadBytes(canary), std::string(kCanaryBytes));
    EXPECT_EQ(ReadBytes(document), std::string("replacement\n"));
    EXPECT_EQ(ReadBytes(SaveFileDurability::BackupPathFor(document)), std::string("previous\n"));
    EXPECT_FALSE(fs::exists(WithSuffix(document, ".tmp")));
    EXPECT_FALSE(fs::exists(WithSuffix(document, ".bak.tmp")));

    // A symlink at the staging name (POSIX, or Windows with symlink privilege).
    std::error_code linkError;
    fs::create_symlink(canary, WithSuffix(document, ".tmp"), linkError);
    if (!linkError)
    {
        ASSERT_TRUE(SaveFileDurability::WriteFileAtomically(document, "third\n", false, error));
        EXPECT_EQ(ReadBytes(canary), std::string(kCanaryBytes));
        EXPECT_EQ(ReadBytes(document), std::string("third\n"));
        EXPECT_FALSE(fs::is_symlink(document));
        EXPECT_FALSE(fs::exists(fs::symlink_status(WithSuffix(document, ".tmp"))));
    }

    // A directory squatting on the staging name is never removed; the write fails closed.
    fs::create_directories(WithSuffix(document, ".tmp") / "occupant");
    EXPECT_FALSE(SaveFileDurability::WriteFileAtomically(document, "blocked\n", false, error));
    EXPECT_TRUE(error == std::make_error_code(std::errc::is_a_directory));
    EXPECT_TRUE(fs::is_directory(WithSuffix(document, ".tmp") / "occupant"));
}

TEST(SEC2Persist_SaveStagingIgnoresPlantedHardLinks)
{
    Scratch scratch("save_links");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    ASSERT_TRUE(saveSystem.Initialize(scratch.Path().string()));

    const fs::path canary = scratch / "canary.bin";
    WriteBytes(canary, kCanaryBytes);

    World world;
    world.CreateEntity("first");
    SaveMetadata metadata;
    metadata.saveName = "first";
    ASSERT_TRUE(saveSystem.Save("linked", world, metadata));

    // Both staging names a save uses: the primary's and the retained copy's.
    const fs::path primary = scratch / "linked.spark_save";
    if (!PlantHardLink(canary, WithSuffix(primary, ".tmp")))
        SKIP_TEST("filesystem does not support hard links");
    ASSERT_TRUE(PlantHardLink(canary, WithSuffix(primary, ".bak.tmp")));

    world.CreateEntity("second");
    metadata.saveName = "second";
    ASSERT_TRUE(saveSystem.Save("linked", world, metadata));
    EXPECT_EQ(ReadBytes(canary), std::string(kCanaryBytes));
    EXPECT_FALSE(fs::exists(WithSuffix(primary, ".tmp")));
    EXPECT_FALSE(fs::exists(WithSuffix(primary, ".bak.tmp")));

    World loaded;
    ASSERT_TRUE(saveSystem.Load("linked", loaded));
    EXPECT_TRUE(FindNamedEntity(loaded, "second") != entt::null);
    EXPECT_TRUE(fs::exists(WithSuffix(primary, ".bak")));
}

// ============================================================================
// Findings 51 and 54: commit reporting and the POSIX directory sync
// ============================================================================

TEST(SEC2Persist_ReplaceNeverReportsUncommittedAfterRename)
{
#if defined(_WIN32)
    SKIP_TEST("POSIX directory-sync path; Windows replaces with MoveFileExW write-through");
#else
    if (::geteuid() == 0)
        SKIP_TEST("root bypasses directory permissions");

    Scratch scratch("replace_commit");
    const fs::path directory = scratch / "unsyncable";
    fs::create_directories(directory);
    const fs::path document = directory / "doc.txt";
    WriteBytes(document, "old\n");
    ASSERT_TRUE(MakeDirectoryUnsyncable(directory));

    // Before the fix the rename landed and the later directory open failed, so the call
    // reported failure while the destination already held the new bytes.
    std::error_code error;
    const bool written = SaveFileDurability::WriteFileAtomically(document, "new\n", false, error);
    const std::string contents = ReadBytes(document);
    EXPECT_EQ(written, contents == "new\n");
    EXPECT_FALSE(written);
    EXPECT_EQ(contents, std::string("old\n"));
    EXPECT_FALSE(fs::exists(WithSuffix(document, ".tmp")));

    const fs::path staged = directory / "staged.txt";
    WriteBytes(staged, "staged\n");
    std::error_code replaceError;
    EXPECT_TRUE(SaveFileDurability::ReplaceFileAtomically(staged, document, replaceError) ==
                SaveFileDurability::ReplaceOutcome::NotCommitted);
    EXPECT_EQ(ReadBytes(document), std::string("old\n"));
#endif
}
