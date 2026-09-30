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

#include "TestFilesystemLinks.h"
#include "TestFramework.h"

#include "Engine/ECS/Components.h"
#include "Engine/Persistence/AsyncDatabase.h"
#include "Utils/SaveFileDurability.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Game/FPSLocalProfile.h"
#include "Game/FPSQuickLoad.h"

#include <cstdint>
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

    std::string GetValue(SQLiteConnection& connection, const std::string& key)
    {
        const auto result = connection.ExecuteRaw("GET " + key);
        if (!result.success || !result.HasRows())
            return "<absent>";
        return result.rows[0].GetString(0);
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

    // A directory link at the staging name runs on every host: an NTFS junction on
    // Windows (no privilege needed, and not reported as a symlink or a directory), a
    // symlink elsewhere. It is unlinked, never written through or emptied.
    const fs::path linkedDirectory = scratch / "linked-target";
    fs::create_directories(linkedDirectory);
    WriteBytes(linkedDirectory / "canary.txt", kCanaryBytes);
    ASSERT_TRUE(SparkTestLinks::MakeDirectoryLink(linkedDirectory, WithSuffix(document, ".tmp")));
    ASSERT_TRUE(SaveFileDurability::WriteFileAtomically(document, "fourth\n", false, error));
    EXPECT_EQ(ReadBytes(document), std::string("fourth\n"));
    EXPECT_EQ(ReadBytes(linkedDirectory / "canary.txt"), std::string(kCanaryBytes));
    EXPECT_TRUE(fs::is_directory(linkedDirectory));
    EXPECT_FALSE(fs::exists(fs::symlink_status(WithSuffix(document, ".tmp"))));

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
// Finding 44: AsyncDatabase staging never follows a planted link
// ============================================================================

TEST(SEC2Persist_AsyncDatabaseStagingIgnoresPlantedHardLink)
{
    Scratch scratch("kv_links");
    const fs::path store = scratch / "store.kv";
    const fs::path canary = scratch / "canary.kv";
    WriteBytes(canary, kCanaryBytes);

    SQLiteConnection connection;
    ASSERT_TRUE(connection.Open(store.string()));
    if (!PlantHardLink(canary, WithSuffix(store, ".tmp")))
        SKIP_TEST("filesystem does not support hard links");

    EXPECT_TRUE(connection.ExecuteRaw("SET alpha one").success);
    EXPECT_EQ(ReadBytes(canary), std::string(kCanaryBytes));
    EXPECT_FALSE(fs::exists(WithSuffix(store, ".tmp")));
    connection.Close();

    SQLiteConnection reopened;
    ASSERT_TRUE(reopened.Open(store.string()));
    EXPECT_EQ(GetValue(reopened, "alpha"), std::string("one"));
    reopened.Close();
}

// ============================================================================
// Finding 46: plain SET/DELETE report and roll back a failed publication
// ============================================================================

TEST(SEC2Persist_AsyncDatabaseFailedWritesRollBack)
{
    Scratch scratch("kv_rollback");
    const fs::path store = scratch / "store.kv";

    {
        SQLiteConnection connection;
        ASSERT_TRUE(connection.Open(store.string()));
        ASSERT_TRUE(connection.ExecuteRaw("SET kept original").success);

        // A non-empty directory on the staging name makes every publication fail.
        const fs::path blocker = WithSuffix(store, ".tmp") / "occupant";
        fs::create_directories(blocker);

        const auto overwrite = connection.ExecuteRaw("SET kept replaced");
        EXPECT_FALSE(overwrite.success);
        EXPECT_STR_CONTAINS(overwrite.errorMessage, "not persisted");
        EXPECT_EQ(GetValue(connection, "kept"), std::string("original"));

        const auto insert = connection.ExecuteRaw("SET fresh value");
        EXPECT_FALSE(insert.success);
        EXPECT_EQ(GetValue(connection, "fresh"), std::string("<absent>"));

        const auto erase = connection.ExecuteRaw("DELETE kept");
        EXPECT_FALSE(erase.success);
        EXPECT_EQ(erase.affectedRows, 0);
        EXPECT_EQ(GetValue(connection, "kept"), std::string("original"));

        fs::remove_all(WithSuffix(store, ".tmp"));
        connection.Close();
    }

    SQLiteConnection reopened;
    ASSERT_TRUE(reopened.Open(store.string()));
    EXPECT_EQ(GetValue(reopened, "kept"), std::string("original"));
    EXPECT_EQ(GetValue(reopened, "fresh"), std::string("<absent>"));
    reopened.Close();
}

// ============================================================================
// Finding 47: an existing store that cannot be loaded completely fails Open
// ============================================================================

TEST(SEC2Persist_AsyncDatabaseRefusesDamagedStore)
{
    Scratch scratch("kv_damaged");

    // A missing file is a new, empty store.
    {
        SQLiteConnection fresh;
        EXPECT_TRUE(fresh.Open((scratch / "new.kv").string()));
        fresh.Close();
    }

    struct DamagedStore
    {
        const char* name;
        std::string bytes;
    };
    const DamagedStore damaged[] = {
        {"no_separator.kv", "#!spark-kv-v2\nalpha\tone\nbroken-record\n"},
        {"truncated.kv", "#!spark-kv-v2\nalpha\tone\nbeta\ttw"},
        {"bad_escape.kv", "#!spark-kv-v2\nalpha\tone\\q\n"},
        {"trailing_backslash.kv", "#!spark-kv-v2\nalpha\tone\\\n"},
        {"duplicate.kv", "#!spark-kv-v2\nalpha\tone\nalpha\ttwo\n"},
        {"legacy_no_separator.kv", "alpha\tone\nsecond line of a multi-line legacy value\n"},
    };
    for (const DamagedStore& store : damaged)
    {
        const fs::path path = scratch / store.name;
        WriteBytes(path, store.bytes);

        SQLiteConnection connection;
        EXPECT_FALSE(connection.Open(path.string()));
        EXPECT_FALSE(connection.IsOpen());
        // Refused, never rewritten: a partial store flushed over it would erase records.
        EXPECT_EQ(ReadBytes(path), store.bytes);

        AsyncDatabasePool pool;
        EXPECT_FALSE(pool.Open(path.string(), 1));
        EXPECT_EQ(ReadBytes(path), store.bytes);
    }

    // Something that exists but is not a readable regular file is not "missing".
    const fs::path directory = scratch / "directory.kv";
    fs::create_directories(directory / "occupant");
    SQLiteConnection onDirectory;
    EXPECT_FALSE(onDirectory.Open(directory.string()));
    EXPECT_TRUE(fs::is_directory(directory / "occupant"));

    // A well-formed store, including legacy and escaped CR/tab/newline values, still loads.
    const fs::path valid = scratch / "valid.kv";
    WriteBytes(valid, "#!spark-kv-v2\nalpha\tline1\\nline2\\ttabbed\\rcr\\\\slash\n");
    SQLiteConnection connection;
    ASSERT_TRUE(connection.Open(valid.string()));
    EXPECT_EQ(GetValue(connection, "alpha"), std::string("line1\nline2\ttabbed\rcr\\slash"));
    ASSERT_TRUE(connection.ExecuteRaw("SET carriage 'ends-with-cr\r'").success);
    connection.Close();
    SQLiteConnection reopened;
    ASSERT_TRUE(reopened.Open(valid.string()));
    EXPECT_EQ(GetValue(reopened, "carriage"), std::string("ends-with-cr\r"));
    reopened.Close();
}

// ============================================================================
// Findings 47/44: the writer never publishes a store its own reader refuses
// ============================================================================

TEST(SEC2Persist_AsyncDatabaseNeverPublishesOverBudgetStore)
{
    Scratch scratch("kv_budget");
    const fs::path store = scratch / "store.kv";

    // "#!spark-kv-v2\n" (14) + "a\t1\n" (4) = 18 bytes. 24 raw backslashes escape to 48,
    // so "big\t<48>\n" adds 53 and the revision would be 71 bytes: over a 64-byte budget
    // although the unescaped data (47 bytes) fits. Without the writer-side check the SET
    // succeeds and the next Open() refuses the store it published.
    constexpr std::uintmax_t kBudget = 64;
    const std::string backslashes(24, '\\');

    std::string published;
    {
        SQLiteConnection connection(kBudget);
        ASSERT_TRUE(connection.Open(store.string()));
        ASSERT_TRUE(connection.ExecuteRaw("SET a 1").success);
        published = ReadBytes(store);
        ASSERT_EQ(published, std::string("#!spark-kv-v2\na\t1\n"));

        const auto oversized = connection.ExecuteRaw("SET big " + backslashes);
        EXPECT_FALSE(oversized.success);
        EXPECT_STR_CONTAINS(oversized.errorMessage, "not persisted");
        EXPECT_EQ(GetValue(connection, "big"), std::string("<absent>"));
        EXPECT_EQ(ReadBytes(store), published);

        // A transaction whose commit would exceed the budget is not committed either.
        ASSERT_TRUE(connection.BeginTransaction());
        ASSERT_TRUE(connection.ExecuteRaw("SET big " + backslashes).success);
        EXPECT_FALSE(connection.CommitTransaction());
        EXPECT_EQ(GetValue(connection, "big"), std::string("<absent>"));
        EXPECT_EQ(GetValue(connection, "a"), std::string("1"));
        EXPECT_EQ(ReadBytes(store), published);

        // A value that fits after escaping is still accepted.
        EXPECT_TRUE(connection.ExecuteRaw("SET b " + std::string(8, '\\')).success);
        EXPECT_LE(static_cast<std::uintmax_t>(fs::file_size(store)), kBudget);
        connection.Close();
    }

    // Every acknowledged revision reopens under the same budget.
    SQLiteConnection reopened(kBudget);
    ASSERT_TRUE(reopened.Open(store.string()));
    EXPECT_EQ(GetValue(reopened, "a"), std::string("1"));
    EXPECT_EQ(GetValue(reopened, "b"), std::string(8, '\\'));
    EXPECT_EQ(GetValue(reopened, "big"), std::string("<absent>"));
    reopened.Close();

    // The lowered budget governs the reader too: a store over it is refused, untouched.
    const fs::path large = scratch / "large.kv";
    const std::string largeBytes = "#!spark-kv-v2\nbig\t" + std::string(64, 'x') + "\n";
    WriteBytes(large, largeBytes);
    SQLiteConnection lowBudget(kBudget);
    EXPECT_FALSE(lowBudget.Open(large.string()));
    EXPECT_EQ(ReadBytes(large), largeBytes);
    SQLiteConnection defaultBudget;
    ASSERT_TRUE(defaultBudget.Open(large.string()));
    EXPECT_EQ(GetValue(defaultBudget, "big"), std::string(64, 'x'));
    defaultBudget.Close();
}

TEST(SEC2Persist_AsyncDatabaseRefusesStoreItCouldNotRepublish)
{
    // SparkFuzzAsyncDatabase's regression-legacy-rewrite-over-budget.db: a legacy
    // (unescaped) store keeps raw backslashes, and an escaped one may hold raw tabs, so
    // both grow when FlushToDisk rewrites them. A 27-byte legacy file rewrites to
    // 14 + 2 + 48 + 1 = 65 bytes, over a 64-byte budget; accepting it would fail every
    // later write. Open refuses it and leaves it untouched.
    Scratch scratch("kv_republish");
    constexpr std::uintmax_t kBudget = 64;
    const fs::path legacy = scratch / "legacy.kv";
    const std::string legacyBytes = "k\t" + std::string(24, '\\') + "\n";
    ASSERT_EQ(legacyBytes.size(), std::size_t{27});
    WriteBytes(legacy, legacyBytes);
    SQLiteConnection legacyConnection(kBudget);
    EXPECT_FALSE(legacyConnection.Open(legacy.string()));
    EXPECT_FALSE(legacyConnection.IsOpen());
    EXPECT_EQ(ReadBytes(legacy), legacyBytes);

    const fs::path rawTabs = scratch / "raw-tabs.kv";
    const std::string rawTabBytes = "#!spark-kv-v2\nk\t" + std::string(47, '\t') + "\n";
    ASSERT_EQ(rawTabBytes.size(), std::size_t{64});
    WriteBytes(rawTabs, rawTabBytes);
    SQLiteConnection rawTabConnection(kBudget);
    EXPECT_FALSE(rawTabConnection.Open(rawTabs.string()));
    EXPECT_EQ(ReadBytes(rawTabs), rawTabBytes);

    // A legacy store whose rewrite fits is still accepted and republished escaped.
    const fs::path fits = scratch / "fits.kv";
    WriteBytes(fits, "k\t" + std::string(8, '\\') + "\n");
    SQLiteConnection fitsConnection(kBudget);
    ASSERT_TRUE(fitsConnection.Open(fits.string()));
    EXPECT_EQ(GetValue(fitsConnection, "k"), std::string(8, '\\'));
    ASSERT_TRUE(fitsConnection.BeginTransaction());
    ASSERT_TRUE(fitsConnection.CommitTransaction());
    EXPECT_EQ(ReadBytes(fits), "#!spark-kv-v2\nk\t" + std::string(16, '\\') + "\n");
    fitsConnection.Close();
}

// ============================================================================
// Finding 53: one authority per store file
// ============================================================================

TEST(SEC2Persist_AsyncDatabaseRejectsSecondAuthority)
{
    Scratch scratch("kv_authority");
    const std::string store = (scratch / "store.kv").string();

    SQLiteConnection first;
    ASSERT_TRUE(first.Open(store));
    ASSERT_TRUE(first.ExecuteRaw("SET owner first").success);

    // A second holder would keep its own snapshot and its flushes would erase the first's.
    SQLiteConnection second;
    EXPECT_FALSE(second.Open(store));
    AsyncDatabasePool pool;
    EXPECT_FALSE(pool.Open(store, 1));

    first.Close();
    ASSERT_TRUE(second.Open(store));
    EXPECT_EQ(GetValue(second, "owner"), std::string("first"));
    second.Close();

    ASSERT_TRUE(pool.Open(store, 1));
    pool.Close();
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

TEST(SEC2Persist_AsyncDatabaseAcksOnlyAfterDirectorySync)
{
#if defined(_WIN32)
    SKIP_TEST("POSIX directory-sync path; Windows replaces with MoveFileExW write-through");
#else
    if (::geteuid() == 0)
        SKIP_TEST("root bypasses directory permissions");

    Scratch scratch("kv_dirsync");
    const fs::path directory = scratch / "store";
    fs::create_directories(directory);
    const fs::path store = directory / "store.kv";

    SQLiteConnection connection;
    ASSERT_TRUE(connection.Open(store.string()));
    ASSERT_TRUE(connection.ExecuteRaw("SET durable yes").success);

    // Without a directory sync the rename can be lost on power failure, so the write must
    // not be acknowledged. The old flush never synced the directory and reported success.
    ASSERT_TRUE(MakeDirectoryUnsyncable(directory));
    EXPECT_FALSE(connection.ExecuteRaw("SET unsynced no").success);
    EXPECT_EQ(GetValue(connection, "unsynced"), std::string("<absent>"));

    std::error_code ignored;
    fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, ignored);
    connection.Close();

    SQLiteConnection reopened;
    ASSERT_TRUE(reopened.Open(store.string()));
    EXPECT_EQ(GetValue(reopened, "durable"), std::string("yes"));
    EXPECT_EQ(GetValue(reopened, "unsynced"), std::string("<absent>"));
    reopened.Close();
#endif
}

// ============================================================================
// Finding 48: disk-loaded entities never carry an indeterminate entityID
// ============================================================================

TEST(SEC2Persist_SerializedEntityIdIsDeterministic)
{
    // Default-initialize over poisoned storage: without a member initializer the field
    // keeps the poison bytes, which ReadFromFile then copied into every loaded entity.
    alignas(SerializedEntity) unsigned char storage[sizeof(SerializedEntity)];
    std::memset(storage, 0xA5, sizeof(storage));
    SerializedEntity* entity = ::new (static_cast<void*>(storage)) SerializedEntity;
    EXPECT_EQ(entity->entityID, 0u);
    std::destroy_at(entity);
}

// ============================================================================
// Finding 50: hand-written serializers keep every authored field
// ============================================================================

TEST(SEC2Persist_RigidBodyAndLightFieldsRoundTrip)
{
    Scratch scratch("component_fields");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    ASSERT_TRUE(saveSystem.Initialize(scratch.Path().string()));

    World source;
    const EntityID entity = source.CreateEntity("authored");
    auto& body = source.AddComponent<RigidBodyComponent>(entity);
    body.gravityFactor = 0.25f;
    body.motionQuality = RigidBodyComponent::MotionQuality::LinearCast;
    auto& light = source.AddComponent<LightComponent>(entity);
    light.type = LightComponent::Type::Spot;
    light.spotAngle = 60.0f;
    light.spotInnerAngle = 20.0f;
    light.shadowMapResolution = 2048;

    SaveMetadata metadata;
    metadata.saveName = "fields";
    ASSERT_TRUE(saveSystem.Save("fields", source, metadata));

    World loaded;
    ASSERT_TRUE(saveSystem.Load("fields", loaded));
    const EntityID loadedEntity = FindNamedEntity(loaded, "authored");
    ASSERT_TRUE(loadedEntity != entt::null);
    const RigidBodyComponent* loadedBody = loaded.GetComponent<RigidBodyComponent>(loadedEntity);
    const LightComponent* loadedLight = loaded.GetComponent<LightComponent>(loadedEntity);
    ASSERT_TRUE(loadedBody != nullptr);
    ASSERT_TRUE(loadedLight != nullptr);
    EXPECT_NEAR(loadedBody->gravityFactor, 0.25f, 0.0001f);
    EXPECT_TRUE(loadedBody->motionQuality == RigidBodyComponent::MotionQuality::LinearCast);
    EXPECT_NEAR(loadedLight->spotAngle, 60.0f, 0.0001f);
    EXPECT_NEAR(loadedLight->spotInnerAngle, 20.0f, 0.0001f);
    EXPECT_EQ(loadedLight->shadowMapResolution, 2048);

    // A save written before these keys existed still loads, with the component defaults.
    SaveData legacy = saveSystem.SerializeWorld(source, metadata);
    for (SerializedEntity& serialized : legacy.entities)
    {
        for (SerializedComponent& component : serialized.components)
        {
            for (const char* key :
                 {"gravityFactor", "motionQuality", "spotAngle", "spotInnerAngle", "shadowMapResolution"})
                component.properties.erase(key);
        }
    }
    World legacyWorld;
    ASSERT_TRUE(saveSystem.DeserializeWorld(legacy, legacyWorld));
    const EntityID legacyEntity = FindNamedEntity(legacyWorld, "authored");
    ASSERT_TRUE(legacyEntity != entt::null);
    EXPECT_NEAR(legacyWorld.GetComponent<RigidBodyComponent>(legacyEntity)->gravityFactor, 1.0f, 0.0001f);
    EXPECT_EQ(legacyWorld.GetComponent<LightComponent>(legacyEntity)->shadowMapResolution, 1024);

    // An unknown motion quality invalidates the snapshot rather than casting garbage.
    SaveData malformed = saveSystem.SerializeWorld(source, metadata);
    for (SerializedEntity& serialized : malformed.entities)
    {
        for (SerializedComponent& component : serialized.components)
        {
            if (component.typeName == "RigidBodyComponent")
                component.properties["motionQuality"] = "7";
        }
    }
    World rejected;
    EXPECT_FALSE(saveSystem.DeserializeWorld(malformed, rejected));
}

// ============================================================================
// Finding 52: DeleteSave reports a retained copy it could not remove
// ============================================================================

TEST(SEC2Persist_DeleteSaveReportsSurvivingRetainedCopy)
{
    Scratch scratch("delete_partial");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    ASSERT_TRUE(saveSystem.Initialize(scratch.Path().string()));

    World world;
    world.CreateEntity("slot");
    SaveMetadata metadata;
    ASSERT_TRUE(saveSystem.Save("partial", world, metadata));

    // A retained copy that cannot be removed (a non-empty directory stands in for a
    // locked file or a different ACL).
    const fs::path backup = scratch / "partial.spark_save.bak";
    fs::create_directories(backup / "occupant");

    EXPECT_FALSE(saveSystem.DeleteSave("partial"));
    EXPECT_FALSE(fs::exists(scratch / "partial.spark_save"));
    EXPECT_TRUE(fs::exists(backup));

    fs::remove_all(backup);
    EXPECT_TRUE(saveSystem.DeleteSave("partial"));
    EXPECT_FALSE(saveSystem.SaveExists("partial"));
}

// ============================================================================
// Finding 49: the FPS quick-load validates the profile before replacing the world
// ============================================================================

TEST(SEC2Persist_FPSQuickLoadRejectsProfileBeforeWorldCommit)
{
    Scratch scratch("fps_quickload");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    ASSERT_TRUE(saveSystem.Initialize(scratch.Path().string()));

    World saved;
    saved.CreateEntity("saved-world");
    SaveMetadata metadata;

    FPSLocalProfile savedProfile;
    savedProfile.progressionXP = 250;
    std::unordered_map<std::string, std::string> goodState;
    savedProfile.WriteTo(goodState);
    std::unordered_map<std::string, std::string> futureState = goodState;
    futureState["fps.profile.version"] = std::to_string(FPSLocalProfile::kVersion + 1);
    ASSERT_TRUE(saveSystem.Save("future", saved, metadata, futureState));
    ASSERT_TRUE(saveSystem.Save("good", saved, metadata, goodState));

    World live;
    live.CreateEntity("live-world");
    FPSLocalProfile liveProfile;
    liveProfile.progressionXP = 7;
    std::string profileError;

    EXPECT_TRUE(LoadSlotWithProfile(saveSystem, "future", live, liveProfile, profileError) ==
                FPSQuickLoadStatus::ProfileRejected);
    EXPECT_FALSE(profileError.empty());
    EXPECT_TRUE(FindNamedEntity(live, "live-world") != entt::null);
    EXPECT_TRUE(FindNamedEntity(live, "saved-world") == entt::null);
    EXPECT_EQ(liveProfile.progressionXP, 7);

    EXPECT_TRUE(LoadSlotWithProfile(saveSystem, "missing", live, liveProfile, profileError) ==
                FPSQuickLoadStatus::LoadFailed);
    EXPECT_TRUE(FindNamedEntity(live, "live-world") != entt::null);

    EXPECT_TRUE(LoadSlotWithProfile(saveSystem, "good", live, liveProfile, profileError) == FPSQuickLoadStatus::Loaded);
    EXPECT_TRUE(FindNamedEntity(live, "saved-world") != entt::null);
    EXPECT_TRUE(FindNamedEntity(live, "live-world") == entt::null);
    EXPECT_EQ(liveProfile.progressionXP, 250);
}
