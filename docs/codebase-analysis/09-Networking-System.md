# 09 — Networking System

**Location:** `SparkEngine/Source/Engine/Networking/`

UDP-based client/server architecture with HeroEngine-inspired area servers, entity replication, client-side prediction, lag compensation, and network interpolation. 21 header files.

---

## Architecture

```
┌─────────────────────────────┐
│        WorldServer          │  Coordinates multiple AreaServers
│  Player routing, load bal.  │  One per game world
├──────┬──────────┬───────────┤
│AreaServer 0 │AreaServer 1 │AreaServer N │  One per world area
│ ECS+Physics  │ ECS+Physics │ ECS+Physics │  Entity migration
├──────┴──────────┴───────────┤
│       NetworkManager        │  Transport, connections, messages
│  Reliable/Unreliable chan.  │
├─────────────────────────────┤
│    ITransport (UDP/Steam)   │  Abstract transport layer
└─────────────────────────────┘
```

---

## NetworkManager — Core Networking

**File:** `SparkEngine/Source/Engine/Networking/NetworkManager.h`

### Lifecycle

```cpp
NetworkManager network;
network.Initialize();

// Server
network.StartServer(27015, 32);  // Port, max clients

// Client
network.Connect("192.168.1.100", 27015);

// Per-frame
network.Update(deltaTime);       // Process I/O, dispatch handlers
network.Shutdown();
```

### Message Types

| Category | Messages |
|----------|----------|
| Connection | Connect, Disconnect, Heartbeat, Ack |
| Entity | Spawn, Destroy, StateUpdate, RPC |
| Input | ClientInput, InputAck |
| Game | ChatMessage, GameStateSync, MatchStart |
| Custom | MessageType::UserDefined (1000+) |

### Message Handling

```cpp
network.RegisterHandler(MessageType::ChatMessage,
    [](const NetworkMessage& msg) {
        std::string text = msg.ReadString();
        DisplayChat(msg.senderId, text);
    });

// Send
NetworkMessage msg(MessageType::ChatMessage);
msg.WriteString("Hello world!");
network.SendToAll(msg, DeliveryMode::Reliable);
network.SendTo(clientId, msg, DeliveryMode::Unreliable);
```

### Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| Port | 27015 | UDP listen port |
| Heartbeat interval | 1.0s | Keepalive frequency |
| Connection timeout | 10.0s | Disconnect on silence |
| Replication interval | 0.05s | Entity state update rate (20 Hz) |
| ACK send rate | ~30 Hz | Acknowledgment frequency |

---

## AreaServer — Per-Area Simulation

**File:** `SparkEngine/Source/Engine/Networking/AreaServer.h`

HeroEngine-inspired area-based server managing a single world area:

```cpp
AreaServer area;
area.Initialize(areaDefinition);
area.Update(deltaTime);   // Tick ECS, physics, AI, scripting

// Entity migration
area.MigrateEntity(entityId, targetAreaServer);
```

- Owns complete ECS registry, physics world, AI system, scripting context
- Entities near boundaries are replicated to adjacent areas
- Seamless handoff when player crosses area boundary

---

## WorldServer — Global Coordinator

**File:** `SparkEngine/Source/Engine/Networking/WorldServer.h`

Coordinates multiple AreaServers:

```cpp
WorldServer world;
world.Initialize();
world.RegisterArea("forest", forestAreaServer);
world.RegisterArea("cave", caveAreaServer);

world.RoutePlayer(playerId, "cave");  // Seamless area transition
world.Update(deltaTime);
```

- Routes players between areas
- Performs load balancing across machines
- Manages global game state (leaderboards, matchmaking)

---

## DedicatedServer — FPS Server

**File:** `SparkEngine/Source/Engine/Networking/DedicatedServer.h`

Standalone FPS server with game mode support:

- Map rotation
- Game mode management (TDM, FFA, CTF)
- RCON (remote console) for administration
- LAN broadcast discovery
- Server browser registration

---

## Entity Replication

### EntityReplicator — Dirty Bitmask Tracking

**File:** `SparkEngine/Source/Engine/Networking/EntityReplicator.h`

TrinityCore-inspired typed field system:

```cpp
EntityReplicator replicator;
replicator.RegisterField<XMFLOAT3>("position", FieldFlags::Interpolated);
replicator.RegisterField<float>("health", FieldFlags::Reliable);
replicator.RegisterField<uint8_t>("state", FieldFlags::Reliable);

// On change
replicator.SetField(entityId, "position", newPos);  // Sets dirty bit

// Serialize only dirty fields
auto packet = replicator.SerializeDirty(entityId);
network.SendToObservers(entityId, packet);
```

### ReplicationFields — Templated Field System

**File:** `SparkEngine/Source/Engine/Networking/ReplicationFields.h`

```cpp
struct PlayerState : public ReplicatedObject {
    ReplicatedField<XMFLOAT3> position{this, "position"};
    ReplicatedField<float> health{this, "health"};
    ReplicatedField<uint8_t> weapon{this, "weapon"};
};

// Auto-dirty on write
playerState.position = {10, 0, 5};  // Marks dirty
auto dirtyMask = playerState.GetDirtyMask();
```

### DeltaSnapshotManager

**File:** `SparkEngine/Source/Engine/Networking/DeltaSnapshotManager.h`

Per-connection delta tracking — only sends fields that changed since last ACK:

```cpp
DeltaSnapshotManager snapshots;
auto delta = snapshots.ComputeDelta(entityId, connectionId);
// delta contains only fields changed since connection's last acknowledged snapshot
```

### ConnectionScope — Visibility Filtering

**Files:** `SparkEngine/Source/Engine/Networking/ConnectionScope.h`, `ConnectionScopeFilter.h`

Torque3D-inspired per-connection entity scoping:

```cpp
ConnectionScope scope;
scope.SetScopeRadius(connectionId, 200.0f);  // Only see entities within 200m
scope.SetAlwaysRelevant(entityId, true);      // Global entities (sun, weather)

auto relevant = scope.GetRelevantEntities(connectionId);
// Only replicate entities in this set
```

Prevents info leaks (fog of war, client privacy).

---

## Client-Side Prediction

**File:** `SparkEngine/Source/Engine/Networking/ClientPrediction.h`

Responsive FPS input handling:

```cpp
ClientPrediction prediction;

// Client: record input and predict locally
prediction.RecordInput(inputFrame, {moveDir, shootPressed, jumpPressed});
prediction.PredictLocally(inputFrame, playerState);

// On server state received: reconcile
prediction.OnServerState(serverFrame, serverState);
// Re-simulates from last acknowledged input to current
```

- Circular buffer of input frames
- Server reconciliation on mismatch
- Rollback and re-simulate on correction

---

## Network Interpolation

**File:** `SparkEngine/Source/Engine/Networking/NetworkInterpolation.h`

Smooth remote entity motion:

```cpp
NetworkInterpolation interp;
interp.SetBufferTime(0.1f);  // 100ms interpolation buffer

// On state received
interp.AddSnapshot(entityId, timestamp, position, rotation);

// Per-frame render
auto [pos, rot] = interp.GetInterpolatedState(entityId, renderTime);
```

Ring-buffer based with configurable delay to handle jitter.

---

## Lag Compensation

**File:** `SparkEngine/Source/Engine/Networking/NetworkManager.h` (internal)

Hitbox rewinding for fair hit detection:

- 1.0s history of entity positions (default)
- On hit-check: rewind entities to shooter's perceived time
- Verify hit against historical positions
- RTT estimation using Jacobson/Karels algorithm
- Adaptive retransmission timeouts (RTO)

---

## Transport Layer

### ITransport — Abstract Interface

**File:** `SparkEngine/Source/Engine/Networking/ITransport.h`

```cpp
class ITransport {
public:
    virtual bool Initialize(uint16_t port) = 0;
    virtual void Shutdown() = 0;
    virtual bool Send(const Address& to, const uint8_t* data, size_t size) = 0;
    virtual bool Receive(Address& from, uint8_t* buffer, size_t bufSize, size_t& received) = 0;
};
```

### Implementations

| Transport | File | Status |
|-----------|------|--------|
| UDPTransport | `UDPTransport.h` | Active — raw UDP sockets |
| SteamTransport | `SteamTransport.h` | Stub — Steam Networking Sockets |

---

## Network Testing

### InstabilitySimulator

**File:** `SparkEngine/Source/Engine/Networking/InstabilitySimulator.h`

Inject artificial network conditions for testing:

```cpp
InstabilitySimulator sim;
sim.SetPacketLoss(0.05f);     // 5% packet loss
sim.SetLatency(50.0f);         // 50ms base latency
sim.SetJitter(20.0f);          // ±20ms jitter
sim.SetDuplication(0.01f);     // 1% duplicate packets
```

### NetworkEncryption

**File:** `SparkEngine/Source/Engine/Networking/NetworkEncryption.h`

Lightweight symmetric encryption for packet security.

### NetworkSecurity

**File:** `SparkEngine/Source/Engine/Networking/NetworkSecurity.h`

Packet encryption + connection token authentication.

---

## Thread Safety

- Queue mutex protects I/O and handler registration
- Socket operations on dedicated thread
- Message dispatch on main thread via queue drain
