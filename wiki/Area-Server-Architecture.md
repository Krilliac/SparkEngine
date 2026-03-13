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

## Related Pages

- [Networking](Networking) — Base networking system
- [Dedicated Server](Dedicated-Server) — Single-process server model
- [Large World Support](Large-World-Support) — Seamless area streaming and origin rebasing
