// Test_persistence_SaveSystem.cpp
// Regression for three SaveSystem findings:
//   P1: Load/ReadFromFile never validated the save-format version. A file written by a
//       newer, incompatible format is now rejected instead of silently misinterpreted.
//   P2: GetSaveMetadata now uses a metadata-only read path; this test also confirms it
//       still parses the metadata header correctly.
//   P3: DeleteSave must evict primary and retained-copy entries from LocalFileCache so
//       a deleted slot cannot be resurrected from stale cached bytes.
// P1 and P2 are exercised through the public GetSaveMetadata() (which needs no World/ECS),
// by hand-crafting .spark_save files with the real on-disk binary layout. P3 uses the
// production-linked Save/Load/Delete path with an attached LocalFileCache.

#include "TestFramework.h"
#include "Core/Reflection.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Game/FPSLocalProfile.h"
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

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

using namespace Spark;

namespace
{
#if !defined(_WIN32)
    class ScopedMinimumFileSizeLimit
    {
      public:
        explicit ScopedMinimumFileSizeLimit(uint64_t minimumBytes) noexcept
        {
            if (getrlimit(RLIMIT_FSIZE, &m_original) != 0)
                return;
            const rlim_t minimum = static_cast<rlim_t>(minimumBytes);
            if (m_original.rlim_cur == RLIM_INFINITY || m_original.rlim_cur >= minimum)
            {
                m_ready = true;
                return;
            }
            if (m_original.rlim_max != RLIM_INFINITY && m_original.rlim_max < minimum)
                return;
            rlimit raised = m_original;
            raised.rlim_cur = minimum;
            if (setrlimit(RLIMIT_FSIZE, &raised) == 0)
            {
                m_changed = true;
                m_ready = true;
            }
        }

        ~ScopedMinimumFileSizeLimit()
        {
            if (m_changed)
                (void)setrlimit(RLIMIT_FSIZE, &m_original);
        }

        bool Ready() const noexcept { return m_ready; }

      private:
        rlimit m_original{};
        bool m_changed = false;
        bool m_ready = false;
    };
#endif

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
    const std::string kValidMeta = "My Save\nLevel1\nSoldier\nScreenshots/good.png\n1234 56.5 100 50 1 2 3 4 5\n";

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

    uint32_t ReadLittleEndian32(const char* bytes)
    {
        return static_cast<uint32_t>(static_cast<uint8_t>(bytes[0])) |
               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[1])) << 8u) |
               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[2])) << 16u) |
               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[3])) << 24u);
    }

    void WriteLittleEndian32(char* bytes, uint32_t value)
    {
        bytes[0] = static_cast<char>(value & 0xFFu);
        bytes[1] = static_cast<char>((value >> 8u) & 0xFFu);
        bytes[2] = static_cast<char>((value >> 16u) & 0xFFu);
        bytes[3] = static_cast<char>((value >> 24u) & 0xFFu);
    }

    uint32_t ComputeFixtureCRC32(const char* bytes, size_t size)
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t index = 0; index < size; ++index)
        {
            crc ^= static_cast<uint8_t>(bytes[index]);
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc & 1u) != 0u ? (crc >> 1u) ^ 0xEDB88320u : crc >> 1u;
        }
        return ~crc;
    }

    bool HasValidV4Checksum(const std::vector<char>& bytes)
    {
        constexpr size_t headerBytes = 4u + sizeof(uint32_t);
        constexpr size_t checksumBytes = sizeof(uint32_t);
        if (bytes.size() < headerBytes + checksumBytes || std::string(bytes.data(), 4) != "SPRK" ||
            ReadLittleEndian32(bytes.data() + 4) != 4u)
        {
            return false;
        }
        const size_t checksumOffset = bytes.size() - checksumBytes;
        return ReadLittleEndian32(bytes.data() + checksumOffset) == ComputeFixtureCRC32(bytes.data(), checksumOffset);
    }

    bool RefreshV4Checksum(std::vector<char>& bytes)
    {
        constexpr size_t headerBytes = 4u + sizeof(uint32_t);
        constexpr size_t checksumBytes = sizeof(uint32_t);
        if (bytes.size() < headerBytes || std::string(bytes.data(), 4) != "SPRK")
            return false;
        if (ReadLittleEndian32(bytes.data() + 4) < 4u)
            return true;
        if (bytes.size() < headerBytes + checksumBytes)
            return false;
        const size_t checksumOffset = bytes.size() - checksumBytes;
        WriteLittleEndian32(bytes.data() + checksumOffset, ComputeFixtureCRC32(bytes.data(), checksumOffset));
        return true;
    }

    bool AppendV4Checksum(std::vector<char>& bytes)
    {
        if (bytes.size() < 8u || std::string(bytes.data(), 4) != "SPRK" || ReadLittleEndian32(bytes.data() + 4) != 4u)
        {
            return false;
        }
        const size_t checksumOffset = bytes.size();
        bytes.resize(bytes.size() + sizeof(uint32_t));
        WriteLittleEndian32(bytes.data() + checksumOffset, ComputeFixtureCRC32(bytes.data(), checksumOffset));
        return true;
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

    struct MissingRestoreProbe
    {
        uint8_t marker = 0;
    };

    struct PrepareFailureProbe
    {
        uint8_t marker = 0;
    };

    struct CandidateGhostProbe
    {
        uint8_t marker = 0;
    };

    struct CandidateExtraComponentProbe
    {
        uint8_t marker = 0;
    };

    struct RetirementPlanProbe
    {
        uint8_t marker = 0;
    };

    struct TransientCandidateProbe
    {
        uint8_t marker = 0;
    };

    struct AllocatorCursorProbe
    {
        uint8_t marker = 0;
    };

    int g_missingRestorePrepareCalls = 0;
    int g_prepareFailureCalls = 0;
    int g_candidateTopologyPrepareCalls = 0;
    int g_retirementPrepareCalls = 0;
    int g_retirementSnapshotCalls = 0;
    int g_transientCandidatePrepareCalls = 0;
    int g_allocatorCursorPrepareCalls = 0;
    EntityID g_transientCandidateGhost = entt::null;
    bool g_throwRetirementSnapshot = false;
    bool g_liveStorageBoundaryCrossed = false;
    bool g_snapshotObservedAfterBoundary = false;

    template <typename Type> void SwapProbeStorageContents(void* destinationWorld, void* sourceWorld) noexcept
    {
        using PayloadStorage = entt::basic_storage<Type>;
        auto& destination = static_cast<World*>(destinationWorld)->GetRegistry().storage<Type>();
        auto& source = static_cast<World*>(sourceWorld)->GetRegistry().storage<Type>();
        static_cast<PayloadStorage&>(destination).swap(static_cast<PayloadStorage&>(source));
    }

    template <typename Type> void NotifyProbeRebound(void* world, uint32_t entity)
    {
        auto& registry = static_cast<World*>(world)->GetRegistry();
        const EntityID entityID = static_cast<EntityID>(entity);
        if (registry.valid(entityID) && registry.all_of<Type>(entityID))
            registry.patch<Type>(entityID, [](Type&) noexcept {});
    }

    void CountMissingRestorePrepare(void* world)
    {
        ++g_missingRestorePrepareCalls;
        (void)static_cast<World*>(world)->GetRegistry().storage<MissingRestoreProbe>();
    }

    void ThrowAfterPreparingLiveStorage(void* world)
    {
        ++g_prepareFailureCalls;
        (void)static_cast<World*>(world)->GetRegistry().storage<PrepareFailureProbe>();
        throw std::runtime_error("intentional live storage preparation failure");
    }

    template <typename Type> void CountCandidateTopologyPrepare(void* world)
    {
        ++g_candidateTopologyPrepareCalls;
        (void)static_cast<World*>(world)->GetRegistry().storage<Type>();
    }

    void CountRetirementPrepare(void* world)
    {
        ++g_retirementPrepareCalls;
        (void)static_cast<World*>(world)->GetRegistry().storage<RetirementPlanProbe>();
    }

    void MarkLiveStorageBoundary(void* world)
    {
        ++g_retirementPrepareCalls;
        g_liveStorageBoundaryCrossed = true;
        (void)static_cast<World*>(world)->GetRegistry().storage<RetirementPlanProbe>();
    }

    void CountTransientCandidatePrepare(void* world)
    {
        ++g_transientCandidatePrepareCalls;
        (void)static_cast<World*>(world)->GetRegistry().storage<TransientCandidateProbe>();
    }

    void CountAllocatorCursorPrepare(void* world)
    {
        ++g_allocatorCursorPrepareCalls;
        (void)static_cast<World*>(world)->GetRegistry().storage<AllocatorCursorProbe>();
    }

    void ObserveRetirementSnapshot(EntityID)
    {
        ++g_retirementSnapshotCalls;
        if (g_liveStorageBoundaryCrossed)
            g_snapshotObservedAfterBoundary = true;
        if (g_throwRetirementSnapshot)
            throw std::runtime_error("intentional retirement-plan snapshot failure");
    }

    template <typename Type> ComponentOps MakeProbeComponentOps(ComponentOps::PrepareStorageFn prepareStorage)
    {
        ComponentOps operations;
        operations.add = [](void* world, uint32_t entity)
        { static_cast<World*>(world)->AddComponent<Type>(static_cast<EntityID>(entity)); };
        operations.has = [](void* world, uint32_t entity)
        { return static_cast<World*>(world)->HasComponent<Type>(static_cast<EntityID>(entity)); };
        operations.remove = [](void* world, uint32_t entity)
        { static_cast<World*>(world)->RemoveComponent<Type>(static_cast<EntityID>(entity)); };
        operations.getRaw = [](void* world, uint32_t entity) -> void*
        { return static_cast<World*>(world)->GetComponent<Type>(static_cast<EntityID>(entity)); };
        operations.prepareStorage = prepareStorage;
        operations.swapStorageContents = &SwapProbeStorageContents<Type>;
        operations.notifyRebound = &NotifyProbeRebound<Type>;
        return operations;
    }

    std::vector<entt::id_type> RegistryStorageIds(const World& world)
    {
        std::vector<entt::id_type> ids;
        for (const auto& [id, storage] : world.GetRegistry().storage())
        {
            (void)storage;
            ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }
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

    // N-1 (v3) has no CRC trailer, so a bare header plus metadata block is a
    // complete metadata record for the metadata-only reader.
    WriteSaveHeader(dir + "/goodslot.spark_save", kOldestSupportedSaveVersion, kValidMeta);

    SaveMetadata meta;
    EXPECT_TRUE(ss.GetSaveMetadata("goodslot", meta));
    EXPECT_EQ(meta.saveName, std::string("My Save"));
    EXPECT_EQ(meta.sceneName, std::string("Level1"));
    EXPECT_EQ(meta.playerClass, std::string("Soldier"));
    EXPECT_EQ(meta.screenshotPath, std::string("Screenshots/good.png"));
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

TEST(SaveSystem_Save_WritesV4ChecksumTrailerAndRoundTrips)
{
    const std::string dir = MakeTempSaveDir("v4_checksum_roundtrip");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    ASSERT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("integrity-owner");
    source.AddComponent<Transform>(sourceEntity).position.x = 19.5f;
    SaveMetadata metadata;
    metadata.saveName = "Integrity roundtrip";
    const std::unordered_map<std::string, std::string> sourceState = {{"integrity.key", "integrity-value"}};
    ASSERT_TRUE(saveSystem.Save("integrity", source, metadata, sourceState));

    const auto path = std::filesystem::path(dir) / "integrity.spark_save";
    const std::vector<char> bytes = ReadBytes(path);
    EXPECT_EQ(ReadHeaderVersion(path), 4u);
    EXPECT_TRUE(HasValidV4Checksum(bytes));

    World loaded;
    std::unordered_map<std::string, std::string> loadedState;
    ASSERT_TRUE(saveSystem.Load("integrity", loaded, loadedState));
    const EntityID loadedEntity = FindNamedEntity(loaded, "integrity-owner");
    ASSERT_TRUE(loadedEntity != entt::null);
    ASSERT_TRUE(loaded.GetComponent<Transform>(loadedEntity) != nullptr);
    EXPECT_NEAR(loaded.GetComponent<Transform>(loadedEntity)->position.x, 19.5f, 0.0001f);
    EXPECT_EQ(loadedState.at("integrity.key"), std::string("integrity-value"));

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_Load_RejectsV4PayloadCorruptionTransactionally)
{
    const std::string dir = MakeTempSaveDir("v4_payload_corruption");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    ASSERT_TRUE(saveSystem.Initialize(dir));

    World source;
    source.AddComponent<Transform>(source.CreateEntity("candidate-only"));
    SaveMetadata metadata;
    metadata.saveName = "Integrity payload";
    ASSERT_TRUE(saveSystem.Save("corrupt-payload", source, metadata, {{"candidate", "state"}}));

    const auto path = std::filesystem::path(dir) / "corrupt-payload.spark_save";
    std::vector<char> bytes = ReadBytes(path);
    ASSERT_TRUE(ReplaceFirstAscii(bytes, "Integrity payload", "Jntegrity payload"));
    ASSERT_TRUE(WriteBytes(path, bytes));

    SaveMetadata unchangedMetadata;
    unchangedMetadata.saveName = "metadata-sentinel";
    EXPECT_FALSE(saveSystem.GetSaveMetadata("corrupt-payload", unchangedMetadata));
    EXPECT_EQ(unchangedMetadata.saveName, std::string("metadata-sentinel"));

    World liveWorld;
    const EntityID sentinel = liveWorld.CreateEntity("live-sentinel");
    liveWorld.AddComponent<Transform>(sentinel).position.x = 77.0f;
    std::unordered_map<std::string, std::string> liveState = {{"live", "sentinel"}};
    EXPECT_FALSE(saveSystem.Load("corrupt-payload", liveWorld, liveState));
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "live-sentinel"));
    EXPECT_NEAR(liveWorld.GetComponent<Transform>(sentinel)->position.x, 77.0f, 0.0001f);
    EXPECT_EQ(liveState.size(), 1u);
    EXPECT_EQ(liveState.at("live"), std::string("sentinel"));

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_MetadataAndLoadRejectV4TrailerAndVersionCorruption)
{
    const std::string dir = MakeTempSaveDir("v4_envelope_corruption");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    ASSERT_TRUE(saveSystem.Initialize(dir));

    World source;
    source.AddComponent<Transform>(source.CreateEntity("envelope-candidate"));
    SaveMetadata metadata;
    metadata.saveName = "Envelope integrity";
    ASSERT_TRUE(saveSystem.Save("envelope", source, metadata));

    const auto path = std::filesystem::path(dir) / "envelope.spark_save";
    const std::vector<char> original = ReadBytes(path);
    ASSERT_TRUE(HasValidV4Checksum(original));

    for (int failureCase = 0; failureCase < 4; ++failureCase)
    {
        std::vector<char> corrupted = original;
        if (failureCase == 0)
            corrupted.back() ^= static_cast<char>(0x40);
        else
            corrupted[4] = static_cast<char>(failureCase);
        ASSERT_TRUE(WriteBytes(path, corrupted));

        SaveMetadata unchanged;
        unchanged.saveName = "metadata-sentinel";
        EXPECT_FALSE(saveSystem.GetSaveMetadata("envelope", unchanged));
        EXPECT_EQ(unchanged.saveName, std::string("metadata-sentinel"));

        World liveWorld;
        liveWorld.AddComponent<Transform>(liveWorld.CreateEntity("live-sentinel"));
        EXPECT_FALSE(saveSystem.Load("envelope", liveWorld));
        EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
        EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "live-sentinel"));
    }

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_Load_RejectsEveryTruncatedV4ChecksumTrailer)
{
    const std::string dir = MakeTempSaveDir("v4_truncated_trailer");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    ASSERT_TRUE(saveSystem.Initialize(dir));

    World source;
    source.AddComponent<Transform>(source.CreateEntity("trailer-candidate"));
    SaveMetadata metadata;
    metadata.saveName = "Trailer truncation";
    ASSERT_TRUE(saveSystem.Save("truncated-trailer", source, metadata));

    const auto path = std::filesystem::path(dir) / "truncated-trailer.spark_save";
    const std::vector<char> original = ReadBytes(path);
    ASSERT_TRUE(HasValidV4Checksum(original));
    for (size_t bytesRemoved = 1; bytesRemoved <= sizeof(uint32_t); ++bytesRemoved)
    {
        const std::vector<char> truncated(original.begin(), original.end() - static_cast<std::ptrdiff_t>(bytesRemoved));
        ASSERT_TRUE(WriteBytes(path, truncated));

        World liveWorld;
        liveWorld.AddComponent<Transform>(liveWorld.CreateEntity("trailer-live-sentinel"));
        EXPECT_FALSE(saveSystem.Load("truncated-trailer", liveWorld));
        EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
        EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "trailer-live-sentinel"));
    }

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_Load_DoesNotLetCachedV4SnapshotHideExternalCorruption)
{
    const std::string dir = MakeTempSaveDir("v4_cache_freshness");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(dir));

    LocalFileCache cache;
    saveSystem.SetFileCache(&cache);
    World source;
    source.AddComponent<Transform>(source.CreateEntity("cached-candidate"));
    SaveMetadata metadata;
    metadata.saveName = "Cached integrity";
    ASSERT_TRUE(saveSystem.Save("cached-integrity", source, metadata));

    World firstLoad;
    ASSERT_TRUE(saveSystem.Load("cached-integrity", firstLoad));
    const auto path = std::filesystem::path(dir) / "cached-integrity.spark_save";
    ASSERT_TRUE(cache.Contains(path.string()));

    std::vector<char> corrupted = ReadBytes(path);
    ASSERT_TRUE(ReplaceFirstAscii(corrupted, "Cached integrity", "Xached integrity"));
    ASSERT_TRUE(WriteBytes(path, corrupted));

    World liveWorld;
    liveWorld.AddComponent<Transform>(liveWorld.CreateEntity("cache-live-sentinel"));
    EXPECT_FALSE(saveSystem.Load("cached-integrity", liveWorld));
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "cache-live-sentinel"));

    saveSystem.SetFileCache(nullptr);
    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_Load_RecoversValidBackupWhenV4PrimaryChecksumFails)
{
    const std::string dir = MakeTempSaveDir("v4_checksum_backup");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    ASSERT_TRUE(saveSystem.Initialize(dir));

    World first;
    first.AddComponent<Transform>(first.CreateEntity("first-integrity-revision"));
    SaveMetadata firstMetadata;
    firstMetadata.saveName = "First integrity revision";
    ASSERT_TRUE(saveSystem.Save("integrity-backup", first, firstMetadata));

    World second;
    second.AddComponent<Transform>(second.CreateEntity("second-integrity-revision"));
    SaveMetadata secondMetadata;
    secondMetadata.saveName = "Second integrity revision";
    ASSERT_TRUE(saveSystem.Save("integrity-backup", second, secondMetadata));

    const auto primaryPath = std::filesystem::path(dir) / "integrity-backup.spark_save";
    const auto backupPath = std::filesystem::path(dir) / "integrity-backup.spark_save.bak";
    ASSERT_TRUE(std::filesystem::exists(backupPath));
    std::vector<char> primaryBytes = ReadBytes(primaryPath);
    ASSERT_TRUE(ReplaceFirstAscii(primaryBytes, "Second integrity revision", "Xecond integrity revision"));
    ASSERT_TRUE(WriteBytes(primaryPath, primaryBytes));

    World recovered;
    ASSERT_TRUE(saveSystem.Load("integrity-backup", recovered));
    EXPECT_EQ(recovered.GetEntityCount(), 1u);
    EXPECT_TRUE(WorldContainsNamedEntity(recovered, "first-integrity-revision"));
    EXPECT_FALSE(WorldContainsNamedEntity(recovered, "second-integrity-revision"));

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_Load_RejectsWhenV4PrimaryAndBackupChecksumsFail)
{
    const std::string dir = MakeTempSaveDir("v4_both_revisions_corrupt");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    ASSERT_TRUE(saveSystem.Initialize(dir));

    World first;
    first.AddComponent<Transform>(first.CreateEntity("first-corruptible-revision"));
    SaveMetadata firstMetadata;
    firstMetadata.saveName = "First corruptible revision";
    ASSERT_TRUE(saveSystem.Save("both-corrupt", first, firstMetadata));

    World second;
    second.AddComponent<Transform>(second.CreateEntity("second-corruptible-revision"));
    SaveMetadata secondMetadata;
    secondMetadata.saveName = "Second corruptible revision";
    ASSERT_TRUE(saveSystem.Save("both-corrupt", second, secondMetadata));

    const auto primaryPath = std::filesystem::path(dir) / "both-corrupt.spark_save";
    const auto backupPath = std::filesystem::path(dir) / "both-corrupt.spark_save.bak";
    std::vector<char> primary = ReadBytes(primaryPath);
    std::vector<char> backup = ReadBytes(backupPath);
    ASSERT_TRUE(ReplaceFirstAscii(primary, "Second corruptible revision", "Xecond corruptible revision"));
    ASSERT_TRUE(ReplaceFirstAscii(backup, "First corruptible revision", "Xirst corruptible revision"));
    ASSERT_TRUE(WriteBytes(primaryPath, primary));
    ASSERT_TRUE(WriteBytes(backupPath, backup));

    World liveWorld;
    liveWorld.AddComponent<Transform>(liveWorld.CreateEntity("both-corrupt-live-sentinel"));
    EXPECT_FALSE(saveSystem.Load("both-corrupt", liveWorld));
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "both-corrupt-live-sentinel"));

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_Save_PreservesValidV4BackupWhenPrimaryChecksumFails)
{
    const std::string dir = MakeTempSaveDir("v4_preserve_backup");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    ASSERT_TRUE(saveSystem.Initialize(dir));

    auto saveRevision = [&](const char* entityName, const char* saveName)
    {
        World world;
        world.AddComponent<Transform>(world.CreateEntity(entityName));
        SaveMetadata metadata;
        metadata.saveName = saveName;
        return saveSystem.Save("preserve-backup", world, metadata);
    };

    ASSERT_TRUE(saveRevision("first-preserved-revision", "First preserved revision"));
    ASSERT_TRUE(saveRevision("second-primary-revision", "Second primary revision"));

    const auto primaryPath = std::filesystem::path(dir) / "preserve-backup.spark_save";
    const auto backupPath = std::filesystem::path(dir) / "preserve-backup.spark_save.bak";
    const std::vector<char> retainedBefore = ReadBytes(backupPath);
    ASSERT_TRUE(HasValidV4Checksum(retainedBefore));

    std::vector<char> corruptPrimary = ReadBytes(primaryPath);
    ASSERT_TRUE(ReplaceFirstAscii(corruptPrimary, "Second primary revision", "Xecond primary revision"));
    ASSERT_TRUE(WriteBytes(primaryPath, corruptPrimary));

    ASSERT_TRUE(saveRevision("third-primary-revision", "Third primary revision"));
    EXPECT_TRUE(ReadBytes(backupPath) == retainedBefore);

    std::vector<char> thirdPrimary = ReadBytes(primaryPath);
    ASSERT_TRUE(ReplaceFirstAscii(thirdPrimary, "Third primary revision", "Xhird primary revision"));
    ASSERT_TRUE(WriteBytes(primaryPath, thirdPrimary));

    World recovered;
    ASSERT_TRUE(saveSystem.Load("preserve-backup", recovered));
    EXPECT_TRUE(WorldContainsNamedEntity(recovered, "first-preserved-revision"));
    EXPECT_FALSE(WorldContainsNamedEntity(recovered, "second-primary-revision"));
    EXPECT_FALSE(WorldContainsNamedEntity(recovered, "third-primary-revision"));

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

    const bool hasV4Checksum = ReadLittleEndian32(original.data() + 4) >= 4u;
    for (size_t bytesRemoved = 1; bytesRemoved <= sizeof(uint32_t); ++bytesRemoved)
    {
        std::vector<char> truncated = original;
        if (hasV4Checksum)
            truncated.resize(truncated.size() - sizeof(uint32_t));
        truncated.resize(truncated.size() - bytesRemoved);
        if (hasV4Checksum)
            ASSERT_TRUE(AppendV4Checksum(truncated));
        ASSERT_TRUE(WriteBytes(path, truncated));

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
    constexpr uint64_t oversizedBytes = 512ull * 1024ull * 1024ull + 1ull;
    {
#if !defined(_WIN32)
        ScopedMinimumFileSizeLimit fileSizeLimit(oversizedBytes);
        ASSERT_TRUE(fileSizeLimit.Ready());
#endif
        std::filesystem::resize_file(path, oversizedBytes);
    }

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
    ASSERT_TRUE(prefix.size() >= 8u + sizeof(uint32_t));
    const bool hasV4Checksum = ReadLittleEndian32(prefix.data() + 4) >= 4u;
    if (hasV4Checksum)
    {
        ASSERT_TRUE(prefix.size() >= 8u + 2u * sizeof(uint32_t));
        prefix.resize(prefix.size() - sizeof(uint32_t));
    }
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

    for (const auto& malformedBytes : malformed)
    {
        auto bytes = malformedBytes;
        if (hasV4Checksum)
            ASSERT_TRUE(AppendV4Checksum(bytes));
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

TEST(SaveMigration_SaveAbortsWhenPreviousRevisionCannotBeRetained)
{
    const std::string dir = MakeTempSaveDir("backup_retention_failure");
    SaveSystem& ss = SaveSystem::GetInstance();
    EXPECT_TRUE(ss.Initialize(dir));

    World firstWorld;
    const EntityID firstEntity = firstWorld.CreateEntity("first-revision");
    firstWorld.AddComponent<Transform>(firstEntity);
    SaveMetadata firstMetadata;
    firstMetadata.saveName = "First revision";
    EXPECT_TRUE(ss.Save("retention-failure", firstWorld, firstMetadata));

    const auto primaryPath = std::filesystem::path(dir) / "retention-failure.spark_save";
    const auto backupPath = std::filesystem::path(dir) / "retention-failure.spark_save.bak";
    ASSERT_TRUE(std::filesystem::create_directory(backupPath));

    World secondWorld;
    const EntityID secondEntity = secondWorld.CreateEntity("second-revision");
    secondWorld.AddComponent<Transform>(secondEntity);
    SaveMetadata secondMetadata;
    secondMetadata.saveName = "Second revision";

    // Retaining the previous revision is part of the save transaction. If that
    // boundary fails, replacing the primary would silently discard the only
    // recoverable copy of the first revision.
    EXPECT_FALSE(ss.Save("retention-failure", secondWorld, secondMetadata));
    EXPECT_TRUE(std::filesystem::exists(primaryPath));
    EXPECT_TRUE(std::filesystem::is_directory(backupPath));

    SaveMetadata retainedMetadata;
    EXPECT_TRUE(ss.GetSaveMetadata("retention-failure", retainedMetadata));
    EXPECT_EQ(retainedMetadata.saveName, std::string("First revision"));

    World retainedWorld;
    EXPECT_TRUE(ss.Load("retention-failure", retainedWorld));
    EXPECT_TRUE(WorldContainsNamedEntity(retainedWorld, "first-revision"));
    EXPECT_FALSE(WorldContainsNamedEntity(retainedWorld, "second-revision"));

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_DeleteSave_EvictsCachedPrimaryAndBackup)
{
    const std::string dir = MakeTempSaveDir("delete_cached_revisions");
    SaveSystem& ss = SaveSystem::GetInstance();
    ss.SetFileCache(nullptr);
    EXPECT_TRUE(ss.Initialize(dir));

    LocalFileCache cache;
    ss.SetFileCache(&cache);

    World firstWorld;
    const EntityID firstEntity = firstWorld.CreateEntity("cached-first-revision");
    firstWorld.AddComponent<Transform>(firstEntity);
    SaveMetadata firstMetadata;
    firstMetadata.saveName = "Cached first revision";
    EXPECT_TRUE(ss.Save("cached-delete", firstWorld, firstMetadata));

    World secondWorld;
    const EntityID secondEntity = secondWorld.CreateEntity("cached-second-revision");
    secondWorld.AddComponent<Transform>(secondEntity);
    SaveMetadata secondMetadata;
    secondMetadata.saveName = "Cached second revision";
    EXPECT_TRUE(ss.Save("cached-delete", secondWorld, secondMetadata));

    const auto primaryPath = std::filesystem::path(dir) / "cached-delete.spark_save";
    const auto backupPath = std::filesystem::path(dir) / "cached-delete.spark_save.bak";
    EXPECT_TRUE(std::filesystem::exists(primaryPath));
    EXPECT_TRUE(std::filesystem::exists(backupPath));

    World cachedPrimary;
    EXPECT_TRUE(ss.Load("cached-delete", cachedPrimary));
    EXPECT_TRUE(cache.Contains(primaryPath.string()));

    const auto cachedBackup = cache.ReadBinary(backupPath.string());
    EXPECT_TRUE(cachedBackup.IsOk());
    EXPECT_TRUE(cache.Contains(backupPath.string()));

    EXPECT_TRUE(ss.DeleteSave("cached-delete"));
    EXPECT_FALSE(std::filesystem::exists(primaryPath));
    EXPECT_FALSE(std::filesystem::exists(backupPath));
    EXPECT_FALSE(cache.Contains(primaryPath.string()));
    EXPECT_FALSE(cache.Contains(backupPath.string()));

    World afterDelete;
    EXPECT_FALSE(ss.Load("cached-delete", afterDelete));

    ss.SetFileCache(nullptr);
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

TEST(SaveMigration_CurrentWriterPersistsScreenshotPathAtCurrentVersion)
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
        ASSERT_TRUE(RefreshV4Checksum(bytes));
        malformedCases.push_back(std::move(bytes));
    }
    {
        auto bytes = original;
        ASSERT_TRUE(DuplicateWireRecord<uint16_t>(bytes, locations.propertyCount, locations.firstPropertyBegin,
                                                  locations.firstPropertyEnd));
        ASSERT_TRUE(RefreshV4Checksum(bytes));
        malformedCases.push_back(std::move(bytes));
    }
    {
        auto bytes = original;
        ASSERT_TRUE(DuplicateWireRecord<uint32_t>(bytes, locations.customStateCount, locations.firstCustomStateBegin,
                                                  locations.firstCustomStateEnd));
        ASSERT_TRUE(RefreshV4Checksum(bytes));
        malformedCases.push_back(std::move(bytes));
    }
    {
        auto bytes = original;
        ASSERT_TRUE(ReplaceLengthPrefixedString(bytes, "Transform", "NameComponent"));
        ASSERT_TRUE(RefreshV4Checksum(bytes));
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
    const std::string dir = MakeTempSaveDir("in_memory_duplicate");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));
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

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_DiskAndMemoryUseSharedRepresentationBoundaries)
{
    using Limits = SaveRepresentationLimits;

    // Synthetic checks cover very large record-count boundaries without
    // constructing pathological vectors or maps merely to reach them.
    EXPECT_TRUE(Limits::SupportsStringBytes(Limits::maxStringBytes));
    EXPECT_FALSE(Limits::SupportsStringBytes(Limits::maxStringBytes + 1u));
    EXPECT_TRUE(Limits::SupportsMetadataBytes(Limits::maxMetadataBytes));
    EXPECT_FALSE(Limits::SupportsMetadataBytes(Limits::maxMetadataBytes + 1u));
    EXPECT_TRUE(Limits::SupportsWireBytes(Limits::maxWireBytes));
    EXPECT_FALSE(Limits::SupportsWireBytes(Limits::maxWireBytes + 1u));
    EXPECT_TRUE(Limits::SupportsEntityCount(Limits::maxEntities));
    EXPECT_FALSE(Limits::SupportsEntityCount(Limits::maxEntities + 1u));
    EXPECT_TRUE(Limits::SupportsComponentCount(Limits::maxComponentsPerEntity));
    EXPECT_FALSE(Limits::SupportsComponentCount(Limits::maxComponentsPerEntity + 1u));
    EXPECT_TRUE(Limits::SupportsPropertyCount(Limits::maxPropertiesPerComponent));
    EXPECT_FALSE(Limits::SupportsPropertyCount(Limits::maxPropertiesPerComponent + 1u));
    EXPECT_TRUE(Limits::SupportsCustomStateCount(Limits::maxCustomStateEntries));
    EXPECT_FALSE(Limits::SupportsCustomStateCount(Limits::maxCustomStateEntries + 1u));

    SaveRepresentationBudget componentBudget;
    EXPECT_TRUE(componentBudget.AddComponents(Limits::maxTotalComponents));
    EXPECT_FALSE(componentBudget.AddComponents(1u));
    SaveRepresentationBudget propertyBudget;
    EXPECT_TRUE(propertyBudget.AddProperties(Limits::maxTotalProperties));
    EXPECT_FALSE(propertyBudget.AddProperties(1u));
    SaveRepresentationBudget customStateBudget;
    EXPECT_TRUE(customStateBudget.AddCustomStateEntries(Limits::maxCustomStateEntries));
    EXPECT_FALSE(customStateBudget.AddCustomStateEntries(1u));
    SaveRepresentationBudget wireBudget;
    EXPECT_TRUE(wireBudget.AddWireBytes(Limits::maxWireBytes));
    EXPECT_FALSE(wireBudget.AddWireBytes(1u));
    SaveRepresentationBudget overflowBudget;
    EXPECT_FALSE(overflowBudget.AddWireBytes(std::numeric_limits<size_t>::max()));

    const std::string dir = MakeTempSaveDir("shared_representation_limits");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    // A representative uint16-sized key must round-trip through the disk path.
    const std::string maximumKey(Limits::maxStringBytes, 'k');
    World diskSource;
    SaveMetadata metadata;
    metadata.saveName = "Representation boundary";
    EXPECT_TRUE(saveSystem.Save("maximum-key", diskSource, metadata, {{maximumKey, "value"}}));
    World diskTarget;
    std::unordered_map<std::string, std::string> loadedCustomState;
    EXPECT_TRUE(saveSystem.Load("maximum-key", diskTarget, loadedCustomState));
    EXPECT_EQ(loadedCustomState.size(), 1u);
    EXPECT_EQ(loadedCustomState.at(maximumKey), std::string("value"));

    const std::string oversizedKey(Limits::maxStringBytes + 1u, 'x');
    EXPECT_FALSE(saveSystem.Save("oversized-key", diskSource, metadata, {{oversizedKey, "value"}}));
    EXPECT_FALSE(saveSystem.SaveExists("oversized-key"));

    // The public in-memory path accepts the exact name boundary and rejects
    // max+1 before it can touch live registry topology or entity subscriptions.
    SaveData maximumName;
    SerializedEntity maximumNameEntity{};
    maximumNameEntity.name.assign(Limits::maxStringBytes, 'n');
    maximumName.entities.push_back(std::move(maximumNameEntity));
    World acceptedWorld;
    EXPECT_TRUE(saveSystem.DeserializeWorld(maximumName, acceptedWorld));
    EXPECT_EQ(acceptedWorld.GetEntityCount(), 1u);

    SaveData oversizedName;
    SerializedEntity oversizedNameEntity{};
    oversizedNameEntity.name.assign(Limits::maxStringBytes + 1u, 'n');
    oversizedName.entities.push_back(std::move(oversizedNameEntity));
    World liveWorld;
    const EntityID sentinel = liveWorld.CreateEntity("representation-live-sentinel");
    liveWorld.AddComponent<Transform>(sentinel).position.x = 515.0f;
    int deliveries = 0;
    auto subscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(sentinel), [&](const SaveLoadLifecycleProbeEvent&) { ++deliveries; });
    const auto topologyBefore = RegistryStorageIds(liveWorld);

    EXPECT_FALSE(saveSystem.DeserializeWorld(oversizedName, liveWorld));
    EXPECT_TRUE(RegistryStorageIds(liveWorld) == topologyBefore);
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "representation-live-sentinel"));
    EXPECT_NEAR(liveWorld.GetComponent<Transform>(sentinel)->position.x, 515.0f, 0.0001f);
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(sentinel)),
              1u);
    Spark::EntityEventBus::Global().Publish<SaveLoadLifecycleProbeEvent>(static_cast<Spark::EventEntityID>(sentinel),
                                                                         {1});
    EXPECT_EQ(deliveries, 1);

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_MissingCustomComponentFailsBeforeLiveStoragePreparation)
{
    const std::string dir = MakeTempSaveDir("missing_custom_component");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("missing-component-source");
    source.AddComponent<Transform>(sourceEntity).position.x = 21.0f;
    SaveMetadata metadata;
    metadata.saveName = "Missing custom component";
    EXPECT_TRUE(saveSystem.Save("missing-custom-component", source, metadata, {{"candidate", "state"}}));

    const auto path = std::filesystem::path(dir) / "missing-custom-component.spark_save";
    std::vector<char> bytes = ReadBytes(path);
    ASSERT_TRUE(ReplaceLengthPrefixedString(bytes, "Transform", "MissingTx"));
    ASSERT_TRUE(RefreshV4Checksum(bytes));
    ASSERT_TRUE(WriteBytes(path, bytes));

    auto& serializers = ComponentSerializerRegistry::GetInstance();
    serializers.Unregister("MissingTx");
    serializers.Register(
        "MissingTx", [](const void*) { return SerializedComponent{"MissingTx", {}}; },
        [](World&, EntityID, const SerializedComponent&)
        {
            // Deliberately broken: a successful callback must materialize the
            // component declared by the serialized record.
        });
    ComponentFactory::Get().Register("MissingTx",
                                     MakeProbeComponentOps<MissingRestoreProbe>(&CountMissingRestorePrepare));
    g_missingRestorePrepareCalls = 0;

    World liveWorld;
    const EntityID sentinel = liveWorld.CreateEntity("missing-component-live-sentinel");
    liveWorld.AddComponent<Transform>(sentinel).position.x = 616.0f;
    int deliveries = 0;
    auto subscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(sentinel), [&](const SaveLoadLifecycleProbeEvent&) { ++deliveries; });
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};
    const auto topologyBefore = RegistryStorageIds(liveWorld);
    const entt::registry& registryBefore = liveWorld.GetRegistry();
    EXPECT_TRUE(registryBefore.storage<MissingRestoreProbe>() == nullptr);

    const bool loaded = saveSystem.Load("missing-custom-component", liveWorld, customState);
    serializers.Unregister("MissingTx");

    EXPECT_FALSE(loaded);
    EXPECT_EQ(g_missingRestorePrepareCalls, 0);
    EXPECT_TRUE(RegistryStorageIds(liveWorld) == topologyBefore);
    const entt::registry& registryAfter = liveWorld.GetRegistry();
    EXPECT_TRUE(registryAfter.storage<MissingRestoreProbe>() == nullptr);
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "missing-component-live-sentinel"));
    EXPECT_NEAR(liveWorld.GetComponent<Transform>(sentinel)->position.x, 616.0f, 0.0001f);
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("live"), std::string("sentinel"));
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(sentinel)),
              1u);
    Spark::EntityEventBus::Global().Publish<SaveLoadLifecycleProbeEvent>(static_cast<Spark::EventEntityID>(sentinel),
                                                                         {1});
    EXPECT_EQ(deliveries, 1);

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_LiveStoragePreparationFailurePropagatesWithoutFalseRollbackClaim)
{
    const std::string dir = MakeTempSaveDir("storage_preparation_failure");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("storage-preparation-source");
    source.AddComponent<Transform>(sourceEntity).position.x = 31.0f;
    SaveMetadata metadata;
    metadata.saveName = "Storage preparation failure";
    EXPECT_TRUE(saveSystem.Save("storage-preparation", source, metadata, {{"candidate", "state"}}));

    const auto path = std::filesystem::path(dir) / "storage-preparation.spark_save";
    std::vector<char> bytes = ReadBytes(path);
    ASSERT_TRUE(ReplaceLengthPrefixedString(bytes, "Transform", "PrepareTx"));
    ASSERT_TRUE(RefreshV4Checksum(bytes));
    ASSERT_TRUE(WriteBytes(path, bytes));

    auto& serializers = ComponentSerializerRegistry::GetInstance();
    serializers.Unregister("PrepareTx");
    serializers.Register(
        "PrepareTx", [](const void*) { return SerializedComponent{"PrepareTx", {}}; },
        [](World& world, EntityID entity, const SerializedComponent&)
        { world.AddComponent<PrepareFailureProbe>(entity); });
    ComponentFactory::Get().Register("PrepareTx",
                                     MakeProbeComponentOps<PrepareFailureProbe>(&ThrowAfterPreparingLiveStorage));
    g_prepareFailureCalls = 0;

    World liveWorld;
    const EntityID sentinel = liveWorld.CreateEntity("storage-preparation-live-sentinel");
    liveWorld.AddComponent<Transform>(sentinel).position.x = 717.0f;
    int deliveries = 0;
    auto subscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(sentinel), [&](const SaveLoadLifecycleProbeEvent&) { ++deliveries; });
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};
    const auto topologyBefore = RegistryStorageIds(liveWorld);
    const entt::registry& registryBefore = liveWorld.GetRegistry();
    EXPECT_TRUE(registryBefore.storage<PrepareFailureProbe>() == nullptr);

    EXPECT_THROW(saveSystem.Load("storage-preparation", liveWorld, customState), std::runtime_error);
    serializers.Unregister("PrepareTx");

    EXPECT_EQ(g_prepareFailureCalls, 1);
    const entt::registry& registryAfter = liveWorld.GetRegistry();
    const auto* preparedStorage = registryAfter.storage<PrepareFailureProbe>();
    ASSERT_TRUE(preparedStorage != nullptr);
    EXPECT_TRUE(preparedStorage->empty());
    const auto topologyAfter = RegistryStorageIds(liveWorld);
    EXPECT_EQ(topologyAfter.size(), topologyBefore.size() + 1u);
    EXPECT_TRUE(std::find(topologyAfter.begin(), topologyAfter.end(), entt::type_hash<PrepareFailureProbe>::value()) !=
                topologyAfter.end());
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "storage-preparation-live-sentinel"));
    EXPECT_NEAR(liveWorld.GetComponent<Transform>(sentinel)->position.x, 717.0f, 0.0001f);
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("live"), std::string("sentinel"));
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(sentinel)),
              1u);
    Spark::EntityEventBus::Global().Publish<SaveLoadLifecycleProbeEvent>(static_cast<Spark::EventEntityID>(sentinel),
                                                                         {1});
    EXPECT_EQ(deliveries, 1);

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_ExtraCandidateEntityFailsBeforeLiveStoragePreparation)
{
    const std::string dir = MakeTempSaveDir("extra_candidate_entity");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("ghost-source");
    source.AddComponent<Transform>(sourceEntity).position.x = 41.0f;
    SaveMetadata metadata;
    metadata.saveName = "Ghost candidate entity";
    EXPECT_TRUE(saveSystem.Save("ghost-candidate", source, metadata, {{"candidate", "state"}}));

    const auto path = std::filesystem::path(dir) / "ghost-candidate.spark_save";
    std::vector<char> bytes = ReadBytes(path);
    ASSERT_TRUE(ReplaceLengthPrefixedString(bytes, "Transform", "GhostTx"));
    ASSERT_TRUE(RefreshV4Checksum(bytes));
    ASSERT_TRUE(WriteBytes(path, bytes));

    auto& serializers = ComponentSerializerRegistry::GetInstance();
    serializers.Unregister("GhostTx");
    serializers.Register(
        "GhostTx", [](const void*) { return SerializedComponent{"GhostTx", {}}; },
        [](World& world, EntityID entity, const SerializedComponent&)
        {
            world.AddComponent<CandidateGhostProbe>(entity);
            const EntityID ghost = world.CreateEntity("deserializer-created-ghost");
            world.AddComponent<CandidateGhostProbe>(ghost);
        });
    ComponentFactory::Get().Register(
        "GhostTx", MakeProbeComponentOps<CandidateGhostProbe>(&CountCandidateTopologyPrepare<CandidateGhostProbe>));
    g_candidateTopologyPrepareCalls = 0;

    World liveWorld;
    const EntityID sentinel = liveWorld.CreateEntity("ghost-live-sentinel");
    liveWorld.AddComponent<Transform>(sentinel).position.x = 818.0f;
    int deliveries = 0;
    auto subscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(sentinel), [&](const SaveLoadLifecycleProbeEvent&) { ++deliveries; });
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};
    const auto topologyBefore = RegistryStorageIds(liveWorld);

    const bool loaded = saveSystem.Load("ghost-candidate", liveWorld, customState);
    serializers.Unregister("GhostTx");

    EXPECT_FALSE(loaded);
    EXPECT_EQ(g_candidateTopologyPrepareCalls, 0);
    EXPECT_TRUE(RegistryStorageIds(liveWorld) == topologyBefore);
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "ghost-live-sentinel"));
    EXPECT_FALSE(WorldContainsNamedEntity(liveWorld, "deserializer-created-ghost"));
    EXPECT_NEAR(liveWorld.GetComponent<Transform>(sentinel)->position.x, 818.0f, 0.0001f);
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("live"), std::string("sentinel"));
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(sentinel)),
              1u);
    Spark::EntityEventBus::Global().Publish<SaveLoadLifecycleProbeEvent>(static_cast<Spark::EventEntityID>(sentinel),
                                                                         {1});
    EXPECT_EQ(deliveries, 1);

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_OneEntityTransientCandidateAllocationFailsBeforeLiveStoragePreparation)
{
    const std::string dir = MakeTempSaveDir("one_entity_transient_candidate");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("one-transient-source");
    source.AddComponent<Transform>(sourceEntity).position.x = 61.0f;
    SaveMetadata metadata;
    metadata.saveName = "One transient candidate entity";
    EXPECT_TRUE(saveSystem.Save("one-transient-candidate", source, metadata, {{"candidate", "state"}}));

    const auto path = std::filesystem::path(dir) / "one-transient-candidate.spark_save";
    std::vector<char> bytes = ReadBytes(path);
    ASSERT_TRUE(ReplaceLengthPrefixedString(bytes, "Transform", "TransientOneTx"));
    ASSERT_TRUE(RefreshV4Checksum(bytes));
    ASSERT_TRUE(WriteBytes(path, bytes));

    auto& serializers = ComponentSerializerRegistry::GetInstance();
    serializers.Unregister("TransientOneTx");
    serializers.Register(
        "TransientOneTx", [](const void*) { return SerializedComponent{"TransientOneTx", {}}; },
        [](World& world, EntityID entity, const SerializedComponent&)
        {
            world.AddComponent<TransientCandidateProbe>(entity);
            const EntityID ghost = world.CreateEntity();
            g_transientCandidateGhost = ghost;
            world.DestroyEntity(ghost);
        });
    ComponentFactory::Get().Register("TransientOneTx",
                                     MakeProbeComponentOps<TransientCandidateProbe>(&CountTransientCandidatePrepare));
    g_transientCandidatePrepareCalls = 0;
    g_transientCandidateGhost = entt::null;

    World liveWorld;
    const EntityID parent = liveWorld.CreateEntity("one-transient-live-parent");
    const EntityID child = liveWorld.CreateEntity("one-transient-live-child");
    liveWorld.AddComponent<Transform>(parent).position.x = 303.0f;
    liveWorld.AddComponent<Transform>(child).position.x = 404.0f;
    ASSERT_TRUE(liveWorld.SetParent(child, parent));
    int deliveries = 0;
    auto subscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(child), [&](const SaveLoadLifecycleProbeEvent&) { ++deliveries; });
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};
    const auto topologyBefore = RegistryStorageIds(liveWorld);
    const auto& liveEntityStorageBefore = liveWorld.GetRegistry().storage<entt::entity>();
    const size_t entityStorageSizeBefore = liveEntityStorageBefore.size();
    const size_t entityFreeListBefore = liveEntityStorageBefore.free_list();

    const bool loaded = saveSystem.Load("one-transient-candidate", liveWorld, customState);
    serializers.Unregister("TransientOneTx");

    EXPECT_FALSE(loaded);
    EXPECT_EQ(g_transientCandidatePrepareCalls, 0);
    EXPECT_TRUE(g_transientCandidateGhost == child);
    EXPECT_TRUE(RegistryStorageIds(liveWorld) == topologyBefore);
    EXPECT_EQ(liveWorld.GetRegistry().storage<entt::entity>().size(), entityStorageSizeBefore);
    EXPECT_EQ(liveWorld.GetRegistry().storage<entt::entity>().free_list(), entityFreeListBefore);
    EXPECT_EQ(liveWorld.GetEntityCount(), 2u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "one-transient-live-parent"));
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "one-transient-live-child"));
    const Transform* parentTransform = liveWorld.GetComponent<Transform>(parent);
    const Transform* childTransform = liveWorld.GetComponent<Transform>(child);
    ASSERT_TRUE(parentTransform != nullptr);
    ASSERT_TRUE(childTransform != nullptr);
    EXPECT_TRUE(std::find(parentTransform->children.begin(), parentTransform->children.end(), child) !=
                parentTransform->children.end());
    EXPECT_TRUE(childTransform->parent == parent);
    EXPECT_NEAR(parentTransform->position.x, 303.0f, 0.0001f);
    EXPECT_NEAR(childTransform->position.x, 404.0f, 0.0001f);
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("live"), std::string("sentinel"));
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(child)),
              1u);
    Spark::EntityEventBus::Global().Publish<SaveLoadLifecycleProbeEvent>(static_cast<Spark::EventEntityID>(child), {1});
    EXPECT_EQ(deliveries, 1);

    const EntityID next = liveWorld.CreateEntity();
    EXPECT_EQ(static_cast<uint32_t>(next), 2u);
    liveWorld.DestroyEntity(next);
    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_TwoEntityTransientCandidateAllocationFailsBeforeLiveStoragePreparation)
{
    const std::string dir = MakeTempSaveDir("two_entity_transient_candidate");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID first = source.CreateEntity("two-transient-first");
    source.AddComponent<Transform>(first).position.x = 71.0f;
    const EntityID second = source.CreateEntity("two-transient-second");
    source.AddComponent<Transform>(second).position.x = 72.0f;
    SaveMetadata metadata;
    metadata.saveName = "Two transient candidate entities";
    EXPECT_TRUE(saveSystem.Save("two-transient-candidate", source, metadata, {{"candidate", "state"}}));

    const auto path = std::filesystem::path(dir) / "two-transient-candidate.spark_save";
    std::vector<char> bytes = ReadBytes(path);
    ASSERT_TRUE(ReplaceLengthPrefixedString(bytes, "Transform", "TransientTwoTx"));
    ASSERT_TRUE(RefreshV4Checksum(bytes));
    ASSERT_TRUE(WriteBytes(path, bytes));

    auto& serializers = ComponentSerializerRegistry::GetInstance();
    serializers.Unregister("TransientTwoTx");
    serializers.Register(
        "TransientTwoTx", [](const void*) { return SerializedComponent{"TransientTwoTx", {}}; },
        [](World& world, EntityID entity, const SerializedComponent&)
        {
            world.AddComponent<TransientCandidateProbe>(entity);
            const EntityID ghost = world.CreateEntity();
            g_transientCandidateGhost = ghost;
            world.DestroyEntity(ghost);
        });
    ComponentFactory::Get().Register("TransientTwoTx",
                                     MakeProbeComponentOps<TransientCandidateProbe>(&CountTransientCandidatePrepare));
    g_transientCandidatePrepareCalls = 0;
    g_transientCandidateGhost = entt::null;

    World liveWorld;
    const EntityID parent = liveWorld.CreateEntity("two-transient-live-parent");
    const EntityID child = liveWorld.CreateEntity("two-transient-live-child");
    liveWorld.AddComponent<Transform>(parent).position.x = 505.0f;
    liveWorld.AddComponent<Transform>(child).position.x = 606.0f;
    ASSERT_TRUE(liveWorld.SetParent(child, parent));
    int deliveries = 0;
    auto subscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(child), [&](const SaveLoadLifecycleProbeEvent&) { ++deliveries; });
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};
    const auto topologyBefore = RegistryStorageIds(liveWorld);
    const auto& liveEntityStorageBefore = liveWorld.GetRegistry().storage<entt::entity>();
    const size_t entityStorageSizeBefore = liveEntityStorageBefore.size();
    const size_t entityFreeListBefore = liveEntityStorageBefore.free_list();

    const bool loaded = saveSystem.Load("two-transient-candidate", liveWorld, customState);
    serializers.Unregister("TransientTwoTx");

    EXPECT_FALSE(loaded);
    EXPECT_EQ(g_transientCandidatePrepareCalls, 0);
    EXPECT_TRUE(RegistryStorageIds(liveWorld) == topologyBefore);
    EXPECT_EQ(liveWorld.GetRegistry().storage<entt::entity>().size(), entityStorageSizeBefore);
    EXPECT_EQ(liveWorld.GetRegistry().storage<entt::entity>().free_list(), entityFreeListBefore);
    EXPECT_EQ(liveWorld.GetEntityCount(), 2u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "two-transient-live-parent"));
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "two-transient-live-child"));
    const Transform* parentTransform = liveWorld.GetComponent<Transform>(parent);
    const Transform* childTransform = liveWorld.GetComponent<Transform>(child);
    ASSERT_TRUE(parentTransform != nullptr);
    ASSERT_TRUE(childTransform != nullptr);
    EXPECT_TRUE(std::find(parentTransform->children.begin(), parentTransform->children.end(), child) !=
                parentTransform->children.end());
    EXPECT_TRUE(childTransform->parent == parent);
    EXPECT_NEAR(parentTransform->position.x, 505.0f, 0.0001f);
    EXPECT_NEAR(childTransform->position.x, 606.0f, 0.0001f);
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("live"), std::string("sentinel"));
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(child)),
              1u);
    Spark::EntityEventBus::Global().Publish<SaveLoadLifecycleProbeEvent>(static_cast<Spark::EventEntityID>(child), {1});
    EXPECT_EQ(deliveries, 1);

    const EntityID next = liveWorld.CreateEntity();
    EXPECT_EQ(static_cast<uint32_t>(next), 2u);
    liveWorld.DestroyEntity(next);
    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_CandidateAllocatorCursorMutationDoesNotReachLiveWorld)
{
    const std::string dir = MakeTempSaveDir("candidate_allocator_cursor");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("allocator-cursor-source");
    source.AddComponent<Transform>(sourceEntity).position.x = 81.0f;
    SaveMetadata metadata;
    metadata.saveName = "Candidate allocator cursor";
    EXPECT_TRUE(saveSystem.Save("candidate-allocator-cursor", source, metadata, {{"loaded", "state"}}));

    const auto path = std::filesystem::path(dir) / "candidate-allocator-cursor.spark_save";
    std::vector<char> bytes = ReadBytes(path);
    ASSERT_TRUE(ReplaceLengthPrefixedString(bytes, "Transform", "CursorTx"));
    ASSERT_TRUE(RefreshV4Checksum(bytes));
    ASSERT_TRUE(WriteBytes(path, bytes));

    auto& serializers = ComponentSerializerRegistry::GetInstance();
    serializers.Unregister("CursorTx");
    serializers.Register(
        "CursorTx", [](const void*) { return SerializedComponent{"CursorTx", {}}; },
        [](World& world, EntityID entity, const SerializedComponent&)
        {
            world.AddComponent<AllocatorCursorProbe>(entity);
            world.GetRegistry().storage<entt::entity>().start_from(static_cast<EntityID>(777u));
        });
    ComponentFactory::Get().Register("CursorTx",
                                     MakeProbeComponentOps<AllocatorCursorProbe>(&CountAllocatorCursorPrepare));
    g_allocatorCursorPrepareCalls = 0;

    World liveWorld;
    const EntityID parent = liveWorld.CreateEntity("allocator-cursor-live-parent");
    const EntityID child = liveWorld.CreateEntity("allocator-cursor-live-child");
    liveWorld.AddComponent<Transform>(parent);
    liveWorld.AddComponent<Transform>(child);
    ASSERT_TRUE(liveWorld.SetParent(child, parent));
    auto parentSubscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(parent), [](const SaveLoadLifecycleProbeEvent&) {});
    auto childSubscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(child), [](const SaveLoadLifecycleProbeEvent&) {});
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};

    const bool loaded = saveSystem.Load("candidate-allocator-cursor", liveWorld, customState);
    serializers.Unregister("CursorTx");

    EXPECT_TRUE(loaded);
    EXPECT_EQ(g_allocatorCursorPrepareCalls, 1);
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    const EntityID incoming = FindNamedEntity(liveWorld, "allocator-cursor-source");
    ASSERT_TRUE(incoming != entt::null);
    EXPECT_EQ(static_cast<uint32_t>(incoming), 0u);
    EXPECT_TRUE(liveWorld.HasComponent<AllocatorCursorProbe>(incoming));
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("loaded"), std::string("state"));
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(parent)),
              0u);
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(child)),
              0u);

    const EntityID next = liveWorld.CreateEntity();
    EXPECT_EQ(static_cast<uint32_t>(next), 1u);
    liveWorld.DestroyEntity(next);
    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_ExtraStagedComponentFailsBeforeLiveStoragePreparation)
{
    const std::string dir = MakeTempSaveDir("extra_staged_component");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID first = source.CreateEntity("extra-component-source");
    source.AddComponent<Transform>(first).position.x = 51.0f;
    const EntityID second = source.CreateEntity("declared-transform-source");
    source.AddComponent<Transform>(second).position.x = 52.0f;
    SaveMetadata metadata;
    metadata.saveName = "Extra staged component";
    EXPECT_TRUE(saveSystem.Save("extra-staged-component", source, metadata, {{"candidate", "state"}}));

    const auto path = std::filesystem::path(dir) / "extra-staged-component.spark_save";
    std::vector<char> bytes = ReadBytes(path);
    ASSERT_TRUE(ReplaceLengthPrefixedString(bytes, "Transform", "ExtraTx"));
    ASSERT_TRUE(RefreshV4Checksum(bytes));
    ASSERT_TRUE(WriteBytes(path, bytes));

    auto& serializers = ComponentSerializerRegistry::GetInstance();
    serializers.Unregister("ExtraTx");
    serializers.Register(
        "ExtraTx", [](const void*) { return SerializedComponent{"ExtraTx", {}}; },
        [](World& world, EntityID entity, const SerializedComponent&)
        {
            world.AddComponent<CandidateExtraComponentProbe>(entity);
            world.AddComponent<Transform>(entity);
        });
    ComponentFactory::Get().Register("ExtraTx", MakeProbeComponentOps<CandidateExtraComponentProbe>(
                                                    &CountCandidateTopologyPrepare<CandidateExtraComponentProbe>));
    g_candidateTopologyPrepareCalls = 0;

    World liveWorld;
    const EntityID sentinel = liveWorld.CreateEntity("extra-component-live-sentinel");
    liveWorld.AddComponent<Transform>(sentinel).position.x = 919.0f;
    int deliveries = 0;
    auto subscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(sentinel), [&](const SaveLoadLifecycleProbeEvent&) { ++deliveries; });
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};
    const auto topologyBefore = RegistryStorageIds(liveWorld);

    const bool loaded = saveSystem.Load("extra-staged-component", liveWorld, customState);
    serializers.Unregister("ExtraTx");

    EXPECT_FALSE(loaded);
    EXPECT_EQ(g_candidateTopologyPrepareCalls, 0);
    EXPECT_TRUE(RegistryStorageIds(liveWorld) == topologyBefore);
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "extra-component-live-sentinel"));
    EXPECT_NEAR(liveWorld.GetComponent<Transform>(sentinel)->position.x, 919.0f, 0.0001f);
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("live"), std::string("sentinel"));
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(sentinel)),
              1u);
    Spark::EntityEventBus::Global().Publish<SaveLoadLifecycleProbeEvent>(static_cast<Spark::EventEntityID>(sentinel),
                                                                         {1});
    EXPECT_EQ(deliveries, 1);

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_HierarchyPlanFailureLeavesExactLiveStateBeforeStoragePreparation)
{
    const std::string dir = MakeTempSaveDir("hierarchy_plan_failure");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("hierarchy-plan-source");
    source.AddComponent<Transform>(sourceEntity);
    SaveMetadata metadata;
    metadata.saveName = "Hierarchy plan failure";
    EXPECT_TRUE(saveSystem.Save("hierarchy-plan", source, metadata, {{"candidate", "state"}}));

    const auto path = std::filesystem::path(dir) / "hierarchy-plan.spark_save";
    std::vector<char> bytes = ReadBytes(path);
    ASSERT_TRUE(ReplaceLengthPrefixedString(bytes, "Transform", "PlanTx"));
    ASSERT_TRUE(RefreshV4Checksum(bytes));
    ASSERT_TRUE(WriteBytes(path, bytes));

    auto& serializers = ComponentSerializerRegistry::GetInstance();
    serializers.Unregister("PlanTx");
    serializers.Register(
        "PlanTx", [](const void*) { return SerializedComponent{"PlanTx", {}}; },
        [](World& world, EntityID entity, const SerializedComponent&)
        { world.AddComponent<RetirementPlanProbe>(entity); });
    ComponentFactory::Get().Register("PlanTx", MakeProbeComponentOps<RetirementPlanProbe>(&CountRetirementPrepare));

    World liveWorld;
    const EntityID parent = liveWorld.CreateEntity("hierarchy-live-parent");
    const EntityID child = liveWorld.CreateEntity("hierarchy-live-child");
    liveWorld.AddComponent<Transform>(parent).position.x = 101.0f;
    liveWorld.AddComponent<Transform>(child).position.x = 202.0f;
    ASSERT_TRUE(liveWorld.SetParent(child, parent));
    int deliveries = 0;
    auto subscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(child), [&](const SaveLoadLifecycleProbeEvent&) { ++deliveries; });
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};
    const auto topologyBefore = RegistryStorageIds(liveWorld);

    g_retirementPrepareCalls = 0;
    g_retirementSnapshotCalls = 0;
    g_throwRetirementSnapshot = true;
    g_liveStorageBoundaryCrossed = false;
    g_snapshotObservedAfterBoundary = false;
    liveWorld.SetRetirementSnapshotProbeForTesting(&ObserveRetirementSnapshot);
    const bool loaded = saveSystem.Load("hierarchy-plan", liveWorld, customState);
    liveWorld.SetRetirementSnapshotProbeForTesting(nullptr);
    g_throwRetirementSnapshot = false;
    serializers.Unregister("PlanTx");

    EXPECT_FALSE(loaded);
    EXPECT_EQ(g_retirementSnapshotCalls, 1);
    EXPECT_EQ(g_retirementPrepareCalls, 0);
    EXPECT_FALSE(g_liveStorageBoundaryCrossed);
    EXPECT_FALSE(g_snapshotObservedAfterBoundary);
    EXPECT_TRUE(RegistryStorageIds(liveWorld) == topologyBefore);
    EXPECT_EQ(liveWorld.GetEntityCount(), 2u);
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "hierarchy-live-parent"));
    EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "hierarchy-live-child"));
    const Transform* parentTransform = liveWorld.GetComponent<Transform>(parent);
    const Transform* childTransform = liveWorld.GetComponent<Transform>(child);
    ASSERT_TRUE(parentTransform != nullptr);
    ASSERT_TRUE(childTransform != nullptr);
    EXPECT_TRUE(std::find(parentTransform->children.begin(), parentTransform->children.end(), child) !=
                parentTransform->children.end());
    EXPECT_TRUE(childTransform->parent == parent);
    EXPECT_NEAR(parentTransform->position.x, 101.0f, 0.0001f);
    EXPECT_NEAR(childTransform->position.x, 202.0f, 0.0001f);
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("live"), std::string("sentinel"));
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(child)),
              1u);
    Spark::EntityEventBus::Global().Publish<SaveLoadLifecycleProbeEvent>(static_cast<Spark::EventEntityID>(child), {1});
    EXPECT_EQ(deliveries, 1);

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_HierarchyRetirementUsesOnlyPreBoundarySnapshots)
{
    const std::string dir = MakeTempSaveDir("hierarchy_snapshot_guard");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    EXPECT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceEntity = source.CreateEntity("hierarchy-guard-source");
    source.AddComponent<Transform>(sourceEntity);
    SaveMetadata metadata;
    metadata.saveName = "Hierarchy snapshot guard";
    EXPECT_TRUE(saveSystem.Save("hierarchy-guard", source, metadata, {{"loaded", "state"}}));

    const auto path = std::filesystem::path(dir) / "hierarchy-guard.spark_save";
    std::vector<char> bytes = ReadBytes(path);
    ASSERT_TRUE(ReplaceLengthPrefixedString(bytes, "Transform", "GuardTx"));
    ASSERT_TRUE(RefreshV4Checksum(bytes));
    ASSERT_TRUE(WriteBytes(path, bytes));

    auto& serializers = ComponentSerializerRegistry::GetInstance();
    serializers.Unregister("GuardTx");
    serializers.Register(
        "GuardTx", [](const void*) { return SerializedComponent{"GuardTx", {}}; },
        [](World& world, EntityID entity, const SerializedComponent&)
        { world.AddComponent<RetirementPlanProbe>(entity); });
    ComponentFactory::Get().Register("GuardTx", MakeProbeComponentOps<RetirementPlanProbe>(&MarkLiveStorageBoundary));

    World liveWorld;
    const EntityID parent = liveWorld.CreateEntity("guard-live-parent");
    const EntityID child = liveWorld.CreateEntity("guard-live-child");
    liveWorld.AddComponent<Transform>(parent);
    liveWorld.AddComponent<Transform>(child);
    ASSERT_TRUE(liveWorld.SetParent(child, parent));
    auto parentSubscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(parent), [](const SaveLoadLifecycleProbeEvent&) {});
    auto childSubscription = Spark::EntityEventBus::Global().Subscribe<SaveLoadLifecycleProbeEvent>(
        static_cast<Spark::EventEntityID>(child), [](const SaveLoadLifecycleProbeEvent&) {});
    std::unordered_map<std::string, std::string> customState = {{"live", "sentinel"}};

    g_retirementPrepareCalls = 0;
    g_retirementSnapshotCalls = 0;
    g_throwRetirementSnapshot = false;
    g_liveStorageBoundaryCrossed = false;
    g_snapshotObservedAfterBoundary = false;
    liveWorld.SetRetirementSnapshotProbeForTesting(&ObserveRetirementSnapshot);
    const bool loaded = saveSystem.Load("hierarchy-guard", liveWorld, customState);
    liveWorld.SetRetirementSnapshotProbeForTesting(nullptr);
    serializers.Unregister("GuardTx");

    EXPECT_TRUE(loaded);
    EXPECT_EQ(g_retirementSnapshotCalls, 2);
    EXPECT_EQ(g_retirementPrepareCalls, 1);
    EXPECT_TRUE(g_liveStorageBoundaryCrossed);
    EXPECT_FALSE(g_snapshotObservedAfterBoundary);
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
    const EntityID incoming = FindNamedEntity(liveWorld, "hierarchy-guard-source");
    ASSERT_TRUE(incoming != entt::null);
    EXPECT_TRUE(liveWorld.HasComponent<RetirementPlanProbe>(incoming));
    EXPECT_EQ(customState.size(), 1u);
    EXPECT_EQ(customState.at("loaded"), std::string("state"));
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(parent)),
              0u);
    EXPECT_EQ(Spark::EntityEventBus::Global().HandlerCount<SaveLoadLifecycleProbeEvent>(
                  static_cast<Spark::EventEntityID>(child)),
              0u);

    std::filesystem::remove_all(dir);
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
        ASSERT_TRUE(RefreshV4Checksum(malformed));
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
    ASSERT_TRUE(RefreshV4Checksum(malformed));
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

TEST(SaveMigration_PreviousVersionInMemoryStepIsExactIdempotentAndTransactional)
{
    // OD-03: N-1 (v3) migrates to N (v4). v4 changes only the disk envelope, so
    // every declared field and record carries over exactly.
    SaveData previous;
    previous.metadata.version = kOldestSupportedSaveVersion;
    previous.metadata.saveName = "Previous-version snapshot";
    previous.metadata.screenshotPath = "Screenshots/previous.png";
    previous.customState["declared"] = "preserved";
    SerializedEntity entity{};
    entity.name = "previous-entity";
    SerializedComponent transform;
    transform.typeName = "Transform";
    transform.properties[kTransformParentProperty] = kTransformParentNone;
    entity.components.push_back(transform);
    previous.entities.push_back(entity);

    ASSERT_EQ(kOldestSupportedSaveVersion + 1, kCurrentSaveVersion);
    EXPECT_TRUE(SaveSystem::MigrateToCurrentVersion(previous));
    EXPECT_EQ(previous.metadata.version, kCurrentSaveVersion);
    EXPECT_EQ(previous.metadata.saveName, std::string("Previous-version snapshot"));
    EXPECT_EQ(previous.metadata.screenshotPath, std::string("Screenshots/previous.png"));
    EXPECT_EQ(previous.customState.at("declared"), std::string("preserved"));
    ASSERT_EQ(previous.entities.size(), 1u);
    ASSERT_EQ(previous.entities[0].components.size(), 1u);
    EXPECT_EQ(previous.entities[0].components[0].properties.at(kTransformParentProperty),
              std::string(kTransformParentNone));

    const SaveData onceMigrated = previous;
    EXPECT_TRUE(SaveSystem::MigrateToCurrentVersion(previous));
    EXPECT_EQ(previous.metadata.version, onceMigrated.metadata.version);
    EXPECT_EQ(previous.metadata.screenshotPath, onceMigrated.metadata.screenshotPath);
    EXPECT_EQ(previous.customState.size(), onceMigrated.customState.size());

    // Older than N-1 and newer than N are outside the window and stay untouched.
    for (const uint32_t outsideWindow : {kOldestSupportedSaveVersion - 1, kCurrentSaveVersion + 1})
    {
        SaveData unsupported = onceMigrated;
        unsupported.metadata.version = outsideWindow;
        unsupported.metadata.saveName = "outside-window-sentinel";
        EXPECT_FALSE(SaveSystem::MigrateToCurrentVersion(unsupported));
        EXPECT_EQ(unsupported.metadata.version, outsideWindow);
        EXPECT_EQ(unsupported.metadata.saveName, std::string("outside-window-sentinel"));
    }
}

namespace
{
    /// Copies an immutable pre-N-1 fixture into a slot and proves that every read
    /// path refuses it without touching caller state, the slot, or the fixture.
    void ExpectRetiredFixtureFailsClosed(const char* fixtureName, size_t expectedBytes, uint32_t expectedVersion,
                                         const char* slotName)
    {
        const auto fixturePath = std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "Tests" / "Fixtures" / "Compatibility" /
                                 "SaveSystem" / fixtureName;
        const std::string fixtureBefore = ReadTextFile(fixturePath);
        const std::vector<char> legacyBytes = DecodeHexFixture(fixtureBefore);
        ASSERT_EQ(legacyBytes.size(), expectedBytes);

        const std::string dir = MakeTempSaveDir(slotName);
        const auto slotPath = std::filesystem::path(dir) / (std::string(slotName) + ".spark_save");
        ASSERT_TRUE(WriteBytes(slotPath, legacyBytes));

        SaveSystem& saveSystem = SaveSystem::GetInstance();
        saveSystem.SetFileCache(nullptr);
        EXPECT_TRUE(saveSystem.Initialize(dir));
        EXPECT_EQ(ReadHeaderVersion(slotPath), expectedVersion);
        EXPECT_TRUE(expectedVersion < kOldestSupportedSaveVersion);

        SaveMetadata metadata;
        metadata.saveName = "metadata-sentinel";
        metadata.version = 77u;
        EXPECT_FALSE(saveSystem.GetSaveMetadata(slotName, metadata));
        EXPECT_EQ(metadata.saveName, std::string("metadata-sentinel"));
        EXPECT_EQ(metadata.version, 77u);

        World liveWorld;
        liveWorld.CreateEntity("live-sentinel");
        std::unordered_map<std::string, std::string> customState = {{"sentinel", "unchanged-on-failure"}};
        EXPECT_FALSE(saveSystem.Load(slotName, liveWorld, customState));
        EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
        EXPECT_TRUE(WorldContainsNamedEntity(liveWorld, "live-sentinel"));
        EXPECT_EQ(customState.size(), 1u);
        EXPECT_EQ(customState.at("sentinel"), std::string("unchanged-on-failure"));

        // Refusing the file never rewrites or quarantines the player's data.
        EXPECT_EQ(ReadHeaderVersion(slotPath), expectedVersion);
        EXPECT_TRUE(ReadBytes(slotPath) == legacyBytes);
        EXPECT_EQ(ReadTextFile(fixturePath), fixtureBefore);

        std::filesystem::remove_all(dir);
    }
} // namespace

TEST(SaveMigration_ImmutableV1FixtureIsOutsideTheWindowAndFailsClosed)
{
    // OD-03 reads only N and N-1; v1 (N-3) must be refused, not migrated.
    ExpectRetiredFixtureFailsClosed("v1-screenshotless.spark_save.hex", 284, 1u, "retired-v1");
}

TEST(SaveMigration_ImmutableV2FixtureIsOutsideTheWindowAndFailsClosed)
{
    ExpectRetiredFixtureFailsClosed("v2-screenshot-without-hierarchy.spark_save.hex", 307, 2u, "retired-v2");
}

TEST(SaveMigration_ImmutableV3FixtureLoadsWithoutRewritingSourceOrSlot)
{
    const auto fixturePath = std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "Tests" / "Fixtures" / "Compatibility" /
                             "SaveSystem" / "v3-fps-profile.spark_save.hex";
    const std::string fixtureBefore = ReadTextFile(fixturePath);
    const std::vector<char> legacyBytes = DecodeHexFixture(fixtureBefore);
    ASSERT_EQ(legacyBytes.size(), static_cast<size_t>(355));

    const std::string dir = MakeTempSaveDir("v3_fixture");
    const auto slotPath = std::filesystem::path(dir) / "legacy-v3.spark_save";
    ASSERT_TRUE(WriteBytes(slotPath, legacyBytes));

    SaveSystem& saveSystem = SaveSystem::GetInstance();
    saveSystem.SetFileCache(nullptr);
    EXPECT_TRUE(saveSystem.Initialize(dir));
    EXPECT_EQ(ReadHeaderVersion(slotPath), 3u);

    SaveMetadata metadata;
    EXPECT_TRUE(saveSystem.GetSaveMetadata("legacy-v3", metadata));
    EXPECT_EQ(metadata.version, kCurrentSaveVersion);
    EXPECT_EQ(metadata.saveName, std::string("Quick Save"));
    EXPECT_EQ(metadata.sceneName, std::string("combat_arena"));

    World loadedWorld;
    loadedWorld.CreateEntity("must-be-replaced-only-on-success");
    std::unordered_map<std::string, std::string> customState = {{"sentinel", "replace-on-success"}};
    EXPECT_TRUE(saveSystem.Load("legacy-v3", loadedWorld, customState));
    EXPECT_EQ(loadedWorld.GetEntityCount(), 0u);
    EXPECT_EQ(customState.at("fps.profile.xp"), std::string("37"));
    EXPECT_EQ(customState.at("fps.profile.level"), std::string("1"));
    EXPECT_EQ(customState.at("fps.profile.weapon"), std::string("7"));

    // The FPS module's own persisted schema inside the N-1 engine save is also in
    // its window, so the production profile reader restores the declared state.
    Spark::FPSLocalProfile profile;
    std::string profileError;
    EXPECT_TRUE(profile.ReadFrom(customState, profileError));
    EXPECT_EQ(profileError, std::string());
    EXPECT_EQ(profile.version, Spark::FPSLocalProfile::kVersion);
    EXPECT_EQ(profile.progressionXP, 37);
    EXPECT_EQ(profile.weapon, 7);

    EXPECT_EQ(ReadHeaderVersion(slotPath), 3u);
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
    ASSERT_TRUE(RefreshV4Checksum(bytes));
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
    ASSERT_TRUE(RefreshV4Checksum(bytes));
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
