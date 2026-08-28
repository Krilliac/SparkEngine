/**
 * @file SaveSystemTypes.h
 * @brief Data types used by the save/load system (metadata, serialized entities, save data).
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * This header defines the plain data structures used to represent serialized
 * game state: SaveMetadata, SerializedComponent, SerializedEntity, and SaveData.
 * These types are separated from SaveSystem.h so that code needing only the
 * data definitions does not pull in the full SaveSystem class.
 */

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

    /** @brief Oldest save format that this build can migrate and load. */
    inline constexpr uint32_t kOldestSupportedSaveVersion = 1;

    /** @brief Save format emitted by every writer in this build. */
    inline constexpr uint32_t kCurrentSaveVersion = 2;

    /**
     * @brief Shared structural limits for both disk-backed and in-memory saves.
     *
     * The binary format uses uint16 length/count fields below the entity and
     * custom-state layers. Aggregate limits are derived from the 512 MiB wire
     * budget and the four-byte minimum encoding of a component/property record.
     * Keeping these values in the representation layer prevents WriteToFile(),
     * ReadFromFile(), and DeserializeWorld() from accepting different shapes.
     */
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

    /**
     * @brief Overflow-safe aggregate accounting for a serialized save shape.
     *
     * This small value type is also useful to validate synthetic boundary cases
     * without allocating a maximum-sized SaveData in a test.
     */
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

    /**
 * @brief Lightweight metadata header attached to every save file.
 *
 * SaveMetadata is written in the versioned binary save header and returned by
 * slot-enumeration APIs such as GetSaveSlots(), which read only that header.
 *
 * ### Fields for slot-selection UI
 * The fields `saveName`, `screenshotPath`, `playerHealth`, `playerKills`, and
 * `playTime` are primarily intended for the save/load screen. Populate them before
 * passing the struct to Save() or AutoSave().
 *
 * ### Versioning
 * The `version` field is incremented whenever the save format changes. The load
 * path checks this value and may run migration routines before deserializing
 * components. Writers always replace this value with kCurrentSaveVersion. Older
 * values are only valid on parsed or manually constructed migration inputs.
 *
 * @code
 *   SaveMetadata meta;
 *   meta.saveName       = "After Tutorial";
 *   meta.sceneName      = world.GetCurrentSceneName();
 *   meta.playerClass    = player.GetClassName();
 *   meta.playTime       = g_totalPlayTime;
 *   meta.playerHealth   = player.GetHealth();
 *   meta.playerPosition = player.GetPosition();
 *   meta.playerKills    = stats.kills;
 *   meta.playerDeaths   = stats.deaths;
 *   ss.Save("slot1", world, meta);
 * @endcode
 */
    struct SaveMetadata
    {
        /** @brief Human-readable name shown in the save-slot UI (e.g. "Before Boss Fight"). */
        std::string saveName;

        /**
     * @brief Internal scene/level identifier used to reload the correct level geometry.
     *
     * Should match the scene file name without extension (e.g. "Level03").
     * The SceneManager uses this string when deserializing to load the level
     * before restoring entity state.
     */
        std::string sceneName;

        /**
     * @brief Name of the player's chosen class at save time.
     *
     * Stored for UI display only (e.g. "Soldier", "Engineer"). The actual class
     * component data is restored from the serialized entity list.
     */
        std::string playerClass;

        /**
     * @brief Save format version number.
     *
     * Defaults to kCurrentSaveVersion. The loader accepts the inclusive range
     * kOldestSupportedSaveVersion..kCurrentSaveVersion and migrates older data in
     * memory before restoring a world. Do not modify this field in game code.
     */
        uint32_t version = kCurrentSaveVersion;

        /**
     * @brief Unix timestamp (seconds since epoch) when the save was created.
     *
     * Set automatically by the SaveSystem to `time(nullptr)` at save time.
     * The UI can convert this to a human-readable date string.
     */
        uint64_t timestamp = 0;

        /**
     * @brief Total accumulated play time in seconds at save time.
     *
     * Populate from your game's running play-time counter before calling Save().
     * Displayed in the slot UI as "Played: 2h 34m".
     */
        float playTime = 0.0f;

        /**
     * @brief Relative or absolute path to a PNG/JPG screenshot for the slot thumbnail.
     *
     * The SaveSystem does **not** capture or write the screenshot; your game code
     * must capture it (e.g. via a back-buffer readback) and set this path. Leave
     * empty to display the engine's placeholder thumbnail.
     */
        std::string screenshotPath;

        // -------------------------------------------------------------------------
        // Player summary fields — used by the save-slot UI for quick display.
        // -------------------------------------------------------------------------

        /** @brief Player's health value at save time. Range: [0, maxHealth]. */
        float playerHealth = 0.0f;

        /** @brief Player's armor/shield value at save time. Range: [0, maxArmor]. */
        float playerArmor = 0.0f;

        /**
     * @brief Player's world-space position at save time.
     *
     * Shown in the UI as a map pin or coordinate display. Also used by the
     * SceneManager as a hint for where to spawn the player during load.
     */
        DirectX::XMFLOAT3 playerPosition{0, 0, 0};

        /** @brief Total enemy kills accumulated up to this save point. */
        int playerKills = 0;

        /** @brief Total player deaths accumulated up to this save point. */
        int playerDeaths = 0;
    };

    /**
 * @brief Serialized representation of a single component instance.
 *
 * SerializedComponent is an intermediate, type-erased container that bridges
 * the strongly-typed component structs (e.g. `Transform`, `HealthComponent`)
 * and the type-erased binary representation on disk.
 *
 * All field values are stored as **strings** so they can be written directly to
 * the common binary property map without type-specific code in the core system. Each
 * component's registered serializer is responsible for encoding and decoding its
 * fields (e.g. converting `XMFLOAT3{1, 2, 3}` to the string `"1.0 2.0 3.0"`).
 *
 * ### Example serialized Transform component
 * ```json
 * {
 *   "typeName": "Transform",
 *   "properties": {
 *     "posX": "1.0", "posY": "0.5", "posZ": "-3.0",
 *     "rotX": "0.0", "rotY": "45.0", "rotZ": "0.0",
 *     "scaleX": "1.0", "scaleY": "1.0", "scaleZ": "1.0"
 *   }
 * }
 * ```
 */
    struct SerializedComponent
    {
        /**
     * @brief C++ type name of the component (e.g. "Transform", "HealthComponent").
     *
     * Must match the key used when registering serializers with
     * ComponentSerializerRegistry::Register(). The deserializer uses this string
     * to look up the correct deserialization function.
     */
        std::string typeName;

        /**
     * @brief String-encoded key-value properties for this component instance.
     *
     * Both keys and values are plain strings. The component serializer encodes
     * typed values (floats, ints, booleans, vectors) to strings, and the
     * deserializer parses them back. Use a consistent encoding convention within
     * each component serializer (e.g. `std::to_string` for numerics).
     */
        std::unordered_map<std::string, std::string> properties;
    };

    /**
 * @brief Serialized representation of a single entity and all its components.
 *
 * SerializedEntity is a snapshot of one ECS entity at save time. The
 * `entityID` stored here is **not** guaranteed to match the entity's ID when
 * loaded, because EnTT may recycle IDs. The ID is stored purely for reference
 * (e.g. resolving entity cross-references in custom-state data).
 *
 * During deserialization, the SaveSystem creates a brand-new entity via
 * `World::CreateEntity()` and attaches each component listed in `components`.
 */
    struct SerializedEntity
    {
        /**
     * @brief EnTT entity ID at save time.
     *
     * Informational only during load; do not use as a stable cross-save reference.
     * If you need stable inter-entity references, store a named identifier in a
     * custom component and reference it via name in `customState`.
     */
        uint32_t entityID;

        /**
     * @brief Human-readable entity name from NameComponent (if present).
     *
     * Used to create the entity with the same display name in the editor and
     * in debug logs. Empty string if the entity had no NameComponent.
     */
        std::string name;

        /**
     * @brief All serialized components attached to this entity.
     *
     * Populated by iterating over the entity's component set at save time and
     * calling the registered serializer for each type. Only components with
     * registered serializers are included; unrecognized components are silently
     * skipped.
     */
        std::vector<SerializedComponent> components;
    };

    /**
 * @brief Complete snapshot of a game state that can be written to and read from disk.
 *
 * SaveData is the root container passed to `SaveSystem::WriteToFile()` and
 * returned by `SaveSystem::ReadFromFile()`. It contains everything needed to
 * reconstruct the game world from scratch: the metadata header, the full entity
 * list, and any free-form key-value state for game-specific data that doesn't
 * fit into ECS components.
 *
 * ### Creating a SaveData manually
 * In most cases you should call `SaveSystem::Save()` rather than constructing
 * SaveData directly. However, `SaveSystem::SerializeWorld()` returns a SaveData
 * without writing to disk, which is useful for in-memory snapshots (e.g. undo
 * systems or server-side checkpointing).
 *
 * @code
 *   // Take an in-memory snapshot without touching the file system
 *   SaveData snapshot = SaveSystem::GetInstance().SerializeWorld(world, meta);
 *   // ... modify world state ...
 *   // Restore from snapshot
 *   SaveSystem::GetInstance().DeserializeWorld(snapshot, world);
 * @endcode
 */
    struct SaveData
    {
        /**
     * @brief Metadata header for this save (slot UI display, versioning, scene name).
     *
     * See SaveMetadata for field descriptions. Always populate this before
     * writing; the SaveSystem reads `metadata.version` to select the appropriate
     * migration path during load.
     */
        SaveMetadata metadata;

        /**
     * @brief Serialized snapshot of all entities and their components.
     *
     * One entry per entity in the World that has at least one serializable
     * component. Entities with no registered-component types are omitted.
     */
        std::vector<SerializedEntity> entities;

        /**
     * @brief Free-form key-value store for game-specific state.
     *
     * Use this for data that doesn't map cleanly to ECS components: global game
     * mode flags, world event triggers, puzzle states, timer values, etc.
     *
     * @code
     *   data.customState["doorOpened_MainHall"] = "true";
     *   data.customState["questStep_FindKey"]   = "3";
     *   data.customState["globalTimer"]         = std::to_string(elapsedSeconds);
     * @endcode
     */
        std::unordered_map<std::string, std::string> customState;
    };

} // namespace Spark
