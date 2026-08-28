/**
 * @file SaveSystem.h
 * @brief Game state serialization and save/load system for Spark Engine.
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * SaveSystem provides complete, ECS-aware game state persistence. It serializes
 * the entire World (entities + components) to disk in a compact, uncompressed
 * binary format, manages multiple save slots, and supports quicksave, quickload,
 * and rotating autosave.
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
 * Save files use a custom, uncompressed binary layout (extension `.spark_save`):
 * - A 4-byte `"SPRK"` magic followed by a `uint32` format version.
 * - A length-prefixed newline-delimited **metadata** text block (SaveMetadata fields;
 *   v2 adds `screenshotPath` after `playerClass`).
 * - A `uint32` entity count, then each **entity** as a length-prefixed name plus its
 *   components (each a length-prefixed type name and a set of length-prefixed
 *   key/value property strings).
 * - A trailing `uint32` **customState** count followed by length-prefixed key/value
 *   pairs (arbitrary game-specific data).
 *
 * String fields use `uint16` length prefixes; the writer rejects (rather than
 * truncates) any string that would overflow that prefix.
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
 * Custom types that can appear in loaded snapshots also require matching
 * ComponentFactory storage-transaction operations; built-in registrations add
 * these automatically.
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
#include "SaveSystemTypes.h"
#include "../../Utils/Assert.h"

#include "../ECS/Components.h"
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
 * which is called automatically during SaveSystem::Initialize(). Game code must
 * pair `Register()` for a custom loadable component with ComponentFactory
 * operations that include `prepareStorage`, `swapStorageContents`, and
 * `notifyRebound`.
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
         * @brief Remove a serializer registration by component type name.
         *
         * Game modules must unregister callbacks they installed before their
         * dynamic library is unloaded. A retained @c std::function target may
         * otherwise point into an unmapped module image during later use or
         * process-static destruction.
         *
         * @param typeName  Component type name to remove.
         * @return          @c true when an entry was removed; @c false when no
         *                  entry with that name existed.
         *
         * @note [game thread] Call during single-threaded module teardown.
         */
        bool Unregister(const std::string& typeName);

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

        /**
         * @brief Auto-register (de)serializers for all reflected component types.
         *
         * Uses Spark::TypeRegistry and Spark::ComponentFactory to generate
         * serialize/deserialize lambdas for every reflected component that does
         * not already have a hand-written serializer. Called automatically at the
         * end of RegisterBuiltins().
         *
         * Fields are serialized using their C++ member name as the property key
         * and GetFieldAsString/SetFieldFromString for value encoding.
         */
        void RegisterReflectedSerializers();

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
 * It coordinates the ComponentSerializerRegistry and the custom binary (de)serializer
 * (see "Serialization format" above) to produce compact, portable save files.
 *
 * ### Save file location
 * All files are written to the directory specified in Initialize() (default: `"Saves/"`
 * relative to the working directory). Each save slot produces a single `.spark_save`
 * file named after the slot (e.g. `"Saves/slot1.spark_save"`).
 *
 * ### Quicksave and autosave
 * - **QuickSave/QuickLoad**: single dedicated `"__quicksave"` slot; always overwrites.
 * - **AutoSave**: rotates through `maxAutoSaves` slots (default: 3). The oldest slot
 *   is overwritten when all slots are used.
 *
 * ### Error handling
 * All public save/load methods return `bool` for ordinary validation, I/O, migration,
 * and candidate-deserialization failures. Load() builds a fresh candidate World and
 * verifies every declared component, type-erased operation, representation bound, and
 * custom-state copy before touching live component storage. Therefore an ordinary
 * `false` leaves the caller's exact registry topology, entities, components, per-entity
 * event subscriptions, and custom-state output unchanged.
 *
 * Preparing a previously absent live component pool is the explicit commit boundary.
 * Allocation or callback exceptions at that boundary propagate; they are never converted
 * to `false`, because an empty pool may already have changed registry topology even though
 * entity/component payloads, per-entity subscriptions, and custom state remain unchanged.
 * After preparation, the commit preserves the live registry's signal objects while
 * retiring old entity lifecycle state and explicitly rebinding live reactive consumers.
 *
 * EnTT lifecycle observers are application callbacks and are not constrained to be
 * non-throwing by the current World API. If one throws after retirement begins, the
 * exception propagates rather than being converted to a misleading `false`; the World
 * may be partially retired and the application must treat this as a fatal lifecycle
 * programming error. Full rollback of observer-owned state remains outside this slice.
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
        using CustomStateValidator =
            std::function<bool(const std::unordered_map<std::string, std::string>& customState)>;

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
     * and writes the binary result to `<saveDirectory>/<slotName>.spark_save`.
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
         * @brief Save ECS state plus module-owned opaque key/value state in one atomic file.
         *
         * Use this overload for orchestration state that is not naturally represented by
         * ECS components (for example a template's active encounter or session cursor).
         */
        bool Save(const std::string& slotName, World& world, const SaveMetadata& metadata,
                  const std::unordered_map<std::string, std::string>& customState);

        /**
     * @brief Load a previously saved game state from the specified slot.
     *
     * Reads and parses the binary save file, reconstructs all entities and components
     * in `world`, and applies any version migrations if the save format version is
     * older than the current engine version. Saves written by a newer format version
     * are rejected (the load fails) rather than misinterpreted.
     *
     * @warning A successful load replaces the provided `world`. Ensure no raw pointers
     *          to world entities are held by callers before invoking this method.
     *          Ordinary pre-commit failures leave exact registry topology and world state
     *          unchanged. Live-pool preparation exceptions propagate and may leave an
     *          empty pool in the registry before retirement. A lifecycle-observer exception
     *          during/after retirement also propagates and may leave a partially committed
     *          world; neither condition is reported as an ordinary `false` rollback.
     *
     * @param slotName  Save slot to load (must match a slot previously written by Save()).
     * @param world     The ECS World to restore into. Existing state is cleared.
     * @return          `true` if the world was fully restored; `false` on ordinary
     *                  pre-commit errors (file not found, corrupt/truncated data,
     *                  version mismatch, candidate deserialization, etc.).
     * @throws           Storage-preparation exceptions after the live-registry boundary,
     *                   and lifecycle-observer exceptions during or after retirement.
     */
        bool Load(const std::string& slotName, World& world);

        /**
         * @brief Load ECS state and return the module-owned opaque state from the same snapshot.
         *
         * @param outCustomState Replaced only after the save parses and the world restores successfully.
         */
        bool Load(const std::string& slotName, World& world,
                  std::unordered_map<std::string, std::string>& outCustomState);

        /**
         * @brief Validate module-owned state before committing the ECS world restore.
         *
         * The validator runs after the complete file has parsed but before DeserializeWorld().
         * Returning false leaves both @p world and @p outCustomState unchanged.
         */
        bool Load(const std::string& slotName, World& world,
                  std::unordered_map<std::string, std::string>& outCustomState,
                  const CustomStateValidator& customStateValidator);

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
     * Removes `<saveDirectory>/<slotName>.spark_save` from the file system. A no-op if the
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
     * Scans the save directory for `*.spark_save` files, reads only the metadata
     * header from each (without parsing the entity data), and returns the results
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
     * @return          `true` if `<saveDirectory>/<slotName>.spark_save` exists on disk.
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
         * @brief Migrate an in-memory save snapshot to kCurrentSaveVersion.
         *
         * The supported compatibility window is exactly
         * kOldestSupportedSaveVersion..kCurrentSaveVersion. The v1-to-v2 step adds
         * the previously unpersisted screenshot field with its defined empty value.
         * Calling this function again after success is a no-op. Unsupported versions
         * return false without changing @p data.
         *
         * @param data Parsed or manually constructed save data to migrate in place.
         * @return true when data is current after the call; false when its source
         *         version is outside the supported compatibility window.
         */
        static bool MigrateToCurrentVersion(SaveData& data);

        /**
     * @brief Restore world state from a SaveData without reading from disk.
     *
     * Reconstructs all entities and components in a fresh candidate World, then retires
     * old entities, swaps validated storage payloads into the existing live registry,
     * and emits explicit reactive rebind notifications for incoming components.
     * Unknown/duplicate component types, unsupported versions, representation-limit
     * failures, incomplete type-erased operations, and candidate-deserializer failures
     * (including a callback that does not materialize its declared component) return
     * `false` without changing exact registry topology, entity/component state, or
     * per-entity EventBus subscriptions.
     * Pair with SerializeWorld() for in-memory snapshot/restore patterns.
     *
     * @param data   SaveData snapshot (e.g. from a previous SerializeWorld() call).
     * @param world  The ECS World to replace after a successful restore.
     * @return       `true` if all entities were restored successfully; `false` on a
     *               pre-commit validation/deserialization error.
     * @throws       Live-storage-preparation exceptions after validation. Preparation
     *               may leave a newly materialized empty pool even when retirement has
     *               not begun. Lifecycle-observer exceptions during/after retirement
     *               also propagate and may leave the caller's World partially committed.
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
     * @brief Serialize a SaveData to the binary save format and write it to disk.
     *
     * Writes `data` in the custom binary layout (see "Serialization format") to a
     * temporary file, then atomically renames it over `filepath`. Returns false on
     * any I/O error or if a string field would overflow its length prefix.
     *
     * @param filepath  Absolute or relative path of the output file.
     * @param data      SaveData to write.
     * @return          `true` on success; `false` on error.
     */
        bool WriteToFile(const std::string& filepath, const SaveData& data) const;

        /**
     * @brief Read and parse a binary save file from disk.
     *
     * Reads the file at `filepath`, verifies the `"SPRK"` magic and format version,
     * and parses the binary layout into `outData`. Returns false if the file does not
     * exist, has a bad magic, has a newer-than-supported version, or is truncated.
     *
     * @param filepath  Absolute or relative path of the input file.
     * @param outData   Output parameter populated on success.
     * @return          `true` on success; `false` on any error.
     */
        bool ReadFromFile(const std::string& filepath, SaveData& outData) const;

        /**
     * @brief Read only the header + metadata block of a save file.
     *
     * Parses the magic, version, and metadata text block and then stops, skipping the
     * (potentially large) entity payload. Used by GetSaveSlots()/GetSaveMetadata() so
     * enumerating slots does not cost the size of every save's full entity data.
     *
     * @param filepath     Absolute or relative path of the input file.
     * @param outMetadata  Output parameter populated on success.
     * @return             `true` if the header + metadata were read successfully.
     */
        bool ReadMetadataOnly(const std::string& filepath, SaveMetadata& outMetadata) const;

        /**
     * @brief Construct the full file path for a save slot.
     *
     * Returns `m_saveDirectory + "/" + slotName + ".spark_save"`. Does not perform any
     * file-system operations.
     *
     * @param slotName  Slot identifier.
     * @return          Full path to the corresponding `.spark_save` file.
     */
        static bool IsValidSlotName(const std::string& slotName);
        std::string GetSavePath(const std::string& slotName) const;

        /** @brief Directory where all `.spark_save` files are stored. Defaults to `"Saves"`. */
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
