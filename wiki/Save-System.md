# Save System

SparkEngine provides an [Entity Component System](Entity-Component-System)-aware save/load system with JSON serialization and miniz compression.

**Source:** `SparkEngine/Source/Engine/SaveSystem/SaveSystem.h`

`ENABLE_SAVE_SYSTEM=ON`

## Features

- [Entity Component System](Entity-Component-System)-aware serialization of all entity components
- JSON format with miniz compression
- Multiple save slots
- Quicksave / quickload
- Rotating autosaves
- Per-component serializer registry
- Metadata tracking ([scene name](Scene-Management), player class, playtime)

## Saving

```cpp
SaveSystem saveSystem;

// Save to a named slot
saveSystem.Save("slot1", world, sceneManager);

// Quicksave
saveSystem.QuickSave(world, sceneManager);
```

## Loading

```cpp
// Load from a named slot
saveSystem.Load("slot1", world, sceneManager);

// Quickload
saveSystem.QuickLoad(world, sceneManager);
```

## Save File Format

Save files use JSON compressed with miniz:

```json
{
    "metadata": {
        "saveName": "slot1",
        "sceneName": "Level01",
        "playerClass": "Soldier",
        "playtime": 3600.0,
        "timestamp": "2025-01-15T14:30:00Z"
    },
    "entities": [
        {
            "id": 1,
            "components": {
                "Transform": { "position": [0, 1, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1] },
                "HealthComponent": { "health": 80.0, "maxHealth": 100.0 }
            }
        }
    ]
}
```

## Autosave

Rotating autosaves with configurable:
- Autosave interval (minutes)
- Maximum number of autosave slots
- Oldest autosave is overwritten when limit is reached

```cpp
// Configure autosave behavior
saveSystem.SetMaxAutoSaves(5);          // Keep 5 rotating autosaves
saveSystem.SetSaveDirectory("Saves/");  // Custom save directory

// Trigger an autosave (call periodically or on level transitions)
saveSystem.AutoSave(world, sceneManager);
```

## Per-Component Serializers

Components are serialized through a registry of serializer functions. The system automatically handles all built-in component types. Custom components can register their own serializers.

```cpp
auto& registry = ComponentSerializerRegistry::GetInstance();

// Register built-in serializers (called once at startup)
registry.RegisterBuiltins();

// Register a custom component serializer
registry.Register("CustomInventory",
    // Serialize
    [](const World& world, EntityID entity) -> std::map<std::string, std::string> {
        const auto& inv = world.GetComponent<CustomInventory>(entity);
        return {
            {"capacity", std::to_string(inv.capacity)},
            {"gold",     std::to_string(inv.gold)}
        };
    },
    // Deserialize
    [](World& world, EntityID entity, const std::map<std::string, std::string>& props) {
        auto& inv = world.AddComponent<CustomInventory>(entity);
        inv.capacity = std::stoi(props.at("capacity"));
        inv.gold     = std::stoi(props.at("gold"));
    }
);
```

## Save Metadata

Each save file stores metadata for the save slot UI:

```cpp
// Access metadata for a specific save
SaveMetadata meta = saveSystem.GetSaveMetadata("slot1");
std::string scene    = meta.sceneName;
std::string cls      = meta.playerClass;
float playtime       = meta.playTime;      // seconds
std::string time     = meta.timestamp;     // ISO 8601
std::string thumb    = meta.screenshotPath;
float hp             = meta.playerHealth;
XMFLOAT3 pos         = meta.playerPosition;
```

## Save Slots

```cpp
// List available saves
auto saves = saveSystem.ListSaves();
for (const auto& save : saves) {
    // save.name, save.sceneName, save.timestamp, save.playtime
}

// Delete a save
saveSystem.DeleteSave("slot1");
```

---

## See Also

- [Entity Component System](Entity-Component-System) — Components that are serialized
- [Scene Management](Scene-Management) — Scene state persistence
- [Scripting with AngelScript](Scripting-with-AngelScript) — Script-driven save/load triggers
- [Event System](Event-System) — Save/load event callbacks
- [Networking](Networking) — Multiplayer state persistence
