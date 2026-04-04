# Area Server Architecture

Inspired by HeroEngine's distributed server model, SparkEngine's area server architecture enables scalable multiplayer worlds by running each world area in its own server process, coordinated by a central WorldServer.

> **Status: Experimental** — Requires `ENABLE_NETWORKING=ON`. See [Networking](Networking) for setup.

## Overview

```
[Client] ──connect──> [WorldServer] ──route──> [AreaServer A]
                                                [AreaServer B]
                                                [AreaServer C]
```

- **WorldServer** — Central coordinator that manages area server registration, routes player connections, handles cross-area entity migration, and performs load balancing.
- **AreaServer** — Self-contained server process that runs a full game loop (physics, AI, scripting, ECS) for a single world area.

## Key Concepts

### AreaServer

Each AreaServer manages one area of the game world. It runs independently with its own:
- ECS simulation tick loop
- Client connections for players in the area
- Entity tracking and replication
- Cross-area messaging to adjacent area servers

```cpp
Spark::Net::AreaServerConfig config;
config.areaName = "ForestZone";
config.scenePath = "Scenes/Forest.scene";
config.tickRate = 60.0f;
config.maxClients = 64;

Spark::Net::AreaServer server;
server.Start(config);
```

### WorldServer

The WorldServer coordinates multiple AreaServers:
- Registers/unregisters area servers
- Routes player connections to the appropriate area
- Manages player sessions across area transfers
- Performs periodic load balancing
- Handles entity migration routing

```cpp
Spark::Net::WorldServerConfig config;
config.worldName = "MyWorld";
config.port = 27020;
config.enableLoadBalancing = true;

Spark::Net::WorldServer world;
world.Start(config);

// Register areas
world.RegisterAreaServer(forestConfig);
world.RegisterAreaServer(townConfig);
```

### Entity Migration

When an entity crosses an area boundary:
1. The source AreaServer serializes the entity's state (position, velocity, components)
2. The WorldServer routes the serialized data to the target AreaServer
3. The target AreaServer deserializes and spawns the entity locally
4. The source AreaServer removes the entity

```cpp
// Migrate entity to adjacent area
server.MigrateEntityOut(entityNetworkID, targetAreaId);

// Accept incoming entity
server.AcceptMigratingEntity(migratingEntity);
```

### Player Session Management

The WorldServer tracks player sessions across area transitions:

```cpp
// Player connects — WorldServer assigns to an area
AreaID area = world.HandlePlayerConnect(clientId, "PlayerName", spawnPosition);

// Player moves to new area — seamless transfer
world.TransferPlayer(clientId, targetArea);
```

### Load Balancing

The WorldServer periodically checks area server loads and identifies imbalances:
- Areas with CPU load > 80% are flagged as overloaded
- Areas with CPU load < 30% are flagged as underloaded
- The system logs planned migrations for manual or automated rebalancing

## Configuration

### AreaServerConfig

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `areaId` | `AreaID` | 0 | Unique area identifier |
| `areaName` | `string` | — | Human-readable area name |
| `scenePath` | `string` | — | Path to area's scene file |
| `port` | `uint16_t` | 0 | Client-facing port (0 = auto) |
| `interServerPort` | `uint16_t` | 0 | Inter-server communication port |
| `tickRate` | `float` | 60.0 | Simulation ticks per second |
| `maxClients` | `int` | 64 | Maximum clients in this area |
| `enableAI` | `bool` | true | Run AI simulation |
| `enablePhysics` | `bool` | true | Run physics simulation |
| `enableScripting` | `bool` | true | Run AngelScript simulation |

### WorldServerConfig

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `worldName` | `string` | "SparkWorld" | World name |
| `port` | `uint16_t` | 27020 | Client connection port |
| `interServerPort` | `uint16_t` | 27021 | AreaServer communication port |
| `maxTotalClients` | `int` | 1000 | Max clients across all areas |
| `tickRate` | `float` | 10.0 | WorldServer tick rate |
| `enableLoadBalancing` | `bool` | true | Enable dynamic load balancing |
| `loadBalanceInterval` | `float` | 30.0 | Seconds between balance checks |

## Source Files

| File | Description |
|------|-------------|
| `SparkEngine/Source/Engine/Networking/AreaServer.h` | AreaServer class and configuration |
| `SparkEngine/Source/Engine/Networking/AreaServer.cpp` | AreaServer implementation |
| `SparkEngine/Source/Engine/Networking/WorldServer.h` | WorldServer class and configuration |
| `SparkEngine/Source/Engine/Networking/WorldServer.cpp` | WorldServer implementation |

## Detailed Architecture

The following diagram illustrates the full communication topology between clients, the WorldServer, and multiple AreaServers, including inter-server messaging and entity migration paths:

```
                            ┌──────────────────────────────────────────────┐
                            │              WORLD SERVER                     │
                            │                                              │
                            │  ┌────────────┐  ┌──────────────────────┐   │
                            │  │  Player     │  │  Load Balancer       │   │
                            │  │  Session    │  │  (30s interval)      │   │
                            │  │  Manager    │  │  CPU > 80% = overload│   │
                            │  └────────────┘  │  CPU < 30% = underload│  │
                            │                   └──────────────────────┘   │
                            │  ┌────────────┐  ┌──────────────────────┐   │
                            │  │  Area       │  │  Entity Migration    │   │
                            │  │  Registry   │  │  Router              │   │
                            │  └────────────┘  └──────────────────────┘   │
                            │        port:27020 (clients)                   │
                            │        port:27021 (inter-server)             │
                            └─────┬──────────┬──────────────┬──────────────┘
                                  │          │              │
                   ┌──────────────┘          │              └──────────────┐
                   ▼                         ▼                             ▼
        ┌─────────────────┐      ┌─────────────────┐           ┌─────────────────┐
        │  AREA SERVER A  │◄────►│  AREA SERVER B  │◄─────────►│  AREA SERVER C  │
        │  "ForestZone"   │ cross│  "TownSquare"   │  cross    │  "Dungeon"      │
        │                 │ area │                 │  area     │                 │
        │  ECS + Physics  │ msgs │  ECS + Physics  │  msgs    │  ECS + Physics  │
        │  AI + Scripting │      │  AI + Scripting │          │  AI + Scripting │
        │  max: 64 clients│      │  max: 64 clients│          │  max: 32 clients│
        └────────┬────────┘      └────────┬────────┘          └────────┬────────┘
                 │                         │                            │
         ┌───────┴───────┐         ┌───────┴───────┐           ┌───────┴───────┐
         │ Clients 1..N  │         │ Clients 1..N  │           │ Clients 1..N  │
         └───────────────┘         └───────────────┘           └───────────────┘
```

## Zone Transfer Protocol

When a player moves from one area to another, the transfer follows a precise handoff sequence to ensure no data is lost and the transition feels seamless.

### Player Handoff Sequence

```
 Client          WorldServer         AreaServer A        AreaServer B
   │                  │                    │                    │
   │  (approaches     │                    │                    │
   │   area boundary) │                    │                    │
   │                  │◄── boundary ───────│                    │
   │                  │    crossed event   │                    │
   │                  │                    │                    │
   │                  │── TransferPlayer ──►                    │
   │                  │   (clientId,       │                    │
   │                  │    targetArea)      │                    │
   │                  │                    │                    │
   │                  │◄── MigratingEntity │                    │
   │                  │    (serialized     │                    │
   │                  │     state + pos    │                    │
   │                  │     + velocity)    │                    │
   │                  │                    │                    │
   │                  │── AcceptMigrating ──────────────────────►
   │                  │    Entity          │                    │
   │                  │                    │                    │
   │                  │◄───────────────────────── ACK ─────────│
   │                  │                    │                    │
   │◄── redirect ─────│                    │                    │
   │    to Area B     │── RemoveClient ───►│                    │
   │                  │                    │                    │
   │── connect ────────────────────────────────────────────────►
   │                  │                    │                    │
```

### Transfer State Machine

The `PlayerSession` struct tracks the transfer state:

```cpp
// WorldServer internally tracks transfer state per player
const PlayerSession* session = world.GetPlayerSession(clientId);

// During transfer:
// session->currentArea   = source area (still receiving updates)
// session->pendingArea   = target area (being prepared)
// session->isTransferring = true

// After completion:
// session->currentArea   = target area
// session->pendingArea   = INVALID_AREA
// session->isTransferring = false
```

### Entity Serialization During Migration

The `MigratingEntity` struct captures the full entity state for transfer:

```cpp
// MigratingEntity fields:
struct MigratingEntity
{
    uint32_t networkID;               // Preserved across the migration
    ClientID ownerID;                 // Client that owns/controls this entity
    std::string entityType;           // Type name for re-spawning
    std::vector<uint8_t> serializedState; // Full ECS component snapshot
    XMFLOAT3 position;               // World-space position at migration time
    XMFLOAT3 velocity;               // Velocity for seamless motion continuity
    float timestamp;                  // Server time when migration initiated
};

// Source area initiates migration:
areaA.MigrateEntityOut(entityNetworkID, targetAreaId);

// WorldServer routes the MigratingEntity to the target:
const auto& pending = areaA.GetPendingMigrations();
for (const auto& entity : pending)
{
    areaB.AcceptMigratingEntity(entity);
}
areaA.ClearPendingMigrations();
```

## Instance Creation and Lifecycle

### Creating an Area Server Instance

```cpp
// Step 1: Configure the area server
Spark::Net::AreaServerConfig config;
config.areaId = 1;
config.areaName = "ForestZone";
config.scenePath = "Scenes/Forest.scene";
config.port = 27100;            // Client-facing port
config.interServerPort = 27101; // Inter-server communication
config.tickRate = 60.0f;        // 60 Hz simulation
config.maxClients = 64;
config.enableAI = true;
config.enablePhysics = true;
config.enableScripting = true;

// Step 2: Start the area server (spawns tick thread)
Spark::Net::AreaServer server;
if (!server.Start(config))
{
    LOG_ERROR("Failed to start area server: {}", config.areaName);
    return;
}

// Step 3: Register with the WorldServer
AreaID assignedId = world.RegisterAreaServer(config);

// Step 4: The area server is now running its own tick loop
// The internal TickLoop() calls:
//   - ProcessClientMessages(dt)
//   - UpdateSimulation(dt)  (physics, AI, ECS, scripting)
//   - CheckEntityBoundaries()  (detect migrations)
```

### Managing Multiple Areas with WorldServer

```cpp
Spark::Net::WorldServerConfig worldConfig;
worldConfig.worldName = "PersistentWorld";
worldConfig.port = 27020;
worldConfig.interServerPort = 27021;
worldConfig.maxTotalClients = 1000;
worldConfig.tickRate = 10.0f;           // WorldServer ticks slower
worldConfig.enableLoadBalancing = true;
worldConfig.loadBalanceInterval = 30.0f;
worldConfig.logFilePath = "world_server.log";

Spark::Net::WorldServer world;
world.Start(worldConfig);

// Register multiple areas
world.RegisterAreaServer(forestConfig);
world.RegisterAreaServer(townConfig);
world.RegisterAreaServer(dungeonConfig);

// Query area information
std::vector<AreaRegistration> areas = world.GetAllAreas();
for (const auto& area : areas)
{
    LOG_INFO("Area '{}': {} clients, CPU={:.0f}%, online={}",
             area.areaName, area.currentClients,
             area.cpuLoad * 100.0f, area.isOnline);
}

// Find which area contains a world position
AreaID areaForPos = world.GetAreaForPosition({500.0f, 0.0f, 300.0f});
```

## Cross-Area Messaging

AreaServers communicate directly with each other for interactions that span area boundaries (e.g., projectiles crossing zones, voice chat range, environmental effects).

```cpp
// Register a handler for cross-area messages on AreaServer B
server.RegisterCrossAreaHandler(MessageType::GameEvent,
    [](AreaID sourceArea, const NetworkMessage& msg) {
        LOG_INFO("Received cross-area message from area {}", sourceArea);
        // Handle projectile impact, voice data, etc.
    });

// Send a cross-area message from AreaServer A
NetworkMessage msg;
// ... populate message ...
server.SendCrossAreaMessage(targetAreaId, msg);
```

## Load Balancing Strategies

The WorldServer performs load balancing at configurable intervals (default: 30 seconds). The `PerformLoadBalancing()` method evaluates each area server's metrics:

### Metrics Tracked per Area

```cpp
struct AreaRegistration
{
    // ... identity fields ...
    int currentClients;       // Player count
    int maxClients;           // Capacity
    float cpuLoad;            // CPU usage [0.0, 1.0]
    size_t memoryUsageMB;     // Memory footprint
    bool isOnline;            // Accepting connections
    // ... heartbeat tracking ...
};
```

### Thresholds

| Metric | Threshold | Action |
|--------|-----------|--------|
| CPU load > 80% | Overloaded | Flag for player redistribution or area splitting |
| CPU load < 30% | Underloaded | Candidate for area merging |
| No heartbeat for 60s | Timeout | Mark offline, redirect players |
| Client count >= maxClients | Full | Route new players to alternate area |

### WorldServer Statistics

```cpp
const WorldServerStats& stats = world.GetStats();

LOG_INFO("World '{}' uptime: {:.0f}s", world.GetConfig().worldName, stats.uptimeSeconds);
LOG_INFO("Players: {}/{} (peak: {})",
         stats.totalPlayers, world.GetConfig().maxTotalClients, stats.peakPlayers);
LOG_INFO("Active areas: {}", stats.activeAreas);
LOG_INFO("Area transfers: {} (failed: {})", stats.totalAreaTransfers, stats.failedTransfers);
LOG_INFO("Entity migrations: {}", stats.totalEntityMigrations);
LOG_INFO("Load balance events: {}", stats.loadBalanceEvents);
LOG_INFO("Average area load: {:.1f}%", stats.averageAreaLoad * 100.0f);
```

## Area Server Statistics and Monitoring

Each AreaServer exposes detailed runtime metrics:

```cpp
AreaServerStats stats = server.GetStats();  // Thread-safe (mutex-guarded)

LOG_INFO("Area '{}' stats:", server.GetAreaName());
LOG_INFO("  Uptime: {:.0f}s, Ticks: {}", stats.uptimeSeconds, stats.totalTicks);
LOG_INFO("  Tick time: avg={:.2f}ms, peak={:.2f}ms", stats.averageTickMs, stats.peakTickMs);
LOG_INFO("  Entities: {}, Clients: {}", stats.entityCount, stats.clientCount);
LOG_INFO("  Memory: {} MB, CPU: {:.1f}%", stats.memoryUsageMB, stats.cpuUsagePercent);
LOG_INFO("  Migrated: {} in, {} out", stats.entitiesMigratedIn, stats.entitiesMigratedOut);

// Console command output
std::string statusStr = server.Console_GetStatus();
```

## Console Commands

Both AreaServer and WorldServer expose console commands for runtime inspection:

```
# WorldServer commands
world_status              # Show WorldServer status, uptime, player count
world_areas               # List all registered area servers with load
world_players             # List all connected players and their areas
world_transfer <id> <area># Force transfer a player to another area
world_balance             # Force an immediate load balancing pass
world_broadcast <message> # Broadcast a message to all players

# AreaServer commands
area_status               # Show current area server stats
area_clients              # List connected clients with heartbeat times
area_entities             # List tracked entities with positions
area_migrate <eid> <area> # Force migrate an entity to another area
```

## Scaling Patterns

### Horizontal Scaling (Multiple Machines)

```
Machine A                Machine B                Machine C
┌──────────────┐        ┌──────────────┐         ┌──────────────┐
│ WorldServer  │        │ AreaServer   │         │ AreaServer   │
│ AreaServer 1 │◄──────►│ AreaServer 3 │◄───────►│ AreaServer 5 │
│ AreaServer 2 │        │ AreaServer 4 │         │ AreaServer 6 │
└──────────────┘        └──────────────┘         └──────────────┘
```

Each AreaServer is a self-contained process with its own ECS, physics, AI, and scripting simulation. The `AreaRegistration::hostAddress` field allows area servers to run on different physical machines, coordinated by the WorldServer's inter-server communication port.

### Dynamic Area Splitting

When an area becomes overloaded, the WorldServer can split it by registering a new AreaServer for a sub-region and migrating entities:

```cpp
// Detect overload
const AreaRegistration* info = world.GetAreaInfo(overloadedAreaId);
if (info && info->cpuLoad > LOAD_BALANCE_OVERLOAD_THRESHOLD)
{
    // Create a new area server for half the region
    AreaServerConfig splitConfig;
    splitConfig.areaName = info->areaName + "_Split";
    splitConfig.scenePath = info->areaName + "_half.scene";
    splitConfig.tickRate = 60.0f;
    splitConfig.maxClients = info->maxClients / 2;

    AreaID newArea = world.RegisterAreaServer(splitConfig);

    // Migrate players in the split region
    // (Application-specific logic to select which players move)
}
```

### Heartbeat and Fault Tolerance

The WorldServer monitors area servers via heartbeats stored in `AreaRegistration::lastHeartbeat`. If an area server stops sending heartbeats (30-second timeout defined by `HEARTBEAT_TIMEOUT` in `AreaServer`), the WorldServer marks it offline and redirects players to the nearest available area.

```cpp
// AreaServer internal heartbeat tracking per client
struct ClientRecord
{
    ClientID clientId;
    float lastHeartbeatTime;
};
// Clients exceeding HEARTBEAT_TIMEOUT (30s) are disconnected

// WorldServer checks area heartbeats in ProcessAreaHeartbeats()
// Areas exceeding timeout are marked isOnline = false
```

## Thread Safety and Concurrency

The area server architecture is designed for multi-threaded operation. Understanding the thread safety guarantees is essential for correct integration.

### AreaServer Thread Model

Each `AreaServer` spawns its own tick thread via `TickLoop()`. The following data is protected by mutexes:

| Data | Mutex | Access Pattern |
|------|-------|----------------|
| `m_stats` | `m_statsMutex` | Written by tick thread, read by monitoring |
| `m_pendingMigrations` | `m_migrationMutex` | Written by tick thread, read by WorldServer |
| `m_crossAreaMessageQueue` | `m_crossAreaMutex` | Written by any sender, read by tick thread |
| `m_connectedClients` | `m_clientMutex` | Written by Add/RemoveClient, read by tick |

```cpp
// Safe: GetStats() acquires a mutex copy
AreaServerStats stats = server.GetStats();

// Safe: AddClient/RemoveClient are mutex-guarded
server.AddClient(clientId);
server.RemoveClient(clientId);

// Safe: Cross-area messages are queued with mutex protection
server.SendCrossAreaMessage(targetAreaId, msg);
```

### WorldServer Thread Model

The WorldServer uses a similar pattern with separate mutexes for areas, players, and messages:

```cpp
// Thread-safe area registration
AreaID id = world.RegisterAreaServer(config);   // Acquires m_areaMutex

// Thread-safe player operations
AreaID area = world.HandlePlayerConnect(clientId, name, pos);  // Acquires m_playerMutex

// Thread-safe message processing
world.BroadcastToAllAreas(msg);    // Acquires m_messageMutex
world.BroadcastToAllPlayers(msg);  // Acquires m_playerMutex + m_areaMutex
```

## Error Handling and Recovery

### Failed Entity Migrations

When an entity migration fails (target area offline, serialization error, capacity exceeded), the source AreaServer retains the entity:

```cpp
bool migrated = server.MigrateEntityOut(entityNetworkID, targetAreaId);
if (!migrated)
{
    // Entity remains in the source area
    // WorldServer increments failedTransfers counter
    LOG_WARN("Migration failed for entity {} to area {}",
             entityNetworkID, targetAreaId);
}
```

### Area Server Crash Recovery

When an AreaServer stops sending heartbeats, the WorldServer follows this recovery sequence:

```
1. WorldServer detects heartbeat timeout (30s)
2. Area marked isOnline = false
3. Players in the crashed area receive redirect to nearest available area
4. WorldServer logs the failure and increments failedTransfers
5. Operator can restart the AreaServer and re-register it
```

```cpp
// Manual area server restart and re-registration
Spark::Net::AreaServer newServer;
if (newServer.Start(config))
{
    AreaID reassigned = world.RegisterAreaServer(config);
    LOG_INFO("Area '{}' recovered and re-registered as ID {}",
             config.areaName, reassigned);
}
```

### Graceful Shutdown Sequence

```cpp
// Step 1: Stop accepting new players
// Step 2: Migrate all entities to adjacent areas
for (const auto& [netId, entity] : server.GetTrackedEntities())
{
    server.MigrateEntityOut(netId, fallbackAreaId);
}

// Step 3: Disconnect remaining clients
// Step 4: Unregister from WorldServer
world.UnregisterAreaServer(areaId);

// Step 5: Stop the area server
server.Stop();
```

## Advanced Configuration Patterns

### Instanced Dungeons

For instanced content (dungeons, battlegrounds), create temporary AreaServers on demand:

```cpp
// Create a dungeon instance for a party
Spark::Net::AreaServerConfig dungeonConfig;
dungeonConfig.areaName = std::format("Dungeon_Instance_{}", instanceId);
dungeonConfig.scenePath = "Scenes/Dungeon_Template.scene";
dungeonConfig.tickRate = 60.0f;
dungeonConfig.maxClients = 5;       // Party size
dungeonConfig.enableAI = true;
dungeonConfig.enablePhysics = true;
dungeonConfig.enableScripting = true;

Spark::Net::AreaServer instance;
instance.Start(dungeonConfig);
AreaID id = world.RegisterAreaServer(dungeonConfig);

// Transfer party members into the instance
for (ClientID member : partyMembers)
{
    world.TransferPlayer(member, id);
}

// When the dungeon is complete, migrate players out and destroy the instance
for (ClientID member : partyMembers)
{
    world.TransferPlayer(member, overWorldAreaId);
}
world.UnregisterAreaServer(id);
instance.Stop();
```

### Area Bounds Configuration

Each AreaServer defines an axis-aligned bounding box for entity boundary detection. Entities crossing these bounds trigger migration checks:

```cpp
// Default bounds: -500 to +500 on all axes
// The AreaServer uses m_boundsMin and m_boundsMax internally
// CheckEntityBoundaries() runs every tick to detect entities
// that have moved outside the area bounds

// To customize bounds, set the AreaRegistration position and size
// when registering with the WorldServer:
AreaRegistration reg;
reg.areaPosition = {0.0f, 0.0f, 0.0f};     // World-space center
reg.areaSize = {2000.0f, 500.0f, 2000.0f};  // Full extent
```

### WorldServer Spatial Routing

The WorldServer uses `AreaRegistration::areaPosition` and `AreaRegistration::areaSize` to route players by position:

```cpp
// Find which area contains a given world position
AreaID targetArea = world.GetAreaForPosition({1500.0f, 0.0f, 800.0f});

// This is used internally by HandlePlayerConnect to assign new
// players to the correct area based on their spawn position:
AreaID assigned = world.HandlePlayerConnect(clientId, "Alice",
                                            {1500.0f, 0.0f, 800.0f});
// Returns the AreaID whose AABB contains the spawn position
```

## Monitoring and Diagnostics

### Runtime Monitoring Dashboard

Build a monitoring view using the statistics APIs:

```cpp
// WorldServer aggregate view
const WorldServerStats& ws = world.GetStats();
LOG_INFO("=== World Server Dashboard ===");
LOG_INFO("Uptime: {:.0f}s | Players: {}/{} (peak: {})",
         ws.uptimeSeconds, ws.totalPlayers,
         world.GetConfig().maxTotalClients, ws.peakPlayers);
LOG_INFO("Active areas: {} | Avg load: {:.1f}%",
         ws.activeAreas, ws.averageAreaLoad * 100.0f);
LOG_INFO("Transfers: {} ok, {} failed | Migrations: {}",
         ws.totalAreaTransfers, ws.failedTransfers,
         ws.totalEntityMigrations);
LOG_INFO("Messages processed: {} | Balance events: {}",
         ws.totalMessagesProcessed, ws.loadBalanceEvents);

// Per-area detail view
for (const auto& area : world.GetAllAreas())
{
    LOG_INFO("  [{}] '{}' — {} clients, CPU={:.0f}%, Mem={} MB, {}",
             area.areaId, area.areaName, area.currentClients,
             area.cpuLoad * 100.0f, area.memoryUsageMB,
             area.isOnline ? "ONLINE" : "OFFLINE");
}
```

### Console Commands Reference

Both AreaServer and WorldServer expose `Console_GetStatus()` methods that return formatted status strings suitable for the engine console:

```cpp
// Display area server status in the console
std::string status = server.Console_GetStatus();
// Example output:
// "AreaServer 'ForestZone' [ID:1] Running
//   Uptime: 3600s | Ticks: 216000 | Avg: 0.45ms | Peak: 2.1ms
//   Entities: 342 | Clients: 28/64 | Memory: 256 MB | CPU: 45.2%
//   Migrated: 15 in, 12 out"

std::string worldStatus = world.Console_GetStatus();
// Example output:
// "WorldServer 'PersistentWorld' Running
//   Uptime: 7200s | Players: 150/1000 (peak: 200)
//   Areas: 5 active | Avg load: 52.3%
//   Transfers: 89 (3 failed) | Migrations: 204"
```

## Integration with SeamlessAreaManager

For single-player or listen-server scenarios, `SeamlessAreaManager` handles area streaming on the client side. In a full multiplayer deployment, the two systems complement each other:

| Concern | Client Side | Server Side |
|---------|-------------|-------------|
| Area loading | `SeamlessAreaManager` | `AreaServer::Start()` |
| Player tracking | `SeamlessAreaManager::SetPlayerState()` | `WorldServer::HandlePlayerConnect()` |
| Area transitions | `SeamlessAreaManager::Update()` | `WorldServer::TransferPlayer()` |
| Entity migration | N/A (server authoritative) | `AreaServer::MigrateEntityOut()` |
| Load balancing | N/A | `WorldServer::PerformLoadBalancing()` |

```cpp
// Client-side: Register a callback to know when areas change
auto& streaming = Spark::Streaming::SeamlessAreaManager::GetInstance();
streaming.RegisterStateCallback([](Spark::Streaming::AreaID areaId,
                                   Spark::Streaming::AreaState newState) {
    if (newState == Spark::Streaming::AreaState::Loaded)
    {
        LOG_INFO("Client loaded area {}", areaId);
    }
});

// Server-side: The WorldServer coordinates the actual game state transfer
world.TransferPlayer(clientId, targetArea);
```

## Related Pages

- [Networking](Networking) — Base networking system
- [Dedicated Server](Dedicated-Server) — Single-process server model
- [Large World Support](Large-World-Support) — Seamless area streaming and origin rebasing
