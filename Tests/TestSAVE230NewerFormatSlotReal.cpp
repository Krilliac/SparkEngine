/**
 * @file TestSAVE230NewerFormatSlotReal.cpp
 * @brief SAVE-230: a slot written by a newer SparkEngine build is neither silently
 *        rolled back to its older retained copy nor destroyed by a downgraded write.
 *
 * Before this fix, SaveSystem treated "declares a newer format" exactly like "torn or
 * corrupt": Load() fell back to `<slot>.spark_save.bak` (an older revision) with only a
 * warning, GetSaveSlots() listed that older copy as the slot, and the next Save() saw an
 * "unreadable" primary and atomically replaced it - permanently discarding the newer
 * build's progress after a downgrade.
 *
 * Every save below is produced by the production SaveSystem::Save path. No writer for a
 * newer format exists yet, so the newer-build primary is that real current-version file
 * with only its header version advanced and its CRC-32 trailer resealed - the shape a
 * next format that keeps the v4 integrity envelope would have. The second test proves the
 * guard does not swallow ordinary corruption: a current-version file whose version field
 * alone was damaged still recovers the retained copy.
 */

#include "TestFramework.h"
#include "Engine/ECS/Components.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Utils/CRC32.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Spark;

namespace
{
    constexpr size_t kHeaderBytes = 8u;
    constexpr size_t kTrailerBytes = sizeof(uint32_t);

    std::string MakeSave230TempDir(const char* name)
    {
        auto dir = std::filesystem::temp_directory_path() / (std::string("spark_save230_") + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir.string();
    }

    std::vector<char> ReadSave230Bytes(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    bool WriteSave230Bytes(const std::filesystem::path& path, const std::vector<char>& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return output.good();
    }

    uint32_t ReadLE32(const std::vector<char>& bytes, size_t offset)
    {
        uint32_t value = 0;
        for (size_t index = 0; index < 4u; ++index)
            value |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[offset + index])) << (8u * index);
        return value;
    }

    void WriteLE32(std::vector<char>& bytes, size_t offset, uint32_t value)
    {
        for (size_t index = 0; index < 4u; ++index)
            bytes[offset + index] = static_cast<char>((value >> (8u * index)) & 0xFFu);
    }

    /// True when @p bytes is a SPRK file of this build's version with a valid CRC-32 trailer.
    bool IsSealedCurrentVersionSave(const std::vector<char>& bytes)
    {
        if (bytes.size() < kHeaderBytes + kTrailerBytes || std::memcmp(bytes.data(), "SPRK", 4) != 0 ||
            ReadLE32(bytes, 4) != kCurrentSaveVersion)
        {
            return false;
        }
        const size_t trailer = bytes.size() - kTrailerBytes;
        return ReadLE32(bytes, trailer) == ComputeCRC32(bytes.data(), trailer);
    }

    /// Advance the header version and reseal the trailer over the complete new prefix.
    void ResealWithHeaderVersion(std::vector<char>& bytes, uint32_t version)
    {
        WriteLE32(bytes, 4, version);
        const size_t trailer = bytes.size() - kTrailerBytes;
        WriteLE32(bytes, trailer, ComputeCRC32(bytes.data(), trailer));
    }

    bool SaveNamedRevision(SaveSystem& saveSystem, const char* slotName, const char* entityName, const char* saveName)
    {
        World world;
        world.AddComponent<Transform>(world.CreateEntity(entityName));
        SaveMetadata metadata;
        metadata.saveName = saveName;
        return saveSystem.Save(slotName, world, metadata);
    }

    bool HasNamedEntity(World& world, const std::string& name)
    {
        auto&& entities = world.GetRegistry().storage<entt::entity>();
        for (auto&& [entity] : entities.each())
        {
            if (const NameComponent* component = world.GetComponent<NameComponent>(entity);
                component && component->name == name)
            {
                return true;
            }
        }
        return false;
    }

    size_t CountListedSlots(const SaveSystem& saveSystem, const std::string& slotName)
    {
        size_t count = 0;
        for (const SaveMetadata& listed : saveSystem.GetSaveSlots())
        {
            if (listed.slotName == slotName)
                ++count;
        }
        return count;
    }
} // namespace

TEST(SaveMigration_NewerBuildPrimaryIsNeitherRolledBackNorOverwritten)
{
    const std::string dir = MakeSave230TempDir("newer_build_primary");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    ASSERT_TRUE(saveSystem.Initialize(dir));

    ASSERT_TRUE(SaveNamedRevision(saveSystem, "newer-build", "older-revision", "Older revision"));
    ASSERT_TRUE(SaveNamedRevision(saveSystem, "newer-build", "newer-revision", "Newer revision"));

    const auto primaryPath = std::filesystem::path(dir) / "newer-build.spark_save";
    const auto backupPath = std::filesystem::path(dir) / "newer-build.spark_save.bak";
    const auto tempPath = std::filesystem::path(dir) / "newer-build.spark_save.tmp";
    ASSERT_TRUE(std::filesystem::exists(backupPath));

    std::vector<char> newerPrimary = ReadSave230Bytes(primaryPath);
    ASSERT_TRUE(IsSealedCurrentVersionSave(newerPrimary));
    ResealWithHeaderVersion(newerPrimary, kCurrentSaveVersion + 1);
    ASSERT_TRUE(WriteSave230Bytes(primaryPath, newerPrimary));
    const std::vector<char> retainedBefore = ReadSave230Bytes(backupPath);
    ASSERT_TRUE(IsSealedCurrentVersionSave(retainedBefore));

    // Load must fail instead of silently returning the older retained revision, and
    // must leave the caller's world and custom state untouched.
    World liveWorld;
    liveWorld.AddComponent<Transform>(liveWorld.CreateEntity("newer-build-live-sentinel"));
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};
    EXPECT_FALSE(saveSystem.Load("newer-build", liveWorld, customState));
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    EXPECT_TRUE(HasNamedEntity(liveWorld, "newer-build-live-sentinel"));
    EXPECT_FALSE(HasNamedEntity(liveWorld, "older-revision"));
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("live"), std::string("sentinel"));

    // The slot still exists, but its older retained copy is not advertised as the slot.
    EXPECT_TRUE(saveSystem.SaveExists("newer-build"));
    EXPECT_EQ(CountListedSlots(saveSystem, "newer-build"), static_cast<size_t>(0));

    // A downgraded write is refused: the newer data and the retained copy survive
    // byte-for-byte, and no temporary file is left behind.
    EXPECT_FALSE(SaveNamedRevision(saveSystem, "newer-build", "downgraded-revision", "Downgraded revision"));
    EXPECT_TRUE(ReadSave230Bytes(primaryPath) == newerPrimary);
    EXPECT_TRUE(ReadSave230Bytes(backupPath) == retainedBefore);
    EXPECT_FALSE(std::filesystem::exists(tempPath));

    // Explicit deletion stays the player's way to discard the newer data.
    EXPECT_TRUE(saveSystem.DeleteSave("newer-build"));
    EXPECT_FALSE(saveSystem.SaveExists("newer-build"));
    EXPECT_TRUE(SaveNamedRevision(saveSystem, "newer-build", "fresh-revision", "Fresh revision"));
    World reloaded;
    EXPECT_TRUE(saveSystem.Load("newer-build", reloaded));
    EXPECT_TRUE(HasNamedEntity(reloaded, "fresh-revision"));

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_DamagedVersionFieldOfCurrentSaveStillRecoversRetainedCopy)
{
    const std::string dir = MakeSave230TempDir("damaged_version_field");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    ASSERT_TRUE(saveSystem.Initialize(dir));

    ASSERT_TRUE(SaveNamedRevision(saveSystem, "damaged-version", "retained-revision", "Retained revision"));
    ASSERT_TRUE(SaveNamedRevision(saveSystem, "damaged-version", "damaged-revision", "Damaged revision"));

    const auto primaryPath = std::filesystem::path(dir) / "damaged-version.spark_save";
    std::vector<char> damaged = ReadSave230Bytes(primaryPath);
    ASSERT_TRUE(IsSealedCurrentVersionSave(damaged));
    // Only the version field is damaged; the trailer still seals the original header,
    // so this is corruption of a file this build wrote, not a newer format.
    WriteLE32(damaged, 4, kCurrentSaveVersion + 1);
    ASSERT_TRUE(WriteSave230Bytes(primaryPath, damaged));

    World recovered;
    EXPECT_TRUE(saveSystem.Load("damaged-version", recovered));
    EXPECT_TRUE(HasNamedEntity(recovered, "retained-revision"));
    EXPECT_FALSE(HasNamedEntity(recovered, "damaged-revision"));
    EXPECT_EQ(CountListedSlots(saveSystem, "damaged-version"), static_cast<size_t>(1));

    // The damaged primary may be replaced; a later load sees the new revision.
    EXPECT_TRUE(SaveNamedRevision(saveSystem, "damaged-version", "replacement-revision", "Replacement revision"));
    World replaced;
    EXPECT_TRUE(saveSystem.Load("damaged-version", replaced));
    EXPECT_TRUE(HasNamedEntity(replaced, "replacement-revision"));

    std::filesystem::remove_all(dir);
}
