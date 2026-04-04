# Making Your First Multiplayer Game

This tutorial walks through building a simple multiplayer FPS prototype using SparkEngine's networking systems. By the end you will have two players connecting over localhost, moving, shooting, and seeing each other's state in real time.

## Prerequisites

- SparkEngine built from source (see [Build System and CMake Modules](Build-System-and-CMake-Modules))
- Completed the single-player [Getting Started](Getting-Started) tutorial
- Basic understanding of client/server architecture

## 1. Network Architecture Overview

SparkEngine uses an authoritative server model over UDP:

- **Server** owns the game state. It validates all player actions and broadcasts the canonical world state.
- **Clients** send input to the server and render the state they receive.
- **Tick rate**: The server runs its simulation at a fixed 60 Hz tick rate. Clients send input every frame and interpolate between received snapshots.
- **Reliability**: Critical messages (connect, disconnect, RPC) use reliable ordered delivery. State updates use unreliable channels for low latency.

```
┌──────────┐         UDP          ┌──────────┐
│  Client   │ ──── Input ───────► │  Server   │
│           │ ◄── State Sync ──── │           │
└──────────┘                      └──────────┘
```

## 2. Setting Up a Server

Use `NetworkManager` to start a server on a port. The server listens for incoming connections and drives the simulation loop.

```cpp
#include "Engine/Networking/NetworkManager.h"
using namespace Spark::Net;

void StartGameServer()
{
    auto& net = NetworkManager::GetInstance();
    net.Initialize();

    // Start listening on port 27015, max 16 clients
    if (!net.StartServer(27015, 16))
    {
        // Handle error
        return;
    }

    // Register message handlers
    net.RegisterHandler(MessageType::ClientInput,
        [](const NetworkMessage& msg)
        {
            // Process player input (see Section 6)
        });
}
```

Call `net.Update(deltaTime)` every frame in your server game loop to process incoming packets and send state updates.

## 3. Creating a Client Connection

On the client side, connect to the server's IP address:

```cpp
void ConnectToServer(const std::string& serverAddress)
{
    auto& net = NetworkManager::GetInstance();
    net.Initialize();

    if (!net.Connect(serverAddress, 27015, "PlayerOne"))
    {
        // Connection failed
        return;
    }

    // Register handlers for state updates
    net.RegisterHandler(MessageType::EntityStateUpdate,
        [](const NetworkMessage& msg)
        {
            // Apply remote entity state (see Section 5)
        });

    net.RegisterHandler(MessageType::EntitySpawn,
        [](const NetworkMessage& msg)
        {
            // Spawn a remote player entity
        });
}
```

## 4. Player Entity Replication

Every networked entity needs a `ReplicatedEntity` registration. The server assigns a unique `networkID` and tracks which properties are dirty.

```cpp
void RegisterPlayerEntity(uint32_t localEntityId, ClientID ownerClient)
{
    auto& net = NetworkManager::GetInstance();

    ReplicatedEntity entity;
    entity.networkID = 0; // Assigned by RegisterReplicatedEntity
    entity.ownerID = ownerClient;
    entity.entityType = "Player";

    // Register position property
    entity.properties.push_back({
        "position",
        ReplicatedProperty::Type::Vector3,
        [localEntityId](NetBuffer& buf)
        {
            // Serialize current position into buffer
            XMFLOAT3 pos = GetEntityPosition(localEntityId);
            buf.WriteVector3(pos);
        },
        [localEntityId](NetBuffer& buf)
        {
            // Deserialize and apply position
            XMFLOAT3 pos = buf.ReadVector3();
            SetEntityPosition(localEntityId, pos);
        }
    });

    // Register health property
    entity.properties.push_back({
        "health",
        ReplicatedProperty::Type::Float,
        [localEntityId](NetBuffer& buf)
        {
            buf.WriteFloat(GetEntityHealth(localEntityId));
        },
        [localEntityId](NetBuffer& buf)
        {
            float hp = buf.ReadFloat();
            SetEntityHealth(localEntityId, hp);
        }
    });

    uint32_t netId = net.RegisterReplicatedEntity(entity);
}
```

## 5. Sending Player Input to the Server

Clients sample input each frame and send it as a `ClientInputState`:

```cpp
void SendInput()
{
    auto& net = NetworkManager::GetInstance();

    ClientInputState input;
    input.inputSequence = nextSequence++;
    input.moveForward = GetAxis("MoveForward");  // -1 to 1
    input.moveRight = GetAxis("MoveRight");       // -1 to 1
    input.lookYaw = GetMouseDeltaX();
    input.lookPitch = GetMouseDeltaY();
    input.fire = IsButtonDown("Fire");
    input.jump = IsButtonPressed("Jump");
    input.sprint = IsButtonDown("Sprint");
    input.deltaTime = frameDeltaTime;
    input.timestamp = localTime;

    net.SendClientInput(input);

    // Store for client-side prediction reconciliation
    pendingInputs.push_back(input);
}
```

## 6. Server-Side Movement and Validation

The server processes each client's input, validates it, and applies movement:

```cpp
void ServerProcessInputs(float deltaTime)
{
    auto& net = NetworkManager::GetInstance();
    const auto& inputs = net.GetPendingInputs();

    for (const auto& input : inputs)
    {
        // Find the player entity for this client
        auto* entity = FindPlayerByClient(input.senderID);
        if (!entity) continue;

        // Validate input (anti-cheat: clamp values, check timing)
        float forward = std::clamp(input.moveForward, -1.0f, 1.0f);
        float right = std::clamp(input.moveRight, -1.0f, 1.0f);

        // Apply movement
        float speed = input.sprint ? 8.0f : 5.0f;
        XMFLOAT3 movement;
        movement.x = right * speed * input.deltaTime;
        movement.y = 0.0f;
        movement.z = forward * speed * input.deltaTime;

        ApplyMovement(entity, movement);

        // Mark position dirty for replication
        net.MarkPropertyDirty(entity->networkID, "position");
    }
}
```

## 7. Client-Side Prediction and Reconciliation

To avoid input lag, the client predicts its own movement locally and reconciles when the server confirms:

```cpp
void ClientReconcile(uint32_t confirmedSequence, const XMFLOAT3& serverPosition)
{
    // Remove all inputs up to the confirmed sequence
    while (!pendingInputs.empty() &&
           pendingInputs.front().inputSequence <= confirmedSequence)
    {
        pendingInputs.erase(pendingInputs.begin());
    }

    // Snap to server position
    SetLocalPlayerPosition(serverPosition);

    // Re-apply remaining unconfirmed inputs
    for (const auto& input : pendingInputs)
    {
        float speed = input.sprint ? 8.0f : 5.0f;
        XMFLOAT3 movement;
        movement.x = input.moveRight * speed * input.deltaTime;
        movement.y = 0.0f;
        movement.z = input.moveForward * speed * input.deltaTime;
        ApplyLocalMovement(movement);
    }
}
```

## 8. Projectile Replication

When a player fires, the server validates and broadcasts:

```cpp
// Client: send fire request
void ClientFire(const XMFLOAT3& origin, const XMFLOAT3& direction)
{
    NetworkMessage msg;
    msg.type = MessageType::EntityRPC;
    msg.channel = ChannelType::ReliableOrdered;

    NetBuffer buf;
    buf.WriteVector3(origin);
    buf.WriteVector3(direction);
    msg.payload = buf.GetData();

    NetworkManager::GetInstance().SendMessage(msg);
}

// Server: validate and broadcast
void ServerHandleFire(ClientID shooter, const XMFLOAT3& origin,
                      const XMFLOAT3& direction)
{
    auto* player = FindPlayerByClient(shooter);
    if (!player) return;

    // Validate: is origin close to player's actual position?
    float dist = Distance(player->position, origin);
    if (dist > 2.0f) return; // Reject suspicious fire

    // Spawn projectile server-side
    uint32_t projId = SpawnProjectile(origin, direction, shooter);

    // Broadcast to all clients
    NetworkMessage spawnMsg;
    spawnMsg.type = MessageType::EntitySpawn;
    NetBuffer buf;
    buf.WriteUint32(projId);
    buf.WriteVector3(origin);
    buf.WriteVector3(direction);
    spawnMsg.payload = buf.GetData();

    NetworkManager::GetInstance().SendToAll(spawnMsg);
}
```

## 9. Handling Disconnects and Reconnects

Register timeout and disconnect handlers:

```cpp
void SetupDisconnectHandling()
{
    auto& net = NetworkManager::GetInstance();

    // Server-side: handle client timeout
    net.SetTimeoutHandler([](ClientID client)
    {
        // Remove player entity, notify other clients
        RemovePlayerEntity(client);
        BroadcastPlayerLeft(client);
    });

    // Client-side: enable auto-reconnect
    NetworkManager::AutoReconnectConfig config;
    config.enabled = true;
    config.baseDelay = 2.0f;
    config.maxAttempts = 5;
    net.SetAutoReconnect(config);

    net.SetReconnectFailedCallback([]()
    {
        ShowDisconnectedUI();
    });
}
```

## 10. Lag Compensation for Hit Detection

The server rewinds entity positions to the time the shot was fired on the client:

```cpp
void ServerHitscan(ClientID shooter, const XMFLOAT3& origin,
                   const XMFLOAT3& direction, float clientTimestamp)
{
    auto& net = NetworkManager::GetInstance();
    auto& lagComp = net.GetLagCompensator();

    // Estimate client's view of the world at their fire time
    float rtt = net.GetEstimatedRTT() / 1000.0f;
    float rewindTime = clientTimestamp - (rtt * 0.5f);

    // Rewind and test
    // LagCompensator stores historical snapshots for rewinding
    // Perform raycast against rewound hitboxes
    // If hit: apply damage, mark health dirty, broadcast
}
```

## 11. Adding a Scoreboard UI

Track kills and deaths per client, replicate via game state sync:

```cpp
// Server broadcasts score updates
void BroadcastScoreUpdate(ClientID player, int kills, int deaths)
{
    NetworkMessage msg;
    msg.type = MessageType::ScoreUpdate;

    NetBuffer buf;
    buf.WriteUint32(player);
    buf.WriteUint32(static_cast<uint32_t>(kills));
    buf.WriteUint32(static_cast<uint32_t>(deaths));
    msg.payload = buf.GetData();

    NetworkManager::GetInstance().SendToAll(msg);
}

// Client renders scoreboard from received data
void RenderScoreboard(const std::vector<PlayerScore>& scores)
{
    // Use ImGui or engine UI to render a table
    for (const auto& score : scores)
    {
        DrawText(score.name + ": " +
                 std::to_string(score.kills) + "K / " +
                 std::to_string(score.deaths) + "D");
    }
}
```

## 12. Testing Locally

Run two instances on the same machine:

1. **Start the server** in one terminal or instance:
   ```
   SparkEngine.exe --server --port 27015
   ```

2. **Start a client** in another:
   ```
   SparkEngine.exe --connect 127.0.0.1 --port 27015 --name Player1
   ```

3. **Start a second client**:
   ```
   SparkEngine.exe --connect 127.0.0.1 --port 27015 --name Player2
   ```

Both clients should see each other moving and shooting. Use the console command `net_stats` to view ping, packet loss, and bandwidth.

## 13. LAN / Internet Deployment

For LAN play, use the machine's local IP address (e.g. `192.168.1.x`) instead of `127.0.0.1`.

For internet play:

- **Port forwarding**: Forward UDP port 27015 on the server's router.
- **Dedicated server**: Use `SparkEngine.exe --server --headless` for a dedicated server with no rendering overhead (uses NullRHIDevice).
- **Firewall**: Ensure the server port is open in the OS firewall.
- **Bandwidth**: With 16 players at 60 Hz, expect approximately 200-400 KB/s upstream on the server.

## Next Steps

- Read the [Networking](Networking) wiki page for full API details
- See [Area Server Architecture](Area-Server-Architecture) for scaling to larger worlds
- Explore the [Networking Wire Format](Networking-Wire-Format) specification
- Check out the SparkGameFPS module (`GameModules/SparkGameFPS/`) for a complete FPS implementation
