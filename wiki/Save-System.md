# Save System

SparkEngine provides an [Entity Component System](Entity-Component-System)-aware save/load system with JSON serialization, miniz compression, multiple save slots, quicksave/quickload, rotating autosaves, and a per-component serializer registry.

**Source:** `SparkEngine/Source/Engine/SaveSystem/SaveSystem.h`

`ENABLE_SAVE_SYSTEM=ON`

## Architecture

The save system is composed of four core types that work together to serialize, compress, and persist the complete game state:

```
┌─────────────────────────────────────────────────────────────────────┐
│                           Game Code                                  │
│         Save() / Load() / QuickSave() / AutoSave()                   │
├─────────────────────────────────────────────────────────────────────┤
│                          SaveSystem                                   │
│                    (singleton facade)                                 │
│                                                                      │
│   ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐ │
│   │ SerializeWorld│  │ WriteToFile  │  │ Configuration            │ │
│   │ DeserializeWorld  │ ReadFromFile │  │ SetMaxAutoSaves()        │ │
│   │              │  │              │  │ SetSaveDirectory()       │ │
│   │  ┌───────┐   │  │  ┌────────┐ │  │ SetFileCache()           │ │
│   │  │SaveData│  │  │  │ miniz  │ │  └──────────────────────────┘ │
│   │  └───────┘   │  │  │compress│ │                               │
│   └──────────────┘  │  └────────┘ │                               │
│                      └──────────────┘                               │
├─────────────────────────────────────────────────────────────────────┤
│              ComponentSerializerRegistry                             │
│     (singleton: maps type names to serialize/deserialize funcs)      │
│                                                                      │
│   Built-in:  Transform, HealthComponent, RigidBodyComponent,        │
│              MeshRenderer, Camera, AudioSourceComponent,             │
│              LightComponent, AnimationController, AIComponent, ...   │
│                                                                      │
│   Custom:    Register("MyComponent", serializeFn, deserializeFn)     │
├─────────────────────────────────────────────────────────────────────┤
│                         ECS World                                    │
│          (entities + components to serialize/deserialize)             │
└─────────────────────────────────────────────────────────────────────┘
```

### Type Overview

| Type | Responsibility |
|------|---------------|
| `SaveSystem` | Singleton facade: orchestrates save/load, manages slots, quicksave, autosave |
| `ComponentSerializerRegistry` | Singleton registry mapping component type names to (de)serializer functions |
| `SaveMetadata` | Lightweight header with display info (name, scene, playtime, screenshot, etc.) |
| `SaveData` | Complete snapshot: metadata + all serialized entities + custom state |
| `SerializedEntity` | One entity's ID, name, and list of serialized components |
| `SerializedComponent` | Type-erased component: type name + string key-value properties |

### Namespace

All save system types reside in `namespace Spark`.

## SaveMetadata

Lightweight metadata header attached to every save file. Written both inside the compressed save and readable without full decompression for the save slot UI:

```cpp
struct SaveMetadata
{
    std::string saveName;                      // Human-readable name (e.g. "Before Boss Fight")
    std::string sceneName;                     // Scene/level identifier (e.g. "Level03")
    std::string playerClass;                   // Player class name (e.g. "Soldier")
    uint32_t version = 1;                      // Save format version
    uint64_t timestamp = 0;                    // Unix timestamp (auto-set by SaveSystem)
    float playTime = 0.0f;                     // Accumulated play time (seconds)
    std::string screenshotPath;                // Path to slot thumbnail PNG/JPG

    // Player summary fields for save-slot UI
    float playerHealth = 0.0f;                 // Player health at save time
    float playerArmor = 0.0f;                  // Player armor at save time
    DirectX::XMFLOAT3 playerPosition{0, 0, 0}; // Player world position
    int playerKills = 0;                       // Kill count
    int playerDeaths = 0;                      // Death count
};
```

| Field | Type | Description |
|-------|------|-------------|
| `saveName` | `string` | Display name in the save slot UI |
| `sceneName` | `string` | Scene file identifier for reloading the level |
| `playerClass` | `string` | Class name (for display only; actual class data is in components) |
| `version` | `uint32_t` | Format version for migration support (default: 1) |
| `timestamp` | `uint64_t` | Auto-set to `time(nullptr)` at save time |
| `playTime` | `float` | Total play time in seconds; populate before saving |
| `screenshotPath` | `string` | Path to thumbnail; game code must capture the screenshot |
| `playerHealth` | `float` | Health at save time for UI display |
| `playerArmor` | `float` | Armor at save time for UI display |
| `playerPosition` | `XMFLOAT3` | World position; used as spawn hint during load |
| `playerKills` | `int` | Accumulated kills for UI display |
| `playerDeaths` | `int` | Accumulated deaths for UI display |

### Versioning

The `version` field enables forward-compatible save files. When the save format changes:

1. Increment the version number in new saves
2. The loader checks `metadata.version` and runs migration routines before deserializing
3. Game code should not modify `version` unless implementing a migration pass

## SerializedComponent

Type-erased intermediate container that bridges strongly-typed C++ structs and JSON:

```cpp
struct SerializedComponent
{
    std::string typeName;                                     // e.g. "Transform"
    std::unordered_map<std::string, std::string> properties;  // All values as strings
};
```

All field values are stored as strings for direct JSON serialization. Each component's registered serializer handles encoding/decoding (e.g., converting `XMFLOAT3{1, 2, 3}` to the string `"1.0 2.0 3.0"`).

### Example Serialized Transform

```json
{
    "typeName": "Transform",
    "properties": {
        "posX": "1.0", "posY": "0.5", "posZ": "-3.0",
        "rotX": "0.0", "rotY": "45.0", "rotZ": "0.0",
        "scaleX": "1.0", "scaleY": "1.0", "scaleZ": "1.0"
    }
}
```

## SerializedEntity

Snapshot of one ECS entity at save time:

```cpp
struct SerializedEntity
{
    uint32_t entityID;                             // EnTT entity ID (informational only)
    std::string name;                              // From NameComponent (if present)
    std::vector<SerializedComponent> components;   // All serializable components
};
```

> **Important:** The `entityID` stored here is not guaranteed to match the entity's ID after loading, because EnTT may recycle IDs. During deserialization, new entities are created via `World::CreateEntity()`. For stable cross-entity references, use named identifiers in custom components.

## SaveData

Complete snapshot of a game state:

```cpp
struct SaveData
{
    SaveMetadata metadata;                                     // Header
    std::vector<SerializedEntity> entities;                    // All entities
    std::unordered_map<std::string, std::string> customState;  // Free-form game state
};
```

### Custom State

The `customState` map stores game-specific data that does not fit into ECS components:

```cpp
data.customState["doorOpened_MainHall"] = "true";
data.customState["questStep_FindKey"]   = "3";
data.customState["globalTimer"]         = std::to_string(elapsedSeconds);
data.customState["weatherState"]        = "rainy";
data.customState["dayNightCycle"]       = std::to_string(timeOfDay);
```

## ComponentSerializerRegistry

Singleton registry mapping component type names to serialize/deserialize function pairs:

```cpp
class ComponentSerializerRegistry
{
public:
    using SerializeFunc = std::function<SerializedComponent(const void* component)>;
    using DeserializeFunc = std::function<void(World& world, EntityID entity,
                                               const SerializedComponent& data)>;

    static ComponentSerializerRegistry& GetInstance();

    void Register(const std::string& typeName,
                  SerializeFunc serialize,
                  DeserializeFunc deserialize);

    bool HasSerializer(const std::string& typeName) const;

    SerializedComponent Serialize(const std::string& typeName,
                                  const void* component) const;

    void Deserialize(const std::string& typeName, World& world,
                     EntityID entity, const SerializedComponent& data) const;

    void RegisterBuiltins();
};
```

| Method | Description |
|--------|-------------|
| `GetInstance()` | Meyer's singleton, thread-safe on first call (C++11 guarantee) |
| `Register(name, ser, deser)` | Register a serialize/deserialize pair for a component type |
| `HasSerializer(name)` | Check if a serializer is registered |
| `Serialize(name, comp)` | Serialize a component via its registered function |
| `Deserialize(name, world, entity, data)` | Deserialize and attach component to entity |
| `RegisterBuiltins()` | Register all built-in engine components (called by `SaveSystem::Initialize()`) |

### Built-in Component Serializers

`RegisterBuiltins()` registers serializers for all Spark Engine built-in components:

| Component | Serialized Fields |
|-----------|------------------|
| `Transform` | posX, posY, posZ, rotX, rotY, rotZ, scaleX, scaleY, scaleZ |
| `NameComponent` | name |
| `HealthComponent` | health, maxHealth |
| `RigidBodyComponent` | mass, friction, restitution, isKinematic |
| `MeshRenderer` | meshPath, materialPath |
| `Camera` | fov, nearPlane, farPlane, isActive |
| `AudioSourceComponent` | clipPath, volume, pitch, loop, is3D |
| `LightComponent` | type, color, intensity, range, spotAngle |
| `AnimationController` | currentAnimation, speed, playing |
| `AIComponent` | behaviorTree, aggroRange, patrolPath |

### Registering Custom Components

```cpp
auto& registry = ComponentSerializerRegistry::GetInstance();

registry.Register("CustomInventory",
    // Serialize
    [](const void* comp) -> SerializedComponent {
        const auto* inv = static_cast<const CustomInventory*>(comp);
        SerializedComponent sc;
        sc.typeName = "CustomInventory";
        sc.properties["capacity"] = std::to_string(inv->capacity);
        sc.properties["gold"]     = std::to_string(inv->gold);
        return sc;
    },
    // Deserialize
    [](World& world, EntityID entity, const SerializedComponent& data) {
        CustomInventory inv;
        inv.capacity = std::stoi(data.properties.at("capacity"));
        inv.gold     = std::stoi(data.properties.at("gold"));
        world.AddComponent<CustomInventory>(entity, inv);
    }
);
```

### Registration Guidelines

1. Call `Register()` once per type during single-threaded initialization
2. The `typeName` string must match exactly between serializer and deserializer
3. Use `std::to_string()` for numeric values and parse with `std::stoi()` / `std::stof()`
4. For vectors, encode as space-separated floats: `"1.0 2.0 3.0"`
5. For booleans, use `"true"` / `"false"` strings

## SaveSystem

Singleton facade that orchestrates all save and load operations:

```cpp
class SaveSystem
{
public:
    static SaveSystem& GetInstance();

    bool Initialize(const std::string& saveDirectory = "Saves");

    // Core save/load
    bool Save(const std::string& slotName, World& world, const SaveMetadata& metadata);
    bool Load(const std::string& slotName, World& world);

    // Quick save/load
    bool QuickSave(World& world, const SaveMetadata& metadata);
    bool QuickLoad(World& world);

    // Autosave
    bool AutoSave(World& world, const SaveMetadata& metadata);

    // Slot management
    bool DeleteSave(const std::string& slotName);
    std::vector<SaveMetadata> GetSaveSlots() const;
    bool GetSaveMetadata(const std::string& slotName, SaveMetadata& outMetadata) const;
    bool SaveExists(const std::string& slotName) const;

    // In-memory serialization (no disk I/O)
    SaveData SerializeWorld(World& world, const SaveMetadata& metadata) const;
    bool DeserializeWorld(const SaveData& data, World& world) const;

    // Configuration
    void SetMaxAutoSaves(int count);       // Default: 3, must be >= 1
    void SetSaveDirectory(const std::string& dir);
    void SetFileCache(LocalFileCache* cache);

    // Console integration
    std::string Console_ListSaves() const;
    std::string Console_GetSaveInfo(const std::string& slotName) const;
};
```

### Method Reference

| Method | Description |
|--------|-------------|
| `Initialize(dir)` | Create save directory, register built-in serializers |
| `Save(slot, world, meta)` | Serialize world to JSON, compress with miniz, write to `<dir>/<slot>.sav` |
| `Load(slot, world)` | Read file, decompress, parse JSON, clear world, restore all entities |
| `QuickSave(world, meta)` | Save to `__quicksave` slot (overwrites previous) |
| `QuickLoad(world)` | Load from `__quicksave` slot |
| `AutoSave(world, meta)` | Save to rotating `__autosave_N` slots |
| `DeleteSave(slot)` | Remove `.sav` file from disk |
| `GetSaveSlots()` | Scan directory, return metadata sorted by timestamp (newest first) |
| `GetSaveMetadata(slot, out)` | Read metadata header without decompressing full save |
| `SaveExists(slot)` | File existence check only |
| `SerializeWorld(world, meta)` | Create SaveData in-memory without disk I/O |
| `DeserializeWorld(data, world)` | Restore world from SaveData without disk I/O |

## Save File Format

Save files use JSON compressed with **miniz** (zlib deflate). File extension: `.sav`.

### On-Disk Layout

```
┌──────────────────────────────────┐
│  miniz compressed blob           │
│  ┌────────────────────────────┐  │
│  │  JSON document             │  │
│  │  {                         │  │
│  │    "metadata": { ... },    │  │
│  │    "entities": [ ... ],    │  │
│  │    "customState": { ... }  │  │
│  │  }                         │  │
│  └────────────────────────────┘  │
└──────────────────────────────────┘
```

### JSON Schema

```json
{
    "metadata": {
        "saveName": "Before Boss Fight",
        "sceneName": "Level03",
        "playerClass": "Soldier",
        "version": 1,
        "timestamp": 1705312200,
        "playTime": 3600.0,
        "screenshotPath": "Saves/screenshots/slot1.png",
        "playerHealth": 80.0,
        "playerArmor": 50.0,
        "playerPosition": [12.5, 1.0, -8.3],
        "playerKills": 47,
        "playerDeaths": 3
    },
    "entities": [
        {
            "entityID": 1,
            "name": "Player",
            "components": [
                {
                    "typeName": "Transform",
                    "properties": {
                        "posX": "12.5", "posY": "1.0", "posZ": "-8.3",
                        "rotX": "0.0", "rotY": "135.0", "rotZ": "0.0",
                        "scaleX": "1.0", "scaleY": "1.0", "scaleZ": "1.0"
                    }
                },
                {
                    "typeName": "HealthComponent",
                    "properties": {
                        "health": "80.0",
                        "maxHealth": "100.0"
                    }
                }
            ]
        },
        {
            "entityID": 42,
            "name": "EnemyGuard_01",
            "components": [
                {
                    "typeName": "Transform",
                    "properties": {
                        "posX": "20.0", "posY": "0.0", "posZ": "-5.0",
                        "rotX": "0.0", "rotY": "90.0", "rotZ": "0.0",
                        "scaleX": "1.0", "scaleY": "1.0", "scaleZ": "1.0"
                    }
                },
                {
                    "typeName": "AIComponent",
                    "properties": {
                        "behaviorTree": "guard_patrol",
                        "aggroRange": "15.0",
                        "patrolPath": "patrol_courtyard"
                    }
                }
            ]
        }
    ],
    "customState": {
        "doorOpened_MainHall": "true",
        "questStep_FindKey": "3",
        "globalTimer": "1234.5"
    }
}
```

## Save Slot Naming

Slot names are arbitrary strings used as file-system-safe base names. Two special prefixes are reserved:

| Slot Name | Purpose | Created By |
|-----------|---------|-----------|
| `"slot1"`, `"slot2"`, etc. | User-created saves | `Save()` |
| `"__quicksave"` | Single quicksave slot | `QuickSave()` |
| `"__autosave_0"` ... `"__autosave_N"` | Rotating autosaves | `AutoSave()` |

File path construction: `<saveDirectory>/<slotName>.sav` (e.g., `Saves/slot1.sav`).

## Typical Usage

### Engine Startup

```cpp
SaveSystem& ss = SaveSystem::GetInstance();
ss.Initialize("Saves");          // Creates Saves/ directory if absent
ss.SetMaxAutoSaves(5);           // Keep 5 rotating autosaves
```

### Saving

```cpp
SaveMetadata meta;
meta.saveName       = "Before Boss Fight";
meta.sceneName      = world.GetCurrentSceneName();
meta.playerClass    = player.GetClassName();
meta.playTime       = g_totalPlayTime;
meta.playerHealth   = player.GetHealth();
meta.playerArmor    = player.GetArmor();
meta.playerPosition = player.GetPosition();
meta.playerKills    = stats.kills;
meta.playerDeaths   = stats.deaths;

ss.Save("slot1", world, meta);
```

### Loading

```cpp
if (ss.SaveExists("slot1"))
{
    // Warning: Load() clears all existing entities in world
    ss.Load("slot1", world);

    // After load, read metadata for scene reload
    SaveMetadata meta;
    ss.GetSaveMetadata("slot1", meta);
    sceneManager.LoadScene(meta.sceneName);
}
```

### Quicksave / Quickload

```cpp
// F5 = quicksave
ss.QuickSave(world, currentMeta);

// F9 = quickload
if (ss.SaveExists("__quicksave"))
    ss.QuickLoad(world);
```

### Autosave

```cpp
// Call periodically or on level transitions
ss.AutoSave(world, currentMeta);
// Rotates through: __autosave_0, __autosave_1, __autosave_2, ...
// Wraps around when all slots used, overwriting the oldest
```

### Save Slot UI

```cpp
// Enumerate for the save/load screen
auto slots = ss.GetSaveSlots();  // Sorted by timestamp (newest first)
for (const auto& meta : slots)
{
    DrawSlotButton(meta.saveName, meta.playTime, meta.screenshotPath);
    DrawSlotDetails(meta.sceneName, meta.playerClass,
                    meta.playerHealth, meta.playerKills);
}

// Delete a save
ss.DeleteSave("slot1");
```

### In-Memory Snapshots

For undo/redo systems, boss-fight checkpoints, or server-side state:

```cpp
// Take an in-memory snapshot without touching the file system
SaveData snapshot = ss.SerializeWorld(world, meta);

// ... modify world state (player fights boss) ...

// Restore from snapshot (e.g., player died, retry boss fight)
ss.DeserializeWorld(snapshot, world);
```

## Internal Implementation

### Save Flow

```
Save(slotName, world, metadata)
  │
  ├── metadata.timestamp = time(nullptr)
  │
  ├── SerializeWorld(world, metadata)
  │     ├── Create SaveData with metadata
  │     ├── For each entity in world:
  │     │     ├── Create SerializedEntity
  │     │     ├── For each component on entity:
  │     │     │     ├── Lookup serializer in ComponentSerializerRegistry
  │     │     │     ├── If found: call serialize(component) -> SerializedComponent
  │     │     │     └── If not found: skip silently
  │     │     └── Add to entities list
  │     └── Return SaveData
  │
  ├── WriteToFile(GetSavePath(slotName), saveData)
  │     ├── Serialize SaveData to JSON (RapidJSON)
  │     ├── Compress JSON with miniz deflate
  │     └── Write binary blob to disk
  │
  └── Return success/failure
```

### Load Flow

```
Load(slotName, world)
  │
  ├── ReadFromFile(GetSavePath(slotName), outData)
  │     ├── Read binary file from disk
  │     ├── Decompress with miniz inflate
  │     └── Parse JSON into SaveData
  │
  ├── Check version, run migrations if needed
  │
  ├── DeserializeWorld(data, world)
  │     ├── Clear all entities in world
  │     ├── For each SerializedEntity in data:
  │     │     ├── CreateEntity() in world
  │     │     ├── Set NameComponent if name is non-empty
  │     │     ├── For each SerializedComponent:
  │     │     │     ├── Lookup deserializer in registry
  │     │     │     ├── If found: call deserialize(world, entity, data)
  │     │     │     └── If not found: log warning, skip
  │     │     └── Continue
  │     └── Return success/failure
  │
  └── Return success/failure
```

### Autosave Rotation

```
AutoSave(world, metadata)
  │
  ├── slotName = "__autosave_" + std::to_string(m_currentAutoSaveIndex)
  ├── Save(slotName, world, metadata)
  ├── m_currentAutoSaveIndex = (m_currentAutoSaveIndex + 1) % m_maxAutoSaves
  └── Return success/failure
```

With `m_maxAutoSaves = 3`, the rotation is:

| Call # | Slot Written | Previous Slots |
|--------|-------------|----------------|
| 1 | `__autosave_0` | -- |
| 2 | `__autosave_1` | `__autosave_0` |
| 3 | `__autosave_2` | `__autosave_0`, `__autosave_1` |
| 4 | `__autosave_0` (overwritten) | `__autosave_1`, `__autosave_2` |

## Error Handling

| Scenario | Behavior |
|----------|----------|
| Save directory does not exist | `Initialize()` creates it; returns false if creation fails |
| Disk full during write | `WriteToFile()` returns false; error logged |
| File not found during load | `ReadFromFile()` returns false; error logged |
| JSON parse error | `ReadFromFile()` returns false; error logged |
| Version mismatch | Migration routines run before deserialization |
| Unknown component type during load | Warning logged, component skipped (entity still created) |
| Corrupted compressed data | miniz inflate fails; `ReadFromFile()` returns false |
| Slot name contains path separators | Undefined behavior; avoid `/` and `\` in slot names |
| `SetMaxAutoSaves(0)` | Assertion failure: `count must be >= 1` |
| `Load()` fails midway | World is left in indeterminate state; caller should reload the level |

### Error Recovery

```cpp
if (!ss.Load("slot1", world))
{
    // World may be partially populated -- reload the level
    sceneManager.ReloadCurrentScene(world);
    LOG_ERROR("Failed to load save slot 'slot1'");
}
```

## Performance Considerations

| Operation | Typical Duration | Notes |
|-----------|-----------------|-------|
| Serialize world (100 entities) | < 1 ms | CPU-bound, main thread |
| Serialize world (10,000 entities) | 10-50 ms | Consider async for large worlds |
| miniz compress | 1-5 ms | Depends on data size |
| Write to disk | 1-10 ms | Depends on file system |
| Read from disk | 1-5 ms | Typically faster than write |
| miniz decompress | 1-3 ms | Faster than compress |
| Parse JSON | 2-10 ms | RapidJSON, depends on entity count |
| Deserialize world | 5-50 ms | Includes entity creation + component attachment |

### Optimization Tips

1. **Limit serialized entities.** Only entities with registered serializers are saved. Avoid registering serializers for ephemeral entities (particles, debris).
2. **Use `SerializeWorld()` on main thread, `WriteToFile()` on background thread.** This avoids blocking the game loop during disk I/O.
3. **Keep `customState` small.** Large string values inflate the JSON and compression time.
4. **Use `GetSaveMetadata()` instead of full `Load()`** for the save slot UI.
5. **Set `m_maxAutoSaves` to 3-5.** More slots mean more disk usage and slightly longer `GetSaveSlots()` scans.

### Background Save Pattern

```cpp
// Serialize on main thread (fast, no disk I/O)
SaveData snapshot = ss.SerializeWorld(world, meta);

// Write to disk on background thread
std::thread([snapshot = std::move(snapshot), path = ss.GetSavePath("slot1")]() {
    ss.WriteToFile(path, snapshot);
}).detach();
```

## Thread Safety

The `SaveSystem` is **not thread-safe**. All methods must be called from the main game thread.

| Component | Thread Safety | Notes |
|-----------|--------------|-------|
| `SaveSystem` | Main thread only | Not mutex-protected |
| `ComponentSerializerRegistry` | Register: single-thread init only | Serialize/Deserialize: read-only after registration, safe from multiple threads |
| `SerializeWorld()` | Main thread | Reads ECS world which is not thread-safe |
| `WriteToFile()` | Any thread | Pure file I/O, no world access; safe for background |
| `ReadFromFile()` | Any thread | Pure file I/O; safe for background |
| `DeserializeWorld()` | Main thread | Writes to ECS world |

### Safe Async Save

```
Main Thread:                     Background Thread:
  SerializeWorld() -> SaveData
  hand off SaveData ──────────>    WriteToFile(path, data)
  continue gameplay                (no world access)
```

## Console Commands

```
save list                    # List all save slots with metadata
save info <slotName>         # Show detailed info about a specific save
save save <slotName>         # Trigger a save to the named slot
save load <slotName>         # Trigger a load from the named slot
save quicksave               # Trigger quicksave
save quickload               # Trigger quickload
save autosave                # Trigger autosave
save delete <slotName>       # Delete a save slot
```

Example `save list` output:

```
=== Save Slots ===
  slot1          "Before Boss Fight"   Level03   Soldier   1h 00m   2025-01-15 14:30
  __quicksave    "Quick Save"          Level03   Soldier   0h 58m   2025-01-15 14:28
  __autosave_2   "Auto Save"           Level02   Soldier   0h 45m   2025-01-15 14:15
  __autosave_1   "Auto Save"           Level02   Soldier   0h 30m   2025-01-15 14:00
  __autosave_0   "Auto Save"           Level01   Soldier   0h 15m   2025-01-15 13:45
```

Example `save info slot1` output:

```
=== Save Info: slot1 ===
  Name:           Before Boss Fight
  Scene:          Level03
  Player Class:   Soldier
  Version:        1
  Timestamp:      2025-01-15T14:30:00Z
  Play Time:      1h 00m 00s (3600.0s)
  Screenshot:     Saves/screenshots/slot1.png
  Player Health:  80.0
  Player Armor:   50.0
  Player Position: (12.5, 1.0, -8.3)
  Kills:          47
  Deaths:         3
```

## Troubleshooting

| Symptom | Possible Cause | Solution |
|---------|---------------|----------|
| `Initialize()` returns false | Cannot create save directory | Check file permissions; verify path |
| `Save()` returns false | Disk full or permission denied | Check available disk space; verify directory permissions |
| `Load()` returns false | File not found or corrupt | Use `SaveExists()` first; check for corrupt `.sav` files |
| Component missing after load | Serializer not registered | Call `Register()` during initialization for custom components |
| Entity IDs differ after load | Expected behavior | EnTT recycles IDs; use named identifiers for cross-references |
| Save file very large | Too many entities serialized | Filter out ephemeral entities; reduce `customState` size |
| Quickload does nothing | No quicksave exists | Check `SaveExists("__quicksave")` |
| Autosaves not rotating | `m_maxAutoSaves` too high | Reduce with `SetMaxAutoSaves()` |
| World partially loaded after error | `Load()` failed midway | Reload the level via SceneManager after failed load |
| Screenshot not appearing in UI | Path not set before save | Capture screenshot and set `meta.screenshotPath` before calling `Save()` |
| Custom component not saving | `typeName` mismatch | Ensure the `typeName` in `Register()` matches the serializer output |
| `std::stoi` throws during load | Property missing or malformed | Use `properties.count()` or `.at()` with try/catch in deserializer |

---

## See Also

- [Entity Component System](Entity-Component-System) -- Components that are serialized
- [Scene Management](Scene-Management) -- Scene state persistence
- [Scripting with AngelScript](Scripting-with-AngelScript) -- Script-driven save/load triggers
- [Event System](Event-System) -- Save/load event callbacks
- [Networking](Networking) -- Multiplayer state persistence
- [Gameplay Systems](Gameplay-Systems) -- Checkpoint and save triggers
