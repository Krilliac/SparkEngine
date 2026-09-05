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

Integration with [Area Server Architecture](Area-Server-Architecture.md) enables server-side area management for multiplayer worlds.

## Source Files

| File | Description |
|------|-------------|
| `SparkEngine/Source/Engine/World/WorldOriginSystem.h` | Origin rebasing system |
| `SparkEngine/Source/Engine/World/WorldOriginSystem.cpp` | Origin rebasing implementation |
| `SparkEngine/Source/Engine/Streaming/SeamlessAreaManager.h` | Seamless area manager |
| `SparkEngine/Source/Engine/Streaming/SeamlessAreaManager.cpp` | Seamless area implementation |
| `SparkEngine/Source/Engine/Streaming/DirectStorageLoader.h` | Async storage/loading |
| `SparkEngine/Source/Engine/Streaming/SceneManifest.h` | Area manifest parsing and its budgets |
| `SparkEngine/Source/Engine/Streaming/AreaAssetLoader.h` | Consumption point for manifest asset paths |

### Manifest parsing budgets and path containment (2026-09 sweep)

- `ParseFromString` and `ParseFromFile` both enforce `MAX_SCENE_MANIFEST_BYTES` (8 MB).
  `ParseFromFile` **fails closed** when `std::filesystem::file_size` reports an error — a path that
  can be opened but not stat'd yields an empty manifest rather than an unbounded read.
- `MAX_SCENE_MANIFEST_ENTRIES` (100000) truncation is logged once instead of dropping silently.
- Path containment for streamed assets is enforced at the **consumption point**,
  `AreaAssetLoader::BeginAreaLoad`, not in the parser: a `SceneManifest` can be built in code
  (`SeamlessAreaManager` does) and handed to `SetManifest` without ever passing through
  `ParseFromString`, so the parser's `IsVirtualPathSafe` filter is defence in depth only.
- When every declared path is rejected the area still **completes** — the callback fires — rather
  than hanging on a completion count that can never be reached.
- `SceneManifest.h` includes `Engine/Modding/VirtualFileSystem.h`, so `AreaAssetLoader.h`,
  `SeamlessAreaManager.h` and the OpenWorld game module carry a link dependency on the Modding TU.

## Origin Rebasing Implementation Details

### How ForceRebase Works Internally

When `Update()` detects the reference position exceeds the threshold, it calls `ForceRebase()`:

```cpp
// Pseudocode of the rebasing process
void WorldOriginSystem::ForceRebase(entt::registry& registry, const XMFLOAT3& offset)
{
    // Step 1: Shift all entity Transform positions
    ShiftAllTransforms(registry, offset);

    // Step 2: Accumulate the offset for coordinate conversion
    m_accumulatedOffset.x += offset.x;
    m_accumulatedOffset.y += offset.y;
    m_accumulatedOffset.z += offset.z;

    // Step 3: Notify all registered callbacks (physics, audio, particles, navmesh)
    NotifyCallbacks(offset);

    // Step 4: Update statistics
    m_stats.totalRebases++;
    m_stats.currentOffset = m_accumulatedOffset;
}
```

`ShiftAllTransforms()` iterates over every entity with a Transform component and subtracts the offset from its position. Child entities inherit the shift through the hierarchy, so only root transforms need adjustment.

### Registering Rebase Callbacks

Every system that maintains world-space positions independently of the ECS must register a callback:

```cpp
Spark::World::WorldOriginSystem originSystem;

// Physics system callback
originSystem.RegisterRebaseCallback([&physicsSystem](const XMFLOAT3& offset) {
    // Shift all physics body positions by -offset
    physicsSystem.ShiftAllBodies(-offset.x, -offset.y, -offset.z);
});

// Audio system callback
originSystem.RegisterRebaseCallback([&audioSystem](const XMFLOAT3& offset) {
    // Shift all audio source positions and listener position
    audioSystem.ShiftWorldPositions(offset);
});

// Particle system callback
originSystem.RegisterRebaseCallback([&particleSystem](const XMFLOAT3& offset) {
    particleSystem.ShiftEmitters(offset);
});

// NavMesh callback
originSystem.RegisterRebaseCallback([&navMesh](const XMFLOAT3& offset) {
    navMesh.ShiftMesh(offset);
});
```

### Coordinate Conversion

The accumulated offset enables conversion between local (rebased) and absolute world coordinates at any time:

```cpp
// Local position: what the ECS and renderer see (near origin)
XMFLOAT3 localPos = entity.GetPosition();   // e.g., (50, 0, 30)

// Absolute position: the "true" world position
XMFLOAT3 absolutePos = originSystem.LocalToAbsolute(localPos);
// If accumulated offset is (150000, 0, 80000), absolutePos = (150050, 0, 80030)

// Convert back for display or save
XMFLOAT3 backToLocal = originSystem.AbsoluteToLocal(absolutePos);
// backToLocal == (50, 0, 30)
```

### Statistics and Monitoring

```cpp
const Spark::World::OriginRebasingStats& stats = originSystem.GetStats();

LOG_INFO("Origin Rebasing Stats:");
LOG_INFO("  Total rebases: {}", stats.totalRebases);
LOG_INFO("  Current offset: ({:.1f}, {:.1f}, {:.1f})",
         stats.currentOffset.x, stats.currentOffset.y, stats.currentOffset.z);
LOG_INFO("  Last rebase distance: {:.1f}", stats.lastRebaseDistance);
LOG_INFO("  Max distance observed: {:.1f}", stats.maxDistanceFromOrigin);
```

---

## Seamless Area Streaming Deep Dive

### Predictive Loading

`SeamlessAreaManager` uses the player's velocity and camera direction to predict which areas will be needed before the player arrives:

```cpp
// The manager tracks player state (thread-safe)
auto& mgr = Spark::Streaming::SeamlessAreaManager::GetInstance();

// Called from gameplay or physics thread
mgr.SetPlayerState(
    playerPosition,     // Current world position
    playerVelocity,     // Current velocity (meters/second)
    cameraForward       // Normalized camera direction
);

// Internally, PredictFuturePosition() calculates:
// predictedPos = position + velocity * lookaheadTime
// This predicted position is used for area distance calculations
```

### Streaming Configuration Reference

```cpp
Spark::Streaming::StreamingConfig config;
config.loadRadius = 500.0f;        // Start loading when player is within 500 units
config.unloadRadius = 800.0f;      // Unload when player is beyond 800 units
config.lookaheadTime = 3.0f;       // Predict 3 seconds ahead
config.updateInterval = 0.25f;     // Recalculate every 250ms
config.maxConcurrentLoads = 2;     // Max 2 areas loading simultaneously

mgr.SetConfig(config);
```

| Parameter | Default | Description | Tuning Guidance |
|-----------|---------|-------------|-----------------|
| `loadRadius` | 500.0 | Distance at which areas begin loading | Increase for fast-moving games (vehicles, flying) |
| `unloadRadius` | 800.0 | Distance at which loaded areas unload | Must be > loadRadius to prevent thrashing |
| `lookaheadTime` | 3.0 | Seconds to predict ahead using velocity | Increase for high-speed gameplay |
| `updateInterval` | 0.25 | Seconds between prediction recalculations | Lower = more responsive, higher = less CPU |
| `maxConcurrentLoads` | 2 | Maximum areas loading at once | Increase on fast storage (NVMe) |

### Area State Machine

Areas transition through the following states:

```
                 ┌──────────┐
                 │ Unloaded │ ◄──────────────────────┐
                 └────┬─────┘                         │
                      │ Player enters loadRadius      │
                      ▼                               │
                 ┌──────────┐                         │
                 │ Loading  │                         │
                 └────┬─────┘                         │
                      │ Async load complete           │
                      ▼                               │
                 ┌──────────┐                         │
                 │  Loaded  │                         │
                 └────┬─────┘                         │
                      │ Player exits unloadRadius     │
                      ▼                               │
                 ┌───────────┐                        │
                 │ Unloading │ ───────────────────────┘
                 └───────────┘
```

```cpp
// Query current area state
Spark::Streaming::AreaState state = mgr.GetAreaState(areaId);

switch (state)
{
    case Spark::Streaming::AreaState::Unloaded:
        // No resources allocated
        break;
    case Spark::Streaming::AreaState::Loading:
        // Async load in progress
        break;
    case Spark::Streaming::AreaState::Loaded:
        // Fully active, entities simulating
        break;
    case Spark::Streaming::AreaState::Unloading:
        // Resources being released
        break;
}
```

### State Change Callbacks

```cpp
mgr.RegisterStateCallback([](Spark::Streaming::AreaID areaId,
                              Spark::Streaming::AreaState newState) {
    switch (newState)
    {
        case Spark::Streaming::AreaState::Loaded:
            LOG_INFO("Area {} loaded and active", areaId);
            // Spawn NPCs, start ambient sounds, etc.
            break;
        case Spark::Streaming::AreaState::Unloading:
            LOG_INFO("Area {} unloading", areaId);
            // Save transient state, despawn actors
            break;
        default:
            break;
    }
});
```

---

## Coordinate Precision Analysis

### Floating-Point Precision at Distance

32-bit floats provide approximately 7 decimal digits of precision. At large distances, this means:

| Distance from Origin | Precision | Visible Effect |
|---------------------|-----------|----------------|
| 1,000 units | ~0.0001 units | None |
| 10,000 units | ~0.001 units | Minor jitter on small objects |
| 100,000 units | ~0.01 units | Visible jitter, physics instability |
| 1,000,000 units | ~0.1 units | Severe jitter, broken physics |

### Threshold Selection

The default threshold of 5000 units provides a safety margin:

```cpp
// Conservative: rebase more often for precision-critical games
originSystem.SetRebasingThreshold(2000.0f);

// Default: good for most scenarios
originSystem.SetRebasingThreshold(5000.0f);

// Aggressive: fewer rebases but lower precision at edges
originSystem.SetRebasingThreshold(10000.0f);
```

### Double Precision Option

For scientific simulations or extremely large worlds, the engine supports double-precision physics via the `SPARK_DOUBLE_PRECISION_PHYSICS` CMake flag. This extends usable range but doubles memory usage for position data.

---

## Multiplayer Considerations

### Origin Rebasing in Networked Games

In a client-server architecture, each client may have a different local origin offset. The key rule is: **always transmit absolute positions over the network**.

```cpp
// Client sends position to server
XMFLOAT3 absolutePos = originSystem.LocalToAbsolute(localPlayerPos);
SendPositionToServer(absolutePos);

// Client receives position from server
XMFLOAT3 receivedAbsolutePos = ReceivePositionFromServer();
XMFLOAT3 localOtherPlayer = originSystem.AbsoluteToLocal(receivedAbsolutePos);
// Render other player at localOtherPlayer
```

### Area Server Integration

When using the [Area Server Architecture](Area-Server-Architecture.md), each AreaServer has its own coordinate space. The WorldServer mediates coordinate translation during entity migration:

```cpp
// Server A's local space -> absolute -> Server B's local space
XMFLOAT3 absolutePos = serverA_origin.LocalToAbsolute(entityLocalPos);
MigratingEntity entity;
entity.position = absolutePos;  // Always store absolute in migration data

// On the receiving side:
XMFLOAT3 localInB = serverB_origin.AbsoluteToLocal(entity.position);
```

---

## Debugging Tools

### Diagnostics

Both systems expose console-friendly status methods:

```cpp
// SeamlessAreaManager diagnostics
std::string areaStatus = mgr.Console_GetStatus();
// Example: "SeamlessAreaManager: 3 areas registered, 2 loaded, current=TownSquare
//           Player: (150.0, 0.0, 80.0) vel=(5.0, 0.0, 0.0)
//           Load queue: 0 pending, 0 active loads"

// WorldOriginSystem diagnostics
const auto& stats = originSystem.GetStats();
LOG_INFO("Rebases: {} | Offset: ({:.0f}, {:.0f}, {:.0f}) | Last dist: {:.0f}",
         stats.totalRebases,
         stats.currentOffset.x, stats.currentOffset.y, stats.currentOffset.z,
         stats.lastRebaseDistance);
```

### Querying Loaded Areas

```cpp
// List all currently loaded areas
const auto& loadedIds = mgr.GetLoadedAreas();
for (AreaID id : loadedIds)
{
    LOG_INFO("  Loaded area: {}", id);
}

// Get the area the player is currently inside
AreaID current = mgr.GetCurrentArea();
```

### Reset for Testing

```cpp
// Reset origin system to initial state (clears offset and stats)
originSystem.Reset();

// Reinitialize streaming manager
mgr.Shutdown();
mgr.Initialize();
```

---

## Related Pages

- [Area Server Architecture](Area-Server-Architecture.md) — Multiplayer area servers
- [Scene Management](Scene-Management.md) — Scene hierarchy and prefabs
- [Networking](Networking.md) — Base networking system
