# Large World Support

SparkEngine provides two systems for supporting game worlds larger than traditional floating-point precision allows: **WorldOriginSystem** for floating-point origin rebasing, and **SeamlessAreaManager** for seamless area streaming without loading screens.

Both systems are inspired by HeroEngine's Seamless World 2.0 technology.

## Floating-Point Origin Rebasing

### The Problem

In a standard game engine, all positions are stored as 32-bit floats relative to the world origin (0,0,0). At large distances (>5000 units), floating-point precision degrades causing:
- Visual jitter on meshes and particles
- Physics instability (objects vibrating, falling through floors)
- Audio positioning errors

### The Solution

`WorldOriginSystem` periodically shifts all entity positions so the player is always near the local origin. The accumulated offset is tracked, enabling conversion between local and absolute coordinates at any time.

```cpp
Spark::World::WorldOriginSystem originSystem;
originSystem.SetRebasingThreshold(5000.0f);  // Rebase when player > 5000 units from origin
originSystem.SetEnabled(true);

// Register callbacks so other systems (physics, audio, particles) shift too
originSystem.RegisterRebaseCallback([](const XMFLOAT3& offset) {
    // Shift physics world, audio listeners, particle emitters, etc.
});

// In the game loop:
bool rebased = originSystem.Update(registry, playerPosition);
```

### How It Works

1. Each frame, `Update()` checks the player's distance from the local origin
2. If the distance exceeds the threshold, `ForceRebase()` is called
3. All root entity transforms are shifted (children inherit via hierarchy)
4. Registered callbacks are fired so external systems can shift their data
5. The accumulated offset is tracked for coordinate conversion

### Coordinate Conversion

```cpp
// Convert local position to absolute world coordinates
XMFLOAT3 absolute = originSystem.LocalToAbsolute(localPos);

// Convert absolute world coordinates to local
XMFLOAT3 local = originSystem.AbsoluteToLocal(absolutePos);
```

### Configuration

| Method | Description | Default |
|--------|-------------|---------|
| `SetRebasingThreshold(float)` | Distance that triggers rebasing | 5000.0 |
| `SetEnabled(bool)` | Enable/disable rebasing | false |
| `RegisterRebaseCallback(fn)` | Register shift notification | — |

## Seamless Area Streaming

### Overview

`SeamlessAreaManager` divides the game world into areas (tiles) and manages their loading/unloading based on player position. Adjacent areas share configurable border overlap regions where entities from both areas coexist.

```cpp
Spark::Streaming::SeamlessAreaManager areaManager;
areaManager.Initialize();

// Define world areas
WorldArea town;
town.name = "Town";
town.scenePath = "Scenes/Town.scene";
town.worldPosition = {0, 0, 0};
town.worldSize = {1000, 100, 1000};
town.loadDistance = 2000.0f;
town.unloadDistance = 3000.0f;
areaManager.RegisterArea(town);

WorldArea forest;
forest.name = "Forest";
forest.scenePath = "Scenes/Forest.scene";
forest.worldPosition = {1000, 0, 0};
forest.worldSize = {1000, 100, 1000};
areaManager.RegisterArea(forest);

// Define border overlap
areaManager.DefineBorder("Town", "Forest", 50.0f);

// In game loop:
areaManager.Update(playerPosition, deltaTime);
```

### Area States

| State | Description |
|-------|-------------|
| `Unloaded` | Not loaded, no resources consumed |
| `Loading` | Being loaded in the background |
| `Active` | Fully loaded and simulating |
| `Border` | In border overlap mode |
| `Unloading` | Being unloaded |
| `Failed` | Loading failed |

### Border Regions

Border regions are overlap zones between adjacent areas. When a player enters a border region, both areas are loaded and active, ensuring seamless gameplay across boundaries.

```cpp
// Check if player is in a border region
if (areaManager.IsInBorderRegion()) {
    // Both areas are active — entities from both are visible
}

// Translate position between area coordinate spaces
XMFLOAT3 forestPos = areaManager.TranslatePosition(townPos, "Town", "Forest");
```

### Area Events

```cpp
areaManager.RegisterTransitionCallback([](const AreaTransitionEvent& event) {
    // event.fromArea, event.toArea, event.crossingPosition, event.isSeamless
    LOG("Player transitioned from %s to %s", event.fromArea, event.toArea);
});
```

### WorldArea Configuration

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | `string` | — | Unique area name |
| `scenePath` | `string` | — | Path to area's scene file |
| `worldPosition` | `XMFLOAT3` | (0,0,0) | Position in absolute world space |
| `worldSize` | `XMFLOAT3` | (1000,100,1000) | Area dimensions |
| `localOrigin` | `XMFLOAT3` | (0,0,0) | Local coordinate origin offset |
| `loadDistance` | `float` | 2000.0 | Distance to start loading |
| `unloadDistance` | `float` | 3000.0 | Distance to unload |
| `priority` | `int` | 0 | Loading priority |
| `alwaysLoaded` | `bool` | false | Never unload this area |

## Combining Both Systems

For the best large-world experience, use both systems together:

1. **WorldOriginSystem** keeps the player near the local origin for precision
2. **SeamlessAreaManager** streams area scenes in/out based on proximity
3. Each area can define its own `localOrigin` for coordinate translation

Integration with [Area Server Architecture](Area-Server-Architecture) enables server-side area management for multiplayer worlds.

## Source Files

| File | Description |
|------|-------------|
| `SparkEngine/Source/Engine/World/WorldOriginSystem.h` | Origin rebasing system |
| `SparkEngine/Source/Engine/World/WorldOriginSystem.cpp` | Origin rebasing implementation |
| `SparkEngine/Source/Engine/Streaming/SeamlessAreaManager.h` | Seamless area manager |
| `SparkEngine/Source/Engine/Streaming/SeamlessAreaManager.cpp` | Seamless area implementation |
| `SparkEngine/Source/Engine/Streaming/DirectStorageLoader.h` | Async storage/loading |

## Related Pages

- [Area Server Architecture](Area-Server-Architecture) — Multiplayer area servers
- [Scene Management](Scene-Management) — Scene hierarchy and prefabs
- [Networking](Networking) — Base networking system
