# Save System

SparkEngine provides an [[Entity Component System]]-aware save/load system with JSON serialization and miniz compression.

**Source:** `SparkEngine/Source/Engine/SaveSystem/SaveSystem.h`

`ENABLE_SAVE_SYSTEM=ON`

## Features

- [[Entity Component System]]-aware serialization of all entity components
- JSON format with miniz compression
- Multiple save slots
- Quicksave / quickload
- Rotating autosaves
- Per-component serializer registry
- Metadata tracking ([[Scene Management|scene name]], player class, playtime)

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

## Per-Component Serializers

Components are serialized through a registry of serializer functions. The system automatically handles all built-in component types. Custom components can register their own serializers.

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

- [[Entity Component System]] — Components that are serialized
- [[Scene Management]] — Scene state persistence
- [[Scripting with AngelScript]] — Script-driven save/load triggers
- [[Event System]] — Save/load event callbacks
- [[Networking]] — Multiplayer state persistence
