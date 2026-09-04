/// @file SaveSystemTypes.h
/// @brief Data types used by the save/load system (metadata, serialized entities, save data).

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Spark
{

    /// Oldest save format that this build can migrate and load.
    inline constexpr uint32_t kOldestSupportedSaveVersion = 1;

    /// Save format emitted by every writer in this build.
    inline constexpr uint32_t kCurrentSaveVersion = 2;

    /// Shared structural limits for both disk-backed and in-memory saves.
    /// The binary format uses uint16 length/count fields below the entity and
    /// custom-state layers. Aggregate limits derive from the 512 MiB wire budget.
    struct SaveRepresentationLimits
    {
        static constexpr size_t maxWireBytes = 512ull * 1024ull * 1024ull;
        static constexpr size_t maxMetadataBytes = 64ull * 1024ull;
        static constexpr size_t maxStringBytes = std::numeric_limits<uint16_t>::max();
        static constexpr size_t maxEntities = 1'000'000;
        static constexpr size_t maxComponentsPerEntity = std::numeric_limits<uint16_t>::max();
        static constexpr size_t maxPropertiesPerComponent = std::numeric_limits<uint16_t>::max();
        static constexpr size_t maxCustomStateEntries = 100'000;
        static constexpr size_t minimumComponentWireBytes = sizeof(uint16_t) + sizeof(uint16_t);
        static constexpr size_t minimumPropertyWireBytes = sizeof(uint16_t) + sizeof(uint16_t);
        static constexpr size_t maxTotalComponents = maxWireBytes / minimumComponentWireBytes;
        static constexpr size_t maxTotalProperties = maxWireBytes / minimumPropertyWireBytes;

        [[nodiscard]] static constexpr bool SupportsStringBytes(size_t count) noexcept
        {
            return count <= maxStringBytes;
        }
        [[nodiscard]] static constexpr bool SupportsMetadataBytes(size_t count) noexcept
        {
            return count <= maxMetadataBytes;
        }
        [[nodiscard]] static constexpr bool SupportsWireBytes(size_t count) noexcept { return count <= maxWireBytes; }
        [[nodiscard]] static constexpr bool SupportsEntityCount(size_t count) noexcept { return count <= maxEntities; }
        [[nodiscard]] static constexpr bool SupportsComponentCount(size_t count) noexcept
        {
            return count <= maxComponentsPerEntity;
        }
        [[nodiscard]] static constexpr bool SupportsPropertyCount(size_t count) noexcept
        {
            return count <= maxPropertiesPerComponent;
        }
        [[nodiscard]] static constexpr bool SupportsCustomStateCount(size_t count) noexcept
        {
            return count <= maxCustomStateEntries;
        }
    };

    /// Overflow-safe aggregate accounting for a serialized save shape.
    struct SaveRepresentationBudget
    {
        size_t wireBytes = 0;
        size_t totalComponents = 0;
        size_t totalProperties = 0;
        size_t totalCustomStateEntries = 0;

        [[nodiscard]] constexpr bool AddWireBytes(size_t count) noexcept
        {
            return Accumulate(wireBytes, count, SaveRepresentationLimits::maxWireBytes);
        }
        [[nodiscard]] constexpr bool AddComponents(size_t count) noexcept
        {
            return Accumulate(totalComponents, count, SaveRepresentationLimits::maxTotalComponents);
        }
        [[nodiscard]] constexpr bool AddProperties(size_t count) noexcept
        {
            return Accumulate(totalProperties, count, SaveRepresentationLimits::maxTotalProperties);
        }
        [[nodiscard]] constexpr bool AddCustomStateEntries(size_t count) noexcept
        {
            return Accumulate(totalCustomStateEntries, count, SaveRepresentationLimits::maxCustomStateEntries);
        }

      private:
        [[nodiscard]] static constexpr bool Accumulate(size_t& total, size_t count, size_t limit) noexcept
        {
            if (total > limit || count > limit - total)
                return false;
            total += count;
            return true;
        }
    };

    // ============================================================================
    // Save Data Types
    // ============================================================================

    /// Lightweight metadata header attached to every save file.
    /// Written in the versioned binary save header and returned by slot-enumeration APIs.
    struct SaveMetadata
    {
        /// Human-readable name shown in the save-slot UI (e.g. "Before Boss Fight").
        std::string saveName;

        /// Internal scene/level identifier for reloading (e.g. "Level03").
        std::string sceneName;

        /// Player's class name at save time, for UI display only.
        std::string playerClass;

        /// Save format version. Writers replace with kCurrentSaveVersion; loaders accept
        /// kOldestSupportedSaveVersion..kCurrentSaveVersion and migrate in memory.
        uint32_t version = kCurrentSaveVersion;

        /// Unix timestamp (seconds since epoch) when the save was created.
        uint64_t timestamp = 0;

        /// Total accumulated play time in seconds at save time.
        float playTime = 0.0f;

        /// Path to a PNG/JPG screenshot for the slot thumbnail. Empty = placeholder.
        std::string screenshotPath;

        /// Player's health value at save time. Range: [0, maxHealth].
        float playerHealth = 0.0f;

        /// Player's armor/shield value at save time. Range: [0, maxArmor].
        float playerArmor = 0.0f;

        /// Player's world-space position at save time.
        DirectX::XMFLOAT3 playerPosition{0, 0, 0};

        /// Total enemy kills accumulated up to this save point.
        int playerKills = 0;

        /// Total player deaths accumulated up to this save point.
        int playerDeaths = 0;
    };

    /// Serialized representation of a single component instance.
    /// Type-erased bridge between strongly-typed component structs and the binary format.
    struct SerializedComponent
    {
        /// C++ type name of the component (must match ComponentSerializerRegistry key).
        std::string typeName;

        /// String-encoded key-value properties for this component instance.
        std::unordered_map<std::string, std::string> properties;
    };

    /// Serialized representation of a single entity and all its components.
    struct SerializedEntity
    {
        /// EnTT entity ID at save time (informational only; IDs may be recycled on load).
        uint32_t entityID;

        /// Human-readable entity name from NameComponent (empty if none).
        std::string name;

        /// All serialized components attached to this entity.
        std::vector<SerializedComponent> components;
    };

    /// Complete snapshot of a game state for disk or in-memory persistence.
    /// Root container for SaveSystem::WriteToFile() and ReadFromFile().
    struct SaveData
    {
        /// Metadata header (slot UI display, versioning, scene name).
        SaveMetadata metadata;

        /// Serialized snapshot of all entities with at least one serializable component.
        std::vector<SerializedEntity> entities;

        /// Free-form key-value store for game-specific state not mapped to ECS components.
        std::unordered_map<std::string, std::string> customState;
    };

} // namespace Spark
