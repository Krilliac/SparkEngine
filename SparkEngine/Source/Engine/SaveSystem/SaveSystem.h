/**
 * @file SaveSystem.h
 * @brief Game state serialization and save/load system for Spark Engine.
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * SaveSystem provides complete, ECS-aware game state persistence. It serializes
 * the entire World (entities + components) to disk as compressed JSON, manages
 * multiple save slots, and supports quicksave, quickload, and rotating autosave.
 *
 * ## Architecture
 *
 * The system is composed of four main pieces:
 *
 * | Type                         | Role                                                     |
 * |------------------------------|----------------------------------------------------------|
 * | SaveMetadata                 | Lightweight header written with every save (slot UI)     |
 * | SaveData                     | Full serialized snapshot: metadata + all entities        |
 * | ComponentSerializerRegistry  | Registry of per-component-type (de)serializers           |
 * | SaveSystem                   | Singleton façade – orchestrates save/load operations     |
 *
 * ## Serialization format
 *
 * Save files are written as JSON compressed with the **miniz** (zlib) deflate
 * algorithm. The JSON root object contains:
 * - `"metadata"` – SaveMetadata fields
 * - `"entities"` – array of SerializedEntity objects
 * - `"customState"` – arbitrary string key-value pairs for game-specific data
 *
 * ## Component registration
 *
 * Each engine component type must register a serialize/deserialize function pair
 * with the ComponentSerializerRegistry via `Register()`. Built-in component types
 * (Transform, HealthComponent, RigidBodyComponent, etc.) are registered
 * automatically by `ComponentSerializerRegistry::RegisterBuiltins()`, which is
 * called during engine startup.
 *
 * Game code can add custom components via:
 * @code
 *   ComponentSerializerRegistry::GetInstance().Register(
 *       "MyComponent",
 *       [](const void* comp) -> SerializedComponent { ... },
 *       [](World& world, EntityID e, const SerializedComponent& d) { ... });
 * @endcode
 *
 * ## Save slot naming
 *
 * Slot names are arbitrary strings used as file-system-safe base names (e.g.
 * `"slot1"`, `"checkpoint_02"`). Two special slots are reserved internally:
 * - `"__quicksave"` – used by QuickSave()/QuickLoad()
 * - `"__autosave_N"` – rotating autosave slots (0..maxAutoSaves-1)
 *
 * ## Typical usage
 * @code
 *   SaveSystem& ss = SaveSystem::GetInstance();
 *   ss.Initialize("Saves");          // create Saves/ directory if absent
 *
 *   // Save
 *   SaveMetadata meta;
 *   meta.saveName    = "Before Boss Fight";
 *   meta.sceneName   = "Level03";
 *   meta.playerClass = "Soldier";
 *   meta.playTime    = totalPlaySeconds;
 *   ss.Save("slot1", world, meta);
 *
 *   // Load
 *   if (ss.SaveExists("slot1"))
 *       ss.Load("slot1", world);
 * @endcode
 */

#pragma once
#include "../../Core/Platform.h"
#include "../../Utils/Assert.h"

#include "../ECS/Components.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <chrono>
#include <cstdint>

namespace Spark
{

    class LocalFileCache;

    // ============================================================================
    // Save Data Types
    // ============================================================================

    /**
 * @brief Lightweight metadata header attached to every save file.
 *
 * SaveMetadata is written both inside the compressed save file and as part of
 * any slot-enumeration API (GetSaveSlots()) so the UI can display useful
 * information without decompressing the full save.
 *
 * ### Fields for slot-selection UI
 * The fields `saveName`, `screenshotPath`, `playerHealth`, `playerKills`, and
 * `playTime` are primarily intended for the save/load screen. Populate them before
 * passing the struct to Save() or AutoSave().
 *
 * ### Versioning
 * The `version` field is incremented whenever the save format changes. The load
 * path checks this value and may run migration routines before deserializing
 * components. Always leave `version` at its default (1) unless you are writing a
 * migration pass.
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
     * Defaults to 1. Increment this when the JSON schema changes so that the
     * loader can distinguish old saves and apply migrations. Do not modify this
     * field in game code unless you are implementing a migration.
     */
        uint32_t version = 1;

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
 * and the JSON representation on disk.
 *
 * All field values are stored as **strings** so they can be written directly to
 * JSON without type-specific serialization code in the core system. Each
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

    // ============================================================================
    // Component Serializer Registry
    // ============================================================================

    /**
 * @class ComponentSerializerRegistry
 * @brief Singleton registry mapping component type names to (de)serializer functions.
 *
 * Each component type that should participate in save/load must be registered
 * exactly once with a pair of functions:
 * - **SerializeFunc** – converts a `const void*` (pointing to the concrete
 *   component struct) into a SerializedComponent (string properties).
 * - **DeserializeFunc** – reads a SerializedComponent and calls
 *   `world.AddComponent<T>(entity, ...)` to restore the component.
 *
 * The `void*` parameter allows the registry to remain type-agnostic; callers
 * must cast the pointer to the correct concrete type inside their lambda.
 *
 * ### Built-in registration
 * All Spark Engine built-in components are registered by `RegisterBuiltins()`,
 * which is called automatically during SaveSystem::Initialize(). Game code only
 * needs to call `Register()` for custom component types.
 *
 * ### Thread safety
 * Register() should be called during single-threaded initialization. Subsequent
 * Serialize() / Deserialize() calls are read-only (after registration) and safe
 * to call from multiple threads, provided no new registrations occur concurrently.
 *
 * @code
 *   // Register a custom "LootComponent"
 *   ComponentSerializerRegistry::GetInstance().Register(
 *       "LootComponent",
 *       [](const void* comp) -> SerializedComponent {
 *           const auto* loot = static_cast<const LootComponent*>(comp);
 *           SerializedComponent sc;
 *           sc.typeName = "LootComponent";
 *           sc.properties["itemID"]  = std::to_string(loot->itemID);
 *           sc.properties["quantity"] = std::to_string(loot->quantity);
 *           return sc;
 *       },
 *       [](World& world, EntityID e, const SerializedComponent& d) {
 *           LootComponent loot;
 *           loot.itemID   = std::stoi(d.properties.at("itemID"));
 *           loot.quantity = std::stoi(d.properties.at("quantity"));
 *           world.AddComponent<LootComponent>(e, loot);
 *       });
 * @endcode
 */
    class ComponentSerializerRegistry
    {
      public:
        /**
     * @brief Function signature for component serialization.
     *
     * @param component  Const void pointer to the component struct. Cast to
     *                   `const ConcreteType*` inside the lambda.
     * @return           SerializedComponent containing all field values as strings.
     */
        using SerializeFunc = std::function<SerializedComponent(const void* component)>;

        /**
     * @brief Function signature for component deserialization.
     *
     * @param world   The World into which the component should be added.
     * @param entity  The entity to attach the component to.
     * @param data    SerializedComponent containing the string properties to parse.
     */
        using DeserializeFunc = std::function<void(World& world, EntityID entity, const SerializedComponent& data)>;

        /**
     * @brief Access the singleton instance of the registry.
     *
     * Constructed on first call (Meyer's singleton). Safe to call from any thread
     * after the first call has completed (guaranteed by C++11 static initialization).
     *
     * @return  Reference to the global ComponentSerializerRegistry instance.
     */
        static ComponentSerializerRegistry& GetInstance();

        /**
     * @brief Register a (de)serializer pair for a component type.
     *
     * Overwrites any existing registration for `typeName`. Call this once per
     * component type during engine/game initialization, before any Save or Load
     * operations.
     *
     * @param typeName     Unique string identifier for this component type. Must match
     *                     the `typeName` field written by the serializer.
     * @param serialize    Function that converts `const void*` → SerializedComponent.
     * @param deserialize  Function that restores the component from SerializedComponent.
     */
        void Register(const std::string& typeName, SerializeFunc serialize, DeserializeFunc deserialize);

        /**
     * @brief Check whether a serializer is registered for the given type name.
     *
     * @param typeName  Component type name to query.
     * @return          `true` if a serializer/deserializer pair exists for `typeName`.
     */
        bool HasSerializer(const std::string& typeName) const;

        /**
     * @brief Serialize a component using its registered serializer.
     *
     * @param typeName   Component type name (must have been registered).
     * @param component  Const void pointer to the component data.
     * @return           SerializedComponent populated by the registered serializer.
     *
     * @pre HasSerializer(typeName) must be true.
     */
        SerializedComponent Serialize(const std::string& typeName, const void* component) const;

        /**
     * @brief Deserialize a component and attach it to an entity.
     *
     * Looks up the registered deserializer for `data.typeName` and invokes it.
     *
     * @param typeName   Component type name (must have been registered).
     * @param world      ECS World that owns the entity.
     * @param entity     Entity to attach the restored component to.
     * @param data       SerializedComponent to deserialize.
     *
     * @pre HasSerializer(typeName) must be true.
     */
        void Deserialize(const std::string& typeName, World& world, EntityID entity,
                         const SerializedComponent& data) const;

        /**
     * @brief Register all built-in Spark Engine component (de)serializers.
     *
     * Covers: Transform, NameComponent, HealthComponent, RigidBodyComponent,
     * MeshRenderer, Camera, AudioSourceComponent, LightComponent,
     * AnimationController, AIComponent, and more. Called automatically by
     * SaveSystem::Initialize(); do not call manually unless reinitializing.
     */
        void RegisterBuiltins();

      private:
        /** @brief Private constructor enforces singleton pattern. */
        ComponentSerializerRegistry() = default;

        /**
     * @brief Internal storage node pairing a serializer with its deserializer.
     */
        struct Entry
        {
            /** @brief Function that converts component data to string properties. */
            SerializeFunc serialize;
            /** @brief Function that restores a component from string properties. */
            DeserializeFunc deserialize;
        };

        /**
     * @brief Map from component type name to its registered Entry.
     *
     * Keyed by the same `typeName` string used in SerializedComponent::typeName.
     */
        std::unordered_map<std::string, Entry> m_serializers;
    };

    // ============================================================================
    // SaveSystem
    // ============================================================================

    /**
 * @class SaveSystem
 * @brief Singleton façade that orchestrates all save and load operations.
 *
 * SaveSystem provides the primary API for persisting and restoring game state.
 * It coordinates the ComponentSerializerRegistry, JSON serialization via RapidJSON,
 * and deflate compression via miniz to produce compact, portable save files.
 *
 * ### Save file location
 * All files are written to the directory specified in Initialize() (default: `"Saves/"`
 * relative to the working directory). Each save slot produces a single `.sav` file
 * named after the slot (e.g. `"Saves/slot1.sav"`).
 *
 * ### Quicksave and autosave
 * - **QuickSave/QuickLoad**: single dedicated `"__quicksave"` slot; always overwrites.
 * - **AutoSave**: rotates through `maxAutoSaves` slots (default: 3). The oldest slot
 *   is overwritten when all slots are used.
 *
 * ### Error handling
 * All public save/load methods return `bool` indicating success. On failure an error
 * message is logged via spdlog. The world is left in an indeterminate state if Load()
 * fails partway through; callers should handle this (e.g. reload the level).
 *
 * ### Thread safety
 * SaveSystem is **not thread-safe**. Call all methods from the main game thread.
 * For background saves, serialize the world to a SaveData (SerializeWorld()) on the
 * main thread and write to disk (WriteToFile()) from a background thread with no
 * further world access.
 *
 * @code
 *   // Engine startup
 *   SaveSystem& ss = SaveSystem::GetInstance();
 *   ss.Initialize("Saves");
 *
 *   // Save
 *   SaveMetadata meta;
 *   meta.saveName  = "Checkpoint 1";
 *   meta.sceneName = "Level01";
 *   meta.playTime  = g_playTime;
 *   ss.Save("checkpoint1", world, meta);
 *
 *   // Enumerate for UI
 *   for (const auto& m : ss.GetSaveSlots())
 *       DrawSlotButton(m.saveName, m.playTime, m.screenshotPath);
 *
 *   // Load
 *   if (ss.SaveExists("checkpoint1"))
 *       ss.Load("checkpoint1", world);
 * @endcode
 */
    class SaveSystem
    {
      public:
        /**
     * @brief Access the singleton SaveSystem instance.
     *
     * The instance is constructed on the first call (Meyer's singleton, thread-safe
     * under C++11). Always use this to obtain the SaveSystem; do not construct one
     * directly.
     *
     * @return  Reference to the global SaveSystem instance.
     */
        static SaveSystem& GetInstance();

        /**
     * @brief Initialize the save system and prepare the save directory.
     *
     * Creates the save directory if it does not exist and registers all built-in
     * component serializers. Must be called once before any Save/Load operations.
     *
     * @param saveDirectory  Path (relative or absolute) to the directory where save
     *                       files will be stored. Defaults to `"Saves"`.
     * @return               `true` on success; `false` if the directory could not be
     *                       created or the serializers could not be registered.
     */
        bool Initialize(const std::string& saveDirectory = "Saves");

        /**
     * @brief Serialize and write the current world state to the specified save slot.
     *
     * Serializes all entities and components in `world`, attaches `metadata`,
     * compresses the JSON, and writes the result to `<saveDirectory>/<slotName>.sav`.
     * If a file already exists for this slot it is overwritten atomically.
     *
     * @param slotName  Unique slot identifier (file-system-safe string, e.g. "slot1").
     *                  Must not be empty or contain path separators.
     * @param world     The ECS World to snapshot. Must remain valid for the duration
     *                  of the call.
     * @param metadata  Metadata to embed in the save file. Populate all display fields
     *                  before calling.
     * @return          `true` if the file was written successfully; `false` on any error
     *                  (serialization failure, disk full, permission denied, etc.).
     */
        bool Save(const std::string& slotName, World& world, const SaveMetadata& metadata);

        /**
     * @brief Load a previously saved game state from the specified slot.
     *
     * Reads and decompresses the save file, reconstructs all entities and components
     * in `world`, and applies any version migrations if the save format version
     * differs from the current engine version.
     *
     * @warning The provided `world` is **cleared** (all existing entities destroyed)
     *          before the saved entities are restored. Ensure no raw pointers to
     *          world entities are held by callers before invoking this method.
     *
     * @param slotName  Save slot to load (must match a slot previously written by Save()).
     * @param world     The ECS World to restore into. Existing state is cleared.
     * @return          `true` if the world was fully restored; `false` on any error
     *                  (file not found, JSON parse error, version mismatch, etc.).
     */
        bool Load(const std::string& slotName, World& world);

        /**
     * @brief Save the current game state to the dedicated quicksave slot.
     *
     * Equivalent to `Save("__quicksave", world, metadata)`. Overwrites the previous
     * quicksave. Intended for single-key quicksave functionality (e.g. F5).
     *
     * @param world     The ECS World to snapshot.
     * @param metadata  Metadata to embed; `saveName` will be set to "Quick Save"
     *                  automatically if left empty.
     * @return          `true` on success; `false` on error.
     */
        bool QuickSave(World& world, const SaveMetadata& metadata);

        /**
     * @brief Restore the game state from the dedicated quicksave slot.
     *
     * Equivalent to `Load("__quicksave", world)`. Returns `false` if no quicksave
     * exists. Intended for single-key quickload (e.g. F9).
     *
     * @param world  The ECS World to restore into. Existing state is cleared.
     * @return       `true` if quicksave was found and loaded; `false` otherwise.
     */
        bool QuickLoad(World& world);

        /**
     * @brief Save to a rotating autosave slot.
     *
     * Cycles through `maxAutoSaves` slots (named `"__autosave_0"` … `"__autosave_N"`).
     * The oldest slot is overwritten. Call this from your game's checkpoint or
     * scene-transition logic to provide automatic crash recovery.
     *
     * @param world     The ECS World to snapshot.
     * @param metadata  Metadata to embed; `saveName` is typically set to "Auto Save".
     * @return          `true` on success; `false` on error.
     */
        bool AutoSave(World& world, const SaveMetadata& metadata);

        /**
     * @brief Delete the save file for the specified slot.
     *
     * Removes `<saveDirectory>/<slotName>.sav` from the file system. A no-op if the
     * file does not exist (returns `true`). Returns `false` only if the file exists
     * but could not be deleted (e.g. permission denied).
     *
     * @param slotName  Slot to delete.
     * @return          `true` if the file was deleted (or didn't exist); `false` on error.
     */
        bool DeleteSave(const std::string& slotName);

        /**
     * @brief Return metadata for all save slots found in the save directory.
     *
     * Scans the save directory for `*.sav` files, reads the metadata header from
     * each (without fully decompressing the entity data), and returns the results
     * sorted by `timestamp` descending (most recent first).
     *
     * Use this to populate the save-slot selection UI without the overhead of
     * loading full save files.
     *
     * @return  Vector of SaveMetadata, one per discovered save file. Empty if the
     *          directory contains no valid save files.
     */
        std::vector<SaveMetadata> GetSaveSlots() const;

        /**
     * @brief Read the metadata header for a specific save slot without loading entities.
     *
     * Faster than Load() when you only need display information (e.g. to show a
     * confirmation dialog before overwriting a slot).
     *
     * @param slotName    Slot identifier to query.
     * @param outMetadata Output parameter populated on success.
     * @return            `true` if the slot exists and its metadata was read successfully;
     *                    `false` if the file doesn't exist or is corrupt.
     */
        bool GetSaveMetadata(const std::string& slotName, SaveMetadata& outMetadata) const;

        /**
     * @brief Check whether a save file exists for the given slot name.
     *
     * Performs a simple file-existence check without opening or parsing the file.
     *
     * @param slotName  Slot identifier to check.
     * @return          `true` if `<saveDirectory>/<slotName>.sav` exists on disk.
     */
        bool SaveExists(const std::string& slotName) const;

        /**
     * @brief Serialize the world to a SaveData without writing to disk.
     *
     * Useful for in-memory checkpointing (e.g. undo/redo, boss-fight snapshots) or
     * for implementing background disk-writes by serializing on the main thread and
     * offloading WriteToFile() to a worker thread.
     *
     * @param world     The ECS World to snapshot.
     * @param metadata  Metadata to embed in the returned SaveData.
     * @return          A fully populated SaveData snapshot of the current world state.
     */
        SaveData SerializeWorld(World& world, const SaveMetadata& metadata) const;

        /**
     * @brief Restore world state from a SaveData without reading from disk.
     *
     * Clears `world` and reconstructs all entities and components from `data`.
     * Pair with SerializeWorld() for in-memory snapshot/restore patterns.
     *
     * @warning Clears all existing entities in `world` before restoring.
     *
     * @param data   SaveData snapshot (e.g. from a previous SerializeWorld() call).
     * @param world  The ECS World to restore into. Existing state is cleared.
     * @return       `true` if all entities were restored successfully; `false` on error.
     */
        bool DeserializeWorld(const SaveData& data, World& world) const;

        // -------------------------------------------------------------------------
        // Configuration
        // -------------------------------------------------------------------------

        /**
     * @brief Set the maximum number of rotating autosave slots.
     *
     * When AutoSave() has been called `count` times, the oldest slot is overwritten.
     * Defaults to 3. Must be ≥ 1.
     *
     * @param count  Number of autosave slots to maintain.
     */
        void SetMaxAutoSaves(int count)
        {
            ASSERT_MSG(count >= 1, "SaveSystem::SetMaxAutoSaves — count must be >= 1");
            m_maxAutoSaves = count;
        }

        /**
     * @brief Override the directory used to store save files at runtime.
     *
     * Changing this after Initialize() has been called takes effect immediately;
     * subsequent Save/Load calls will use the new directory. The directory is
     * created if it does not exist on the next Save call.
     *
     * @param dir  New save directory path (relative or absolute).
     */
        void SetSaveDirectory(const std::string& dir) { m_saveDirectory = dir; }

        /** @brief Set the file cache for save I/O (non-owning). */
        void SetFileCache(LocalFileCache* cache) { m_fileCache = cache; }

        // -------------------------------------------------------------------------
        // Console integration
        // -------------------------------------------------------------------------

        /**
     * @brief List all save slots as a formatted string for the debug console.
     *
     * Returns a human-readable table of slot names, timestamps, scene names, and
     * play times. Bound to the console command `save list`.
     *
     * @return  Formatted multi-line string suitable for console output.
     */
        std::string Console_ListSaves() const;

        /**
     * @brief Return detailed information about a specific save slot for the console.
     *
     * Reads full metadata (without loading entities) and formats it as a human-readable
     * string. Bound to the console command `save info <slotName>`.
     *
     * @param slotName  Slot to inspect.
     * @return          Formatted string with all SaveMetadata fields, or an error message
     *                  if the slot does not exist.
     */
        std::string Console_GetSaveInfo(const std::string& slotName) const;

      private:
        /** @brief Private constructor enforces singleton pattern. */
        SaveSystem() = default;

        /**
     * @brief Compress a SaveData to JSON and write it to disk.
     *
     * Serializes `data` to JSON via RapidJSON, compresses with miniz deflate, and
     * writes the binary blob to `filepath`. Returns false on any I/O or compression error.
     *
     * @param filepath  Absolute or relative path of the output file.
     * @param data      SaveData to write.
     * @return          `true` on success; `false` on error.
     */
        bool WriteToFile(const std::string& filepath, const SaveData& data) const;

        /**
     * @brief Read, decompress, and parse a save file from disk.
     *
     * Reads the binary file at `filepath`, decompresses with miniz, and parses
     * the JSON into `outData`. Returns false if the file does not exist, cannot
     * be decompressed, or fails JSON parsing.
     *
     * @param filepath  Absolute or relative path of the input file.
     * @param outData   Output parameter populated on success.
     * @return          `true` on success; `false` on any error.
     */
        bool ReadFromFile(const std::string& filepath, SaveData& outData) const;

        /**
     * @brief Construct the full file path for a save slot.
     *
     * Returns `m_saveDirectory + "/" + slotName + ".sav"`. Does not perform any
     * file-system operations.
     *
     * @param slotName  Slot identifier.
     * @return          Full path to the corresponding `.sav` file.
     */
        std::string GetSavePath(const std::string& slotName) const;

        /** @brief Directory where all `.sav` files are stored. Defaults to `"Saves"`. */
        std::string m_saveDirectory = "Saves";

        /**
     * @brief Maximum number of rotating autosave slots.
     *
     * AutoSave() cycles through slots `0 .. m_maxAutoSaves - 1` and overwrites
     * the oldest when all are filled.
     */
        int m_maxAutoSaves = 3;

        /**
     * @brief Index of the next autosave slot to write (0 .. m_maxAutoSaves - 1).
     *
     * Incremented (mod m_maxAutoSaves) each time AutoSave() is called.
     */
        int m_currentAutoSaveIndex = 0;

        /**
     * @brief Reserved slot name for quicksave/quickload operations.
     *
     * Hard-coded to `"__quicksave"`. Do not use this name for user-facing save slots.
     */
        std::string m_quickSaveSlot = "__quicksave";

        /** @brief Optional file cache for save file I/O (non-owning). */
        LocalFileCache* m_fileCache = nullptr;
    };

} // namespace Spark
