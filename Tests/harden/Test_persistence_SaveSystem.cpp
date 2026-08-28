// Test_persistence_SaveSystem.cpp
// Regression for two SaveSystem findings:
//   P1: Load/ReadFromFile never validated the save-format version. A file written by a
//       newer, incompatible format is now rejected instead of silently misinterpreted.
//   P2: GetSaveMetadata now uses a metadata-only read path; this test also confirms it
//       still parses the metadata header correctly.
// Both are exercised through the public GetSaveMetadata() (which needs no World/ECS),
// by hand-crafting .spark_save files with the real on-disk binary layout.

#include "TestFramework.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/ReactiveSystem.h"
#include "Utils/LocalFileCache.h"

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Spark;

namespace
{
    // Write a minimal but format-correct save file (header + metadata block only; zero
    // entities are not required because GetSaveMetadata stops after the metadata block).
    void WriteSaveHeader(const std::string& path, uint32_t version, const std::string& metaStr)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write("SPRK", 4);
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));
        uint32_t metaSize = static_cast<uint32_t>(metaStr.size());
        out.write(reinterpret_cast<const char*>(&metaSize), sizeof(metaSize));
        out.write(metaStr.data(), static_cast<std::streamsize>(metaStr.size()));
    }

    std::string MakeTempSaveDir(const char* name)
    {
        auto dir = std::filesystem::temp_directory_path() / (std::string("spark_harden_save_") + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir.string();
    }

    // Metadata layout: three getline fields, then whitespace-separated
    // timestamp playTime health armor posX posY posZ kills deaths.
    const std::string kValidMeta = "My Save\nLevel1\nSoldier\n1234 56.5 100 50 1 2 3 4 5\n";

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    std::vector<char> DecodeHexFixture(const std::string& encoded)
    {
        auto nibble = [](char value) -> int
        {
            if (value >= '0' && value <= '9')
                return value - '0';
            if (value >= 'a' && value <= 'f')
                return value - 'a' + 10;
            if (value >= 'A' && value <= 'F')
                return value - 'A' + 10;
            return -1;
        };

        std::string compact;
        compact.reserve(encoded.size());
        for (char value : encoded)
        {
            if (!std::isspace(static_cast<unsigned char>(value)))
                compact.push_back(value);
        }
        if (compact.size() % 2 != 0)
            return {};

        std::vector<char> decoded;
        decoded.reserve(compact.size() / 2);
        for (size_t index = 0; index < compact.size(); index += 2)
        {
            const int high = nibble(compact[index]);
            const int low = nibble(compact[index + 1]);
            if (high < 0 || low < 0)
                return {};
            decoded.push_back(static_cast<char>((high << 4) | low));
        }
        return decoded;
    }

    bool ReplaceFirstAscii(std::vector<char>& bytes, const std::string& from, const std::string& to)
    {
        if (from.size() != to.size())
            return false;
        const auto match = std::search(bytes.begin(), bytes.end(), from.begin(), from.end());
        if (match == bytes.end())
            return false;
        std::copy(to.begin(), to.end(), match);
        return true;
    }

    bool ReplaceLengthPrefixedString(std::vector<char>& bytes, const std::string& from, const std::string& to)
    {
        if (to.size() > std::numeric_limits<uint16_t>::max())
            return false;

        auto match = bytes.begin();
        while ((match = std::search(match, bytes.end(), from.begin(), from.end())) != bytes.end())
        {
            const size_t offset = static_cast<size_t>(std::distance(bytes.begin(), match));
            if (offset >= sizeof(uint16_t))
            {
                const auto low = static_cast<uint8_t>(bytes[offset - 2]);
                const auto high = static_cast<uint8_t>(bytes[offset - 1]);
                if (static_cast<uint16_t>(low | (high << 8)) == from.size())
                {
                    const auto replacementLength = static_cast<uint16_t>(to.size());
                    bytes[offset - 2] = static_cast<char>(replacementLength & 0xFFu);
                    bytes[offset - 1] = static_cast<char>((replacementLength >> 8u) & 0xFFu);
                    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                bytes.begin() + static_cast<std::ptrdiff_t>(offset + from.size()));
                    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(offset), to.begin(), to.end());
                    return true;
                }
            }
            ++match;
        }
        return false;
    }

    bool WriteBytes(const std::filesystem::path& path, const std::vector<char>& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return output.good();
    }

    std::vector<char> ReadBytes(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    template <typename Integer> bool ReadIntegerAt(const std::vector<char>& bytes, size_t& offset, Integer& value)
    {
        if (offset > bytes.size() || sizeof(Integer) > bytes.size() - offset)
            return false;
        std::memcpy(&value, bytes.data() + offset, sizeof(Integer));
        offset += sizeof(Integer);
        return true;
    }

    bool SkipWireString(const std::vector<char>& bytes, size_t& offset)
    {
        uint16_t length = 0;
        if (!ReadIntegerAt(bytes, offset, length) || offset > bytes.size() || length > bytes.size() - offset)
            return false;
        offset += length;
        return true;
    }

    struct SaveWireOffsets
    {
        size_t componentCount = 0;
        size_t firstComponentBegin = 0;
        size_t firstComponentEnd = 0;
        size_t propertyCount = 0;
        size_t firstPropertyBegin = 0;
        size_t firstPropertyEnd = 0;
        size_t customStateCount = 0;
        size_t firstCustomStateBegin = 0;
        size_t firstCustomStateEnd = 0;
    };

    bool LocateFirstSaveRecords(const std::vector<char>& bytes, SaveWireOffsets& locations)
    {
        if (bytes.size() < 12 || std::string(bytes.data(), 4) != "SPRK")
            return false;

        size_t offset = 8;
        uint32_t metadataSize = 0;
        if (!ReadIntegerAt(bytes, offset, metadataSize) || offset > bytes.size() ||
            metadataSize > bytes.size() - offset)
            return false;
        offset += metadataSize;

        uint32_t entityCount = 0;
        if (!ReadIntegerAt(bytes, offset, entityCount) || entityCount == 0)
            return false;

        for (uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex)
        {
            if (!SkipWireString(bytes, offset))
                return false;

            const size_t componentCountOffset = offset;
            uint16_t componentCount = 0;
            if (!ReadIntegerAt(bytes, offset, componentCount))
                return false;
            if (entityIndex == 0)
            {
                if (componentCount == 0)
                    return false;
                locations.componentCount = componentCountOffset;
            }

            for (uint16_t componentIndex = 0; componentIndex < componentCount; ++componentIndex)
            {
                const size_t componentBegin = offset;
                if (!SkipWireString(bytes, offset))
                    return false;

                const size_t propertyCountOffset = offset;
                uint16_t propertyCount = 0;
                if (!ReadIntegerAt(bytes, offset, propertyCount))
                    return false;
                if (entityIndex == 0 && componentIndex == 0)
                {
                    if (propertyCount == 0)
                        return false;
                    locations.firstComponentBegin = componentBegin;
                    locations.propertyCount = propertyCountOffset;
                }

                for (uint16_t propertyIndex = 0; propertyIndex < propertyCount; ++propertyIndex)
                {
                    const size_t propertyBegin = offset;
                    if (!SkipWireString(bytes, offset) || !SkipWireString(bytes, offset))
                        return false;
                    if (entityIndex == 0 && componentIndex == 0 && propertyIndex == 0)
                    {
                        locations.firstPropertyBegin = propertyBegin;
                        locations.firstPropertyEnd = offset;
                    }
                }

                if (entityIndex == 0 && componentIndex == 0)
                    locations.firstComponentEnd = offset;
            }
        }

        locations.customStateCount = offset;
        uint32_t customStateCount = 0;
        if (!ReadIntegerAt(bytes, offset, customStateCount) || customStateCount == 0)
            return false;
        locations.firstCustomStateBegin = offset;
        if (!SkipWireString(bytes, offset) || !SkipWireString(bytes, offset))
            return false;
        locations.firstCustomStateEnd = offset;
        return true;
    }

    template <typename Count>
    bool DuplicateWireRecord(std::vector<char>& bytes, size_t countOffset, size_t begin, size_t end)
    {
        if (begin >= end || end > bytes.size() || countOffset > bytes.size() ||
            sizeof(Count) > bytes.size() - countOffset)
            return false;
        Count count = 0;
        std::memcpy(&count, bytes.data() + countOffset, sizeof(count));
        if (count == std::numeric_limits<Count>::max())
            return false;
        ++count;
        std::memcpy(bytes.data() + countOffset, &count, sizeof(count));
        const std::vector<char> copy(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(end));
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(end), copy.begin(), copy.end());
        return true;
    }

    uint32_t ReadHeaderVersion(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        char magic[4]{};
        uint32_t version = 0;
        input.read(magic, sizeof(magic));
        input.read(reinterpret_cast<char*>(&version), sizeof(version));
        if (!input || std::string(magic, sizeof(magic)) != "SPRK")
            return 0;
        return version;
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

    bool WorldContainsNamedEntity(World& world, const std::string& name)
    {
        return FindNamedEntity(world, name) != entt::null;
    }

    struct SaveLoadLifecycleProbeEvent
    {
        int value = 0;
    };

    struct ThrowingSaveDestroyObserver
    {
        void OnDestroy(entt::registry&, entt::entity) { throw std::runtime_error("intentional destroy observer"); }
    };
} // namespace

TEST(ComponentSerializerRegistry_UnregisterDestroysOwnedCallbacks)
{
    auto& registry = ComponentSerializerRegistry::GetInstance();
    constexpr const char* typeName = "Test.ModuleOwnedSerializer";
    registry.Unregister(typeName);

    auto callbackLifetime = std::make_shared<int>(42);
    const std::weak_ptr<int> callbackLifetimeObserver = callbackLifetime;
    registry.Register(
        typeName,
        [callbackLifetime](const void*)
        {
            (void)callbackLifetime;
            SerializedComponent component;
            component.typeName = "Test.ModuleOwnedSerializer";
            return component;
        },
        [callbackLifetime](World&, EntityID, const SerializedComponent&) { (void)callbackLifetime; });
    callbackLifetime.reset();

    EXPECT_TRUE(registry.HasSerializer(typeName));
    EXPECT_FALSE(callbackLifetimeObserver.expired());
    EXPECT_TRUE(registry.Unregister(typeName));
    EXPECT_FALSE(registry.HasSerializer(typeName));
    EXPECT_TRUE(callbackLifetimeObserver.expired());
    EXPECT_FALSE(registry.Unregister(typeName));
}

TEST(SaveSystem_GetSaveMetadata_ParsesHeader)
{
    const std::string dir = MakeTempSaveDir("valid");
    SaveSystem& ss = SaveSystem::GetInstance();
    ss.SetSaveDirectory(dir);

    WriteSaveHeader(dir + "/goodslot.spark_save", 1u, kValidMeta);

    SaveMetadata meta;
    EXPECT_TRUE(ss.GetSaveMetadata("goodslot", meta));
    EXPECT_EQ(meta.saveName, std::string("My Save"));
    EXPECT_EQ(meta.sceneName, std::string("Level1"));
    EXPECT_EQ(meta.playerClass, std::string("Soldier"));
    EXPECT_EQ(meta.timestamp, static_cast<uint64_t>(1234));
    EXPECT_EQ(meta.version, kCurrentSaveVersion);

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_OnDiskRejectsFutureVersion)
{
    const std::string dir = MakeTempSaveDir("newer");
    SaveSystem& ss = SaveSystem::GetInstance();
    ss.SetSaveDirectory(dir);

    // Version far higher than any supported format: must be refused, not parsed.
    WriteSaveHeader(dir + "/futureslot.spark_save", 999u, kValidMeta);

    SaveMetadata meta;
    EXPECT_FALSE(ss.GetSaveMetadata("futureslot", meta));

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_OnDiskRejectsRetiredVersionTransactionally)
{
    const std::string dir = MakeTempSaveDir("version_zero");
    SaveSystem& ss = SaveSystem::GetInstance();
    ss.SetSaveDirectory(dir);

    WriteSaveHeader(dir + "/zeroslot.spark_save", 0u, kValidMeta);

    SaveMetadata meta;
    meta.saveName = "sentinel";
    meta.version = 77u;
    EXPECT_FALSE(ss.GetSaveMetadata("zeroslot", meta));
    EXPECT_EQ(meta.saveName, std::string("sentinel"));
    EXPECT_EQ(meta.version, 77u);

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_GetSaveMetadata_RejectsBadMagic)
{
    const std::string dir = MakeTempSaveDir("badmagic");
    SaveSystem& ss = SaveSystem::GetInstance();
    ss.SetSaveDirectory(dir);

    {
        std::ofstream out(dir + "/junkslot.spark_save", std::ios::binary | std::ios::trunc);
        const char junk[] = "NOPExxxxxxxx";
        out.write(junk, sizeof(junk) - 1);
    }

    SaveMetadata meta;
    EXPECT_FALSE(ss.GetSaveMetadata("junkslot", meta));

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_Load_RejectsTruncatedCustomStateCountWithoutChangingWorld)
{
    const std::string dir = MakeTempSaveDir("truncated_tail");
    SaveSystem& ss = SaveSystem::GetInstance();
    EXPECT_TRUE(ss.Initialize(dir));

    World source;
    SaveMetadata metadata;
    metadata.saveName = "Tail test";
    EXPECT_TRUE(ss.Save("tailslot", source, metadata));

    const auto path = std::filesystem::path(dir) / "tailslot.spark_save";
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> original((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    EXPECT_TRUE(original.size() >= sizeof(uint32_t));

    for (size_t bytesRemoved = 1; bytesRemoved <= sizeof(uint32_t); ++bytesRemoved)
    {
        std::ofstream truncated(path, std::ios::binary | std::ios::trunc);
        truncated.write(original.data(), static_cast<std::streamsize>(original.size() - bytesRemoved));
        truncated.close();

        World target;
        target.CreateEntity("sentinel");
        EXPECT_FALSE(ss.Load("tailslot", target));
        EXPECT_EQ(target.GetEntityCount(), 1u);
    }

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_Load_RejectsOversizedFileBeforeCacheRead)
{
    const std::string dir = MakeTempSaveDir("oversized_cached");
    SaveSystem& ss = SaveSystem::GetInstance();
    EXPECT_TRUE(ss.Initialize(dir));

    LocalFileCache cache;
    ss.SetFileCache(&cache);
    const auto path = std::filesystem::path(dir) / "oversized.spark_save";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write("SPRK", 4);
    }
    std::filesystem::resize_file(path, 512ull * 1024ull * 1024ull + 1ull);

    World target;
    target.CreateEntity("sentinel");
    EXPECT_FALSE(ss.Load("oversized", target));
    EXPECT_EQ(target.GetEntityCount(), 1u);
    EXPECT_EQ(cache.GetMetrics().misses, uint64_t{0});

    ss.SetFileCache(nullptr);
    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_Load_RejectsEveryTruncatedCustomStateField)
{
    const std::string dir = MakeTempSaveDir("truncated_custom_entry");
    SaveSystem& ss = SaveSystem::GetInstance();
    EXPECT_TRUE(ss.Initialize(dir));

    World source;
    SaveMetadata metadata;
    metadata.saveName = "Custom entry test";
    EXPECT_TRUE(ss.Save("customslot", source, metadata));

    const auto path = std::filesystem::path(dir) / "customslot.spark_save";
    std::ifstream input(path, std::ios::binary);
    std::vector<char> prefix((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    EXPECT_TRUE(prefix.size() >= sizeof(uint32_t));
    prefix.resize(prefix.size() - sizeof(uint32_t));

    auto append16 = [](std::vector<char>& bytes, uint16_t value)
    {
        bytes.push_back(static_cast<char>(value & 0xFF));
        bytes.push_back(static_cast<char>((value >> 8) & 0xFF));
    };
    auto append32 = [](std::vector<char>& bytes, uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<char>((value >> shift) & 0xFF));
    };

    std::vector<std::vector<char>> malformed;
    auto entry = prefix;
    append32(entry, 1);
    malformed.push_back(entry); // missing key length
    append16(entry, 2);
    malformed.push_back(entry); // missing key bytes
    entry.push_back('k');
    malformed.push_back(entry); // partial key bytes
    entry.push_back('2');
    malformed.push_back(entry); // missing value length
    append16(entry, 3);
    malformed.push_back(entry); // missing value bytes
    entry.push_back('v');
    malformed.push_back(entry); // partial value bytes

    for (const auto& bytes : malformed)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();

        World target;
        target.CreateEntity("sentinel");
        EXPECT_FALSE(ss.Load("customslot", target));
        EXPECT_EQ(target.GetEntityCount(), 1u);
    }

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_Save_ReplacesExistingSlotAtomically)
{
    const std::string dir = MakeTempSaveDir("replace_existing");
    SaveSystem& ss = SaveSystem::GetInstance();
    EXPECT_TRUE(ss.Initialize(dir));

    World firstWorld;
    const EntityID firstEntity = firstWorld.CreateEntity("first-only");
    firstWorld.AddComponent<Transform>(firstEntity);
    SaveMetadata firstMetadata;
    firstMetadata.saveName = "First revision";
    EXPECT_TRUE(ss.Save("same-slot", firstWorld, firstMetadata));

    World secondWorld;
    const EntityID secondA = secondWorld.CreateEntity("second-a");
    const EntityID secondB = secondWorld.CreateEntity("second-b");
    secondWorld.AddComponent<Transform>(secondA);
    secondWorld.AddComponent<Transform>(secondB);
    SaveMetadata secondMetadata;
    secondMetadata.saveName = "Second revision";
    EXPECT_TRUE(ss.Save("same-slot", secondWorld, secondMetadata));

    SaveMetadata loadedMetadata;
    EXPECT_TRUE(ss.GetSaveMetadata("same-slot", loadedMetadata));
    EXPECT_EQ(loadedMetadata.saveName, std::string("Second revision"));

    World loadedWorld;
    EXPECT_TRUE(ss.Load("same-slot", loadedWorld));
    EXPECT_EQ(loadedWorld.GetEntityCount(), 2u);

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_CustomState_RoundTripsWithWorldAndDoesNotMutateOutputOnFailure)
{
    const std::string dir = MakeTempSaveDir("custom_state_roundtrip");
    SaveSystem& ss = SaveSystem::GetInstance();
    EXPECT_TRUE(ss.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("custom-state-owner");
    source.AddComponent<Transform>(sourceEntity);
    SaveMetadata metadata;
    metadata.saveName = "Custom state roundtrip";
    const std::unordered_map<std::string, std::string> customState = {
        {"SparkGameRPG.demo.v1", "RPGDEMO 1 state"},
        {"SparkGameARPG.demo.v1", "ARPGDEMO 1 state"},
    };
    EXPECT_TRUE(ss.Save("custom-roundtrip", source, metadata, customState));

    World rejectedWorld;
    rejectedWorld.CreateEntity("validator-sentinel");
    std::unordered_map<std::string, std::string> rejectedCustomState = {{"sentinel", "unchanged"}};
    bool validatorCalled = false;
    EXPECT_FALSE(ss.Load("custom-roundtrip", rejectedWorld, rejectedCustomState,
                         [&](const auto& candidate)
                         {
                             validatorCalled = true;
                             return candidate.contains("missing-required-key");
                         }));
    EXPECT_TRUE(validatorCalled);
    EXPECT_EQ(rejectedWorld.GetEntityCount(), 1u);
    EXPECT_EQ(rejectedCustomState.size(), 1u);
    EXPECT_EQ(rejectedCustomState.at("sentinel"), std::string("unchanged"));

    World loaded;
    std::unordered_map<std::string, std::string> loadedCustomState = {{"sentinel", "unchanged-on-failure"}};
    EXPECT_TRUE(ss.Load("custom-roundtrip", loaded, loadedCustomState));
    EXPECT_EQ(loaded.GetEntityCount(), 1u);
    EXPECT_EQ(loadedCustomState.size(), 2u);
    EXPECT_EQ(loadedCustomState.at("SparkGameRPG.demo.v1"), std::string("RPGDEMO 1 state"));
    EXPECT_EQ(loadedCustomState.at("SparkGameARPG.demo.v1"), std::string("ARPGDEMO 1 state"));

    const auto path = std::filesystem::path(dir) / "custom-roundtrip.spark_save";
    std::filesystem::resize_file(path, 8);
    loadedCustomState = {{"sentinel", "unchanged-on-failure"}};
    EXPECT_FALSE(ss.Load("custom-roundtrip", loaded, loadedCustomState));
    EXPECT_EQ(loadedCustomState.size(), 1u);
    EXPECT_EQ(loadedCustomState.at("sentinel"), std::string("unchanged-on-failure"));

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_CurrentWriterPersistsScreenshotPathAsVersion2)
{
    const std::string dir = MakeTempSaveDir("v2_writer");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID entity = source.CreateEntity("writer-source");
    source.AddComponent<Transform>(entity);

    SaveMetadata metadata;
    metadata.version = kOldestSupportedSaveVersion;
    metadata.saveName = "Version 2 writer";
    metadata.screenshotPath = "Screenshots/version-2.png";
    EXPECT_TRUE(saveSystem.Save("v2-writer", source, metadata));

    const auto savePath = std::filesystem::path(dir) / "v2-writer.spark_save";
    EXPECT_EQ(ReadHeaderVersion(savePath), kCurrentSaveVersion);

    SaveMetadata loadedMetadata;
    EXPECT_TRUE(saveSystem.GetSaveMetadata("v2-writer", loadedMetadata));
    EXPECT_EQ(loadedMetadata.version, kCurrentSaveVersion);
    EXPECT_EQ(loadedMetadata.screenshotPath, std::string("Screenshots/version-2.png"));

    World loadedWorld;
    EXPECT_TRUE(saveSystem.Load("v2-writer", loadedWorld));
    EXPECT_TRUE(WorldContainsNamedEntity(loadedWorld, "writer-source"));

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_SuccessfulLoadPreservesObserversAndRetiresEntitySubscriptions)
{
    const std::string dir = MakeTempSaveDir("lifecycle_commit");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("loaded-renderable");
    auto& sourceRenderer = source.AddComponent<MeshRenderer>(sourceEntity);
    sourceRenderer.meshPath = "Meshes/loaded.mesh";
    sourceRenderer.materialPath = "Materials/loaded.mat";
    sourceRenderer.emissive = 0.375f;
    auto& sourceLight = source.AddComponent<LightComponent>(sourceEntity);
    sourceLight.intensity = 2.5f;
    SaveMetadata metadata;
    metadata.saveName = "Lifecycle-aware restore";
    EXPECT_TRUE(saveSystem.Save("lifecycle", source, metadata, {{"loaded", "state"}}));

    World liveWorld;
    Spark::ECS::MaterialChangeReactiveSystem reactiveSystem;
    Spark::ECS::LightChangeReactiveSystem lightReactiveSystem;
    reactiveSystem.Connect(liveWorld.GetRegistry());
    lightReactiveSystem.Connect(liveWorld.GetRegistry());
    const EntityID retiredEntity = liveWorld.CreateEntity("retired-renderable");
    liveWorld.AddComponent<MeshRenderer>(retiredEntity);
    liveWorld.AddComponent<LightComponent>(retiredEntity);
    reactiveSystem.Update(liveWorld, 0.0f);
    lightReactiveSystem.Update(liveWorld, 0.0f);
    lightReactiveSystem.ResetDirtyCount();
    EXPECT_EQ(reactiveSystem.GetPendingChangeCount(), 0u);
    EXPECT_EQ(lightReactiveSystem.GetPendingChangeCount(), 0u);

    int staleDeliveries = 0;
    auto staleHandle = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(retiredEntity),
        [&](const SaveLoadLifecycleProbeEvent&) { ++staleDeliveries; });
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(retiredEntity)),
              1u);

    entt::registry* const liveRegistry = &liveWorld.GetRegistry();
    std::unordered_map<std::string, std::string> customState;
    EXPECT_TRUE(saveSystem.Load("lifecycle", liveWorld, customState));
    EXPECT_EQ(&liveWorld.GetRegistry(), liveRegistry);
    EXPECT_EQ(customState.at("loaded"), std::string("state"));

    const EntityID loadedEntity = FindNamedEntity(liveWorld, "loaded-renderable");
    ASSERT_TRUE(loadedEntity != entt::null);
    EXPECT_EQ(static_cast<uint32_t>(loadedEntity), static_cast<uint32_t>(retiredEntity));
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(loadedEntity)),
              0u);
    Spark::EntityEventBus::Global().Publish<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(loadedEntity), {1});
    EXPECT_EQ(staleDeliveries, 0);

    // The old-component destruction and explicit incoming-component rebind both
    // reach the pre-existing live observers. The loaded component must not depend
    // on a later manual patch to become visible to reactive consumers.
    EXPECT_EQ(reactiveSystem.GetPendingChangeCount(), 2u);
    EXPECT_EQ(lightReactiveSystem.GetPendingChangeCount(), 2u);
    reactiveSystem.Update(liveWorld, 0.0f);
    lightReactiveSystem.Update(liveWorld, 0.0f);
    EXPECT_EQ(lightReactiveSystem.GetDirtyLightCount(), 1u);
    const LightComponent* loadedLight = liveWorld.GetComponent<LightComponent>(loadedEntity);
    ASSERT_TRUE(loadedLight != nullptr);
    EXPECT_NEAR(loadedLight->intensity, 2.5f, 0.0001f);

    // Observers remain connected for ordinary post-load updates as well.
    liveWorld.GetRegistry().patch<MeshRenderer>(loadedEntity, [](MeshRenderer& renderer) { renderer.visible = false; });
    EXPECT_EQ(reactiveSystem.GetPendingChangeCount(), 1u);
    reactiveSystem.Update(liveWorld, 0.0f);
    EXPECT_EQ(reactiveSystem.GetPendingChangeCount(), 0u);

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_OnDiskRejectsDuplicateRecordsWithoutMutation)
{
    const std::string dir = MakeTempSaveDir("duplicate_records");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("duplicate-source");
    source.AddComponent<Transform>(sourceEntity).position.x = 12.0f;
    SaveMetadata metadata;
    metadata.saveName = "Duplicate structure source";
    EXPECT_TRUE(saveSystem.Save("duplicate-records", source, metadata, {{"state.key", "value"}}));

    const auto path = std::filesystem::path(dir) / "duplicate-records.spark_save";
    const std::vector<char> original = ReadBytes(path);
    SaveWireOffsets locations;
    ASSERT_TRUE(LocateFirstSaveRecords(original, locations));

    std::vector<std::vector<char>> malformedCases;
    {
        auto bytes = original;
        ASSERT_TRUE(DuplicateWireRecord<uint16_t>(bytes, locations.componentCount, locations.firstComponentBegin,
                                                  locations.firstComponentEnd));
        malformedCases.push_back(std::move(bytes));
    }
    {
        auto bytes = original;
        ASSERT_TRUE(DuplicateWireRecord<uint16_t>(bytes, locations.propertyCount, locations.firstPropertyBegin,
                                                  locations.firstPropertyEnd));
        malformedCases.push_back(std::move(bytes));
    }
    {
        auto bytes = original;
        ASSERT_TRUE(DuplicateWireRecord<uint32_t>(bytes, locations.customStateCount, locations.firstCustomStateBegin,
                                                  locations.firstCustomStateEnd));
        malformedCases.push_back(std::move(bytes));
    }
    {
        auto bytes = original;
        ASSERT_TRUE(ReplaceLengthPrefixedString(bytes, "Transform", "NameComponent"));
        malformedCases.push_back(std::move(bytes));
    }

    for (const auto& malformed : malformedCases)
    {
        ASSERT_TRUE(WriteBytes(path, malformed));
        World liveWorld;
        const EntityID sentinel = liveWorld.CreateEntity("duplicate-live-sentinel");
        liveWorld.AddComponent<Transform>(sentinel).position.x = 808.0f;
        int deliveries = 0;
        auto subscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
            static_cast<Spark::EventEntityID>(sentinel), [&](const SaveLoadLifecycleProbeEvent&) { ++deliveries; });
        std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};

        EXPECT_FALSE(saveSystem.Load("duplicate-records", liveWorld, customState));
        EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
        EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "duplicate-live-sentinel"));
        EXPECT_NEAR(liveWorld.GetComponent<Transform>(sentinel)->position.x, 808.0f, 0.0001f);
        EXPECT_EQ(customState.size(), 1u);
        EXPECT_EQ(customState.at("live"), std::string("sentinel"));
        EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                      static_cast<Spark::EventEntityID>(sentinel)),
                  1u);
        Spark::EntityEventBus::Global().Publish<SaveLoadLifecycleProbeEvent>(
            static_cast<Spark::EventEntityID>(sentinel), {1});
        EXPECT_EQ(deliveries, 1);
    }

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_InMemoryRejectsDuplicateAndExplicitNameComponentsBeforeLifecycle)
{
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    World source;
    const EntityID sourceEntity = source.CreateEntity("memory-duplicate-source");
    source.AddComponent<Transform>(sourceEntity).position.x = 33.0f;
    SaveMetadata metadata;
    metadata.saveName = "In-memory duplicate";
    const SaveData original = saveSystem.SerializeWorld(source, metadata);
    ASSERT_EQ(original.entities.size(), 1u);
    ASSERT_EQ(original.entities.front().components.size(), 1u);

    SaveData duplicateComponent = original;
    duplicateComponent.entities.front().components.push_back(duplicateComponent.entities.front().components.front());
    SaveData explicitName = original;
    explicitName.entities.front().components.push_back(SerializedComponent{"NameComponent", {{"name", "shadow-name"}}});

    for (const SaveData* malformed : {&duplicateComponent, &explicitName})
    {
        World liveWorld;
        const EntityID sentinel = liveWorld.CreateEntity("memory-live-sentinel");
        liveWorld.AddComponent<Transform>(sentinel).position.x = 909.0f;
        int deliveries = 0;
        auto subscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
            static_cast<Spark::EventEntityID>(sentinel), [&](const SaveLoadLifecycleProbeEvent&) { ++deliveries; });

        EXPECT_FALSE(saveSystem.DeserializeWorld(*malformed, liveWorld));
        EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
        EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "memory-live-sentinel"));
        EXPECT_NEAR(liveWorld.GetComponent<Transform>(sentinel)->position.x, 909.0f, 0.0001f);
        EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                      static_cast<Spark::EventEntityID>(sentinel)),
                  1u);
        Spark::EntityEventBus::Global().Publish<SaveLoadLifecycleProbeEvent>(
            static_cast<Spark::EventEntityID>(sentinel), {1});
        EXPECT_EQ(deliveries, 1);
    }
}

TEST(SaveMigration_LifecycleObserverExceptionPropagatesInsteadOfReturningFalse)
{
    const std::string dir = MakeTempSaveDir("throwing_destroy_observer");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("incoming-after-observer");
    source.AddComponent<Transform>(sourceEntity);
    SaveMetadata metadata;
    metadata.saveName = "Throwing observer";
    EXPECT_TRUE(saveSystem.Save("throwing-observer", source, metadata, {{"candidate", "state"}}));

    World liveWorld;
    const EntityID retiredEntity = liveWorld.CreateEntity("observer-live-sentinel");
    liveWorld.AddComponent<Transform>(retiredEntity);
    ThrowingSaveDestroyObserver observer;
    liveWorld.GetRegistry().on_destroy<Transform>().connect<&ThrowingSaveDestroyObserver::OnDestroy>(observer);
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};

    EXPECT_THROW(saveSystem.Load("throwing-observer", liveWorld, customState), std::runtime_error);
    liveWorld.GetRegistry().on_destroy<Transform>().disconnect<&ThrowingSaveDestroyObserver::OnDestroy>(observer);
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("live"), std::string("sentinel"));

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_HandWrittenComponentSemanticFailuresRollbackWorldAndCustomState)
{
    const std::string dir = MakeTempSaveDir("strict_handwritten");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("strict-transform-source");
    auto& sourceTransform = source.AddComponent<Transform>(sourceEntity);
    sourceTransform.position.x = 12.5f;
    SaveMetadata metadata;
    metadata.saveName = "Strict hand-written component";
    EXPECT_TRUE(saveSystem.Save("strict-transform", source, metadata, {{"candidate", "state"}}));

    const auto path = std::filesystem::path(dir) / "strict-transform.spark_save";
    const std::vector<char> original = ReadBytes(path);
    ASSERT_TRUE(!original.empty());

    for (int failureCase = 0; failureCase < 3; ++failureCase)
    {
        std::vector<char> malformed = original;
        if (failureCase == 0)
            ASSERT_TRUE(ReplaceLengthPrefixedString(malformed, "px", "qx"));
        else if (failureCase == 1)
            ASSERT_TRUE(ReplaceLengthPrefixedString(malformed, "12.500000", "not-float"));
        else
            ASSERT_TRUE(ReplaceLengthPrefixedString(malformed, "12.500000",
                                                    std::string(std::numeric_limits<uint16_t>::max(), '9')));
        ASSERT_TRUE(WriteBytes(path, malformed));

        World liveWorld;
        const EntityID sentinel = liveWorld.CreateEntity("handwritten-live-sentinel");
        auto& sentinelTransform = liveWorld.AddComponent<Transform>(sentinel);
        sentinelTransform.position = {444.0f, 555.0f, 666.0f};
        std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};

        EXPECT_FALSE(saveSystem.Load("strict-transform", liveWorld, customState));
        EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
        EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "handwritten-live-sentinel"));
        EXPECT_NEAR(liveWorld.GetComponent<Transform>(sentinel)->position.x, 444.0f, 0.0001f);
        EXPECT_EQ(customState.size(), 1u);
        EXPECT_EQ(customState.at("live"), std::string("sentinel"));
    }

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_ReflectedSetFieldFailureRollsBackWorldAndCustomState)
{
    const std::string dir = MakeTempSaveDir("strict_reflected");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("strict-reflected-source");
    auto& sourceRenderer = source.AddComponent<MeshRenderer>(sourceEntity);
    sourceRenderer.meshPath = "Meshes/reflected.mesh";
    sourceRenderer.materialPath = "Materials/reflected.mat";
    sourceRenderer.emissive = 0.625f;
    SaveMetadata metadata;
    metadata.saveName = "Strict reflected component";
    EXPECT_TRUE(saveSystem.Save("strict-reflected", source, metadata, {{"candidate", "state"}}));

    const auto path = std::filesystem::path(dir) / "strict-reflected.spark_save";
    std::vector<char> malformed = ReadBytes(path);
    ASSERT_TRUE(ReplaceLengthPrefixedString(malformed, "0.625000", "badfloat"));
    ASSERT_TRUE(WriteBytes(path, malformed));

    World liveWorld;
    const EntityID sentinel = liveWorld.CreateEntity("reflected-live-sentinel");
    auto& sentinelTransform = liveWorld.AddComponent<Transform>(sentinel);
    sentinelTransform.position.x = 777.0f;
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};

    EXPECT_FALSE(saveSystem.Load("strict-reflected", liveWorld, customState));
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "reflected-live-sentinel"));
    EXPECT_NEAR(liveWorld.GetComponent<Transform>(sentinel)->position.x, 777.0f, 0.0001f);
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("live"), std::string("sentinel"));

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_V1ToV2InMemoryStepIsExactIdempotentAndTransactional)
{
    SaveData legacy;
    legacy.metadata.version = kOldestSupportedSaveVersion;
    legacy.metadata.saveName = "Legacy memory snapshot";
    legacy.metadata.screenshotPath = "not-representable-in-v1.png";
    legacy.customState["declared"] = "preserved";

    EXPECT_TRUE(SaveSystem::MigrateToCurrentVersion(legacy));
    EXPECT_EQ(legacy.metadata.version, kCurrentSaveVersion);
    EXPECT_EQ(legacy.metadata.screenshotPath, std::string());
    EXPECT_EQ(legacy.metadata.saveName, std::string("Legacy memory snapshot"));
    EXPECT_EQ(legacy.customState.at("declared"), std::string("preserved"));

    const SaveData onceMigrated = legacy;
    EXPECT_TRUE(SaveSystem::MigrateToCurrentVersion(legacy));
    EXPECT_EQ(legacy.metadata.version, onceMigrated.metadata.version);
    EXPECT_EQ(legacy.metadata.screenshotPath, onceMigrated.metadata.screenshotPath);
    EXPECT_EQ(legacy.metadata.saveName, onceMigrated.metadata.saveName);
    EXPECT_EQ(legacy.customState.size(), onceMigrated.customState.size());
    EXPECT_EQ(legacy.customState.at("declared"), onceMigrated.customState.at("declared"));

    SaveData unsupported = onceMigrated;
    unsupported.metadata.version = kCurrentSaveVersion + 1;
    unsupported.metadata.saveName = "future-sentinel";
    EXPECT_FALSE(SaveSystem::MigrateToCurrentVersion(unsupported));
    EXPECT_EQ(unsupported.metadata.version, kCurrentSaveVersion + 1);
    EXPECT_EQ(unsupported.metadata.saveName, std::string("future-sentinel"));
}

TEST(SaveMigration_ImmutableV1FixtureLoadsWithoutRewritingSourceOrSlot)
{
    const auto fixturePath = std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "Tests" / "Fixtures" / "Compatibility" /
                             "SaveSystem" / "v1-screenshotless.spark_save.hex";
    const std::string fixtureBefore = ReadTextFile(fixturePath);
    const std::vector<char> legacyBytes = DecodeHexFixture(fixtureBefore);
    ASSERT_EQ(legacyBytes.size(), static_cast<size_t>(284));

    const std::string dir = MakeTempSaveDir("v1_fixture");
    const auto slotPath = std::filesystem::path(dir) / "legacy-v1.spark_save";
    ASSERT_TRUE(WriteBytes(slotPath, legacyBytes));

    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));
    EXPECT_EQ(ReadHeaderVersion(slotPath), kOldestSupportedSaveVersion);

    SaveMetadata metadata;
    EXPECT_TRUE(saveSystem.GetSaveMetadata("legacy-v1", metadata));
    EXPECT_EQ(metadata.version, kCurrentSaveVersion);
    EXPECT_EQ(metadata.saveName, std::string("Legacy screenshotless save"));
    EXPECT_EQ(metadata.sceneName, std::string("LegacyScene"));
    EXPECT_EQ(metadata.playerClass, std::string("Ranger"));
    EXPECT_EQ(metadata.screenshotPath, std::string());
    EXPECT_EQ(metadata.timestamp, uint64_t{1700000000});
    EXPECT_NEAR(metadata.playTime, 42.5f, 0.0001f);
    EXPECT_NEAR(metadata.playerHealth, 75.0f, 0.0001f);
    EXPECT_NEAR(metadata.playerArmor, 25.0f, 0.0001f);
    EXPECT_NEAR(metadata.playerPosition.x, 1.0f, 0.0001f);
    EXPECT_NEAR(metadata.playerPosition.y, 2.0f, 0.0001f);
    EXPECT_NEAR(metadata.playerPosition.z, 3.0f, 0.0001f);
    EXPECT_EQ(metadata.playerKills, 4u);
    EXPECT_EQ(metadata.playerDeaths, 1u);

    World loadedWorld;
    loadedWorld.CreateEntity("must-be-replaced-only-on-success");
    std::unordered_map<std::string, std::string> customState = {{"sentinel", "replace-on-success"}};
    EXPECT_TRUE(saveSystem.Load("legacy-v1", loadedWorld, customState));
    EXPECT_EQ(loadedWorld.GetEntityCount(), 1u);
    const EntityID legacyPlayer = FindNamedEntity(loadedWorld, "legacy-player");
    ASSERT_TRUE(legacyPlayer != entt::null);
    const Transform* transform = loadedWorld.GetComponent<Transform>(legacyPlayer);
    ASSERT_TRUE(transform != nullptr);
    EXPECT_NEAR(transform->position.x, 12.5f, 0.0001f);
    EXPECT_NEAR(transform->position.y, -3.25f, 0.0001f);
    EXPECT_NEAR(transform->position.z, 99.75f, 0.0001f);
    EXPECT_NEAR(transform->rotation.x, 0.125f, 0.0001f);
    EXPECT_NEAR(transform->rotation.y, 1.5f, 0.0001f);
    EXPECT_NEAR(transform->rotation.z, -2.25f, 0.0001f);
    EXPECT_NEAR(transform->scale.x, 2.0f, 0.0001f);
    EXPECT_NEAR(transform->scale.y, 0.5f, 0.0001f);
    EXPECT_NEAR(transform->scale.z, 3.75f, 0.0001f);
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("legacy.key"), std::string("legacy-value"));

    // Migration is in memory only: neither the checked-in fixture nor the copied
    // N-1 slot is rewritten as a side effect of reading it.
    EXPECT_EQ(ReadHeaderVersion(slotPath), kOldestSupportedSaveVersion);
    EXPECT_TRUE(ReadBytes(slotPath) == legacyBytes);
    EXPECT_EQ(ReadTextFile(fixturePath), fixtureBefore);

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_UnknownComponentFailsWithoutMutatingWorldOrCustomState)
{
    const std::string dir = MakeTempSaveDir("unknown_component");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("candidate-only");
    source.AddComponent<Transform>(sourceEntity);
    SaveMetadata metadata;
    metadata.saveName = "Unknown component transaction";
    EXPECT_TRUE(saveSystem.Save("unknown-component", source, metadata, {{"candidate", "state"}}));

    const auto path = std::filesystem::path(dir) / "unknown-component.spark_save";
    std::vector<char> bytes = ReadBytes(path);
    ASSERT_TRUE(ReplaceFirstAscii(bytes, "Transform", "NoSuchCmp"));
    ASSERT_TRUE(WriteBytes(path, bytes));

    World liveWorld;
    liveWorld.CreateEntity("live-sentinel");
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};
    EXPECT_FALSE(saveSystem.Load("unknown-component", liveWorld, customState));
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "live-sentinel"));
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("live"), std::string("sentinel"));

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_ThrowingDeserializerFailsWithoutMutatingWorldOrCustomState)
{
    const std::string dir = MakeTempSaveDir("throwing_deserializer");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("candidate-only");
    source.AddComponent<Transform>(sourceEntity);
    SaveMetadata metadata;
    metadata.saveName = "Throwing component transaction";
    EXPECT_TRUE(saveSystem.Save("throwing-component", source, metadata, {{"candidate", "state"}}));

    const auto path = std::filesystem::path(dir) / "throwing-component.spark_save";
    std::vector<char> bytes = ReadBytes(path);
    ASSERT_TRUE(ReplaceFirstAscii(bytes, "Transform", "ThrowTest"));
    ASSERT_TRUE(WriteBytes(path, bytes));

    auto& registry = ComponentSerializerRegistry::GetInstance();
    registry.Unregister("ThrowTest");
    bool deserializerCalled = false;
    registry.Register(
        "ThrowTest", [](const void*) { return SerializedComponent{"ThrowTest", {}}; },
        [&](World&, EntityID, const SerializedComponent&)
        {
            deserializerCalled = true;
            throw std::runtime_error("intentional compatibility fixture failure");
        });

    World liveWorld;
    liveWorld.CreateEntity("live-sentinel");
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};
    const bool loaded = saveSystem.Load("throwing-component", liveWorld, customState);
    registry.Unregister("ThrowTest");

    EXPECT_FALSE(loaded);
    EXPECT_TRUE(deserializerCalled);
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "live-sentinel"));
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("live"), std::string("sentinel"));

    std::filesystem::remove_all(dir);
}
