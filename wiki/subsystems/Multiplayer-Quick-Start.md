# Multiplayer Quick Start

A practical guide to getting multiplayer working in SparkEngine. Covers the core networking APIs, entity replication, client-side prediction, lag compensation, and local testing tools.

**Prerequisites:** Basic familiarity with SparkEngine's [ECS](Entity-Component-System.md) and [game module](../getting-started/Creating-a-Game-Module.md) system. For the full API reference, see [Networking](Networking.md).

---

## 1. Overview

SparkEngine's networking stack provides:

- **UDP client/server** architecture on configurable ports (default 27015)
- **Entity replication** with bitmask-driven dirty tracking at 20 Hz
- **Client-side prediction** with input buffering and server reconciliation
- **Lag compensation** via server-side hitbox history rewinding
- **Reliable and unreliable** message channels with ACK-based retransmission
- **Dedicated server** support with RCON, map rotation, and LAN discovery
- **Instability simulation** for testing under packet loss, latency, and jitter

All networking types live in `Spark::Net`. The `ClientPrediction` class lives in `Spark`.

---

## 2. Enabling Networking

Networking is **enabled by default** (`ENABLE_NETWORKING=ON`). To verify or toggle:

```bash
# Explicit enable (default)
cmake -B build -DENABLE_NETWORKING=ON

# Disable for single-player-only builds
cmake -B build -DENABLE_NETWORKING=OFF
```

When disabled, a `NetworkManagerStub` is compiled in its place -- all calls become no-ops and `GetRole()` returns `NetworkRole::None`. No `#ifdef` guards needed in game code.

Verify at runtime with the console command:

```
net_status
```

This prints the current role (`Server`, `Client`, or `None`) and connection state.

---

## 3. Quick Start: Host + Client

### Hosting a Server

```cpp
#include "Engine/Networking/NetworkManager.h"

auto& net = Spark::Net::NetworkManager::GetInstance();
net.Initialize();
net.StartServer(27015, 16);  // port, max clients

// Main loop
while (running)
{
    float dt = timer.GetDeltaTime();
    net.Update(dt);       // process incoming/outgoing messages
    physics.Update(dt);
    ecs.Update(dt);
}

net.Shutdown();
```

### Connecting as a Client

```cpp
auto& net = Spark::Net::NetworkManager::GetInstance();
net.Initialize();
net.Connect("127.0.0.1", 27015, "PlayerOne");

// Main loop
while (running)
{
    float dt = timer.GetDeltaTime();
    net.Update(dt);
    // ... game logic, rendering ...
}

net.Disconnect();
net.Shutdown();
```

### Using Console Commands

From a game module with `SimpleConsole` integration:

```
net_host 27015 16       # Start server on port 27015, max 16 players
net_connect 127.0.0.1   # Connect to localhost (default port 27015)
net_disconnect           # Disconnect
```

---

## 4. Entity Replication

The server replicates entity state to all clients automatically at 20 Hz. Two approaches are available:

### String-Based Properties (NetworkManager)

```cpp
using namespace Spark::Net;

ReplicatedEntity entity;
entity.entityType = "PlayerCharacter";
entity.ownerID = clientId;
entity.position = spawnPosition;

ReplicatedProperty healthProp;
healthProp.name = "health";
healthProp.type = ReplicatedProperty::Type::Float;
healthProp.serialize = [&](NetBuffer& buf) { buf.WriteFloat(m_health); };
healthProp.deserialize = [&](NetBuffer& buf) { m_health = buf.ReadFloat(); };
entity.properties.push_back(healthProp);

uint32_t netId = net.RegisterReplicatedEntity(entity);
net.MarkPropertyDirty(netId, "health");  // after changing health
```

### Typed Fields (EntityReplicator, Bitmask System)

Higher-performance approach with automatic dirty tracking -- up to 64 fields per entity:

```cpp
#include "Engine/Networking/ReplicationFields.h"

ReplicatedField<float> m_health{0, FieldVisibility::Public};   // all clients see this
ReplicatedField<int32_t> m_ammo{1, FieldVisibility::Private};  // owner only

m_health.Set(75.0f);  // dirty bit set automatically, delta-sent next tick
```

Visibility levels: `Public` (all clients), `Private` (owner only), `Party` (squad), `Spectator`.

Only dirty fields are sent each tick. Full syncs happen automatically for new clients, or manually via `net.SendFullEntitySync(clientId)`.

---

## 5. Client-Side Prediction

Prediction provides responsive movement by applying inputs locally, then reconciling with the authoritative server state.

### Setup

```cpp
#include "Engine/Networking/ClientPrediction.h"

Spark::ClientPrediction prediction;
prediction.SetMaxPendingInputs(128);
prediction.SetSmoothCorrection(true, 10.0f);  // smooth corrections over ~0.1s
```

### Each Frame (Client)

```cpp
// 1. Capture and record input
Spark::PredictedInput input;
input.moveDirection = GetInputDirection();
input.jump = IsJumpPressed();
input.sprint = IsSprintHeld();
uint32_t seq = prediction.RecordInput(input);  // auto-assigns sequence number

// 2. Apply locally for instant response
Spark::PredictedState state = prediction.GetState();
prediction.ApplyPrediction(state, input, deltaTime);

// 3. Send input to server
Spark::Net::ClientInputState netInput;
netInput.inputSequence = seq;
netInput.moveForward = input.moveDirection.z;
netInput.moveRight = input.moveDirection.x;
netInput.jump = input.jump;
netInput.deltaTime = deltaTime;
net.SendClientInput(netInput);
```

### On Server Correction

```cpp
// When server state arrives, reconcile: snap to server, re-apply unACKed inputs
prediction.Reconcile(serverState, fixedDeltaTime);

// Query correction magnitude (useful for debugging rubber-banding)
float correction = prediction.GetLastCorrectionMagnitude();
```

### Custom Movement Simulator

Override the default FPS movement logic:

```cpp
prediction.SetMovementSimulator(
    [](Spark::PredictedState& state, const Spark::PredictedInput& input, float dt)
    {
        // Your custom movement physics here
        constexpr float speed = 6.0f;
        state.position.x += input.moveDirection.x * speed * dt;
        state.position.z += input.moveDirection.z * speed * dt;
    });
```

---

## 6. Lag Compensation

For FPS hit detection, the server rewinds entity positions to the time the shooter fired.

Each server tick, record a snapshot of all entity positions and AABBs. When a client reports a shot, rewind to verify:

```cpp
Spark::Net::LagCompensator& lag = net.GetLagCompensator();
lag.SetMaxHistoryDuration(1.0f);  // keep 1 second of history

// Each tick: record snapshot
Spark::Net::HistorySnapshot snap;
snap.timestamp = net.GetServerTime();
for (const auto& e : allEntities)
    snap.entities.push_back({e.networkID, e.position, e.rotation, e.boundsMin, e.boundsMax});
lag.RecordSnapshot(snap);

// When processing a shot: rewind and verify
Spark::Net::HistorySnapshot rewound;
if (lag.RewindToTime(clientShootTimestamp, rewound))
{
    for (const auto& ent : rewound.entities)
        if (RayIntersectsAABB(shootRay, ent.boundsMin, ent.boundsMax))
            { ApplyDamage(ent.networkID, damage); break; }
}
```

---

## 7. Message Types and Custom Handlers

Register handlers for built-in or custom message types. User-defined types start at `MessageType::UserDefined` (1000):

```cpp
constexpr auto MyGameEvent =
    static_cast<Spark::Net::MessageType>(static_cast<uint16_t>(Spark::Net::MessageType::UserDefined) + 1);

// Send a custom reliable message
Spark::Net::NetBuffer buf;
buf.WriteUint32(eventId);
buf.WriteVector3(eventPosition);

Spark::Net::NetworkMessage msg;
msg.type = MyGameEvent;
msg.channel = Spark::Net::ChannelType::Reliable;
msg.payload = buf.GetData();
net.SendMessage(msg);

// Register handler on the receiving end
net.RegisterHandler(MyGameEvent,
    [](const Spark::Net::NetworkMessage& msg) { /* handle */ });
```

**Channel types:** `Unreliable` (fire-and-forget, for position updates), `Reliable` (ACKed delivery, for game events), `ReliableOrdered` (strict ordering, for score updates).

---

## 8. Dedicated Server Mode

```cpp
#include "Engine/Networking/DedicatedServer.h"

Spark::Net::DedicatedServer server;
Spark::Net::ServerConfig config;
config.serverName = "My Game Server";
config.port = 27015;
config.maxClients = 24;
config.tickRate = 64.0f;
config.gameMode = Spark::Net::GameModeType::TeamDeathmatch;
config.mapRotation = {"dm_arena", "dm_warehouse", "dm_rooftop"};
config.rconPassword = "admin123";

server.Start(config);  // launches tick loop on background thread
```

**Running headless** -- two approaches:

```bash
./SparkEngine -headless -game SparkGame.dll              # runtime headless (game module)
./SparkServer --manifest spark.modules.json --port 27015 --max-clients 32  # compile-time headless (built-in)
```

For the built-in server, build with `-DENABLE_GRAPHICS=OFF -DENABLE_SERVER_PROCESSES=ON`.
Every `SparkServer` launch must select game code with either `--manifest <path>`
or `--module <game-library>`. See [Dedicated Server](Dedicated-Server.md) for full details.

**RCON** -- built-in commands: `help`, `status`, `kick`, `ban`, `map`, `say`. Add custom ones:

```cpp
server.RegisterRconCommand("restart", "Restart match",
    [&](const std::vector<std::string>&) -> std::string {
        server.EndMatch(); server.StartMatch(); return "Match restarted.";
    });
```

---

## 9. Testing Multiplayer Locally

### InstabilitySimulator

Inject artificial latency, packet loss, jitter, and reordering via code or console:

```cpp
auto& sim = Spark::Net::InstabilitySimulator::GetInstance();
Spark::Net::InstabilitySettings settings;
settings.enabled = true;
settings.latencyMs = 100.0f;       // 100ms added latency
settings.jitterMs = 20.0f;         // +/- 20ms variance
settings.packetLossPercent = 5.0f; // 5% packet loss
sim.SetSettings(settings);
```

Or use console commands for live tuning: `net.lag 100`, `net.loss 5`, `net.jitter 20`, `net.reorder 10`.

### Two-Instance Local Test

```bash
# Terminal 1 (server):  ./SparkEngine -headless -game SparkGame.dll
# Terminal 2 (client):  ./SparkEngine -game SparkGame.dll
#   then in console:    net_connect 127.0.0.1 27015
```

### Auto-Reconnect

```cpp
Spark::Net::NetworkManager::AutoReconnectConfig reconnect;
reconnect.enabled = true;
reconnect.baseDelay = 2.0f;   // initial delay, doubles each retry up to maxDelay
reconnect.maxDelay = 30.0f;
reconnect.maxAttempts = 5;
net.SetAutoReconnect(reconnect);
```

---

## 10. Console Commands

| Command | Description |
|---------|-------------|
| `net_status` | Show connection state and role (Server/Client/None) |
| `net_stats` | Show ping, jitter, packet loss, bandwidth |
| `net_clients` | List connected clients with stats (server only) |
| `net_host [port] [max]` | Start server on port (default 27015) |
| `net_connect <addr> [port]` | Connect to a server |
| `net_disconnect` | Disconnect from current server |
| `net_stack_status` | Show transport and encryption status |
| `prediction_status` | Show pending input count and correction magnitude |
| `server_status` | Show DedicatedServer uptime, players, map, match state |
| `net.lag <ms>` | Set simulated latency |
| `net.loss <percent>` | Set simulated packet loss |
| `net.jitter <ms>` | Set simulated jitter |
| `net.reorder <percent>` | Set simulated reordering |

---

## See Also

- [Networking](Networking.md) -- Full API reference and architecture
- [Dedicated Server](Dedicated-Server.md) -- Server deployment options
- [Area Server Architecture](Area-Server-Architecture.md) -- MMO-scale distributed servers
- [Networking Wire Format](../specifications/Networking-Wire-Format.md) -- Binary packet format details
- [Entity Component System](Entity-Component-System.md) -- NetworkIdentity component
- [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md) -- Multiplayer game modes
