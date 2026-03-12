# Networking

SparkEngine includes a UDP-based networking system for multiplayer games with entity replication, client-side prediction, and lag compensation.

**Source:** `SparkEngine/Source/Engine/Networking/NetworkManager.h`

> **Note:** Networking is disabled by default (`ENABLE_NETWORKING=OFF`) due to CURL dependency issues. Enable it with `-DENABLE_NETWORKING=ON` during CMake configuration.

## Architecture

The networking system uses a client/server model:

```
┌──────────┐     UDP      ┌──────────┐
│  Client  │ ◄──────────► │  Server  │
│          │              │          │
│ Predict  │              │ Authority│
│ Reconcile│              │ Replicate│
└──────────┘              └──────────┘
```

- **Server** — Authoritative game state, processes inputs, replicates state
- **Client** — Predicts locally, reconciles with server corrections

## Message Channels

| Channel | Description |
|---------|-------------|
| **Reliable** | Guaranteed delivery, ordered (game events, chat) |
| **Unreliable** | No delivery guarantee, fastest (position updates) |
| **Ordered** | Ordered delivery, may drop old packets |

## Entity Replication

Entities with `NetworkIdentity` are automatically replicated via the [Entity Component System](Entity-Component-System):

```cpp
auto& net = world.AddComponent<NetworkIdentity>(entity);
net.networkId         = uniqueId;
net.ownerId           = clientId;
net.isLocalPlayer     = true;
net.isServerAuthority = false;
```

The server sends authoritative state updates for replicated entities. Clients receive and apply these updates, smoothing position with interpolation.

## Client-Side Prediction

For responsive gameplay, clients predict the results of local input immediately without waiting for server confirmation:

1. Client applies input locally
2. Client sends input to server
3. Server processes input and sends authoritative result
4. Client reconciles: if server result differs from prediction, client corrects

## Server Reconciliation

When the server's authoritative state differs from the client's prediction:
1. Client rewinds to the server's confirmed state
2. Client re-applies all unconfirmed inputs
3. Result is smoothly blended to avoid visual snapping

## Lag Compensation

For hit detection in fast-paced FPS [gameplay](Gameplay-Systems):

- Server maintains a **1-second history** of entity positions (hitbox rewinding)
- When processing a shot, the server rewinds entities to where they were when the shooter fired
- This ensures that what players see on-screen matches hit detection regardless of latency

## Starting a Server and Connecting

```cpp
NetworkManager network;

// Start a dedicated server
network.StartServer(27015);  // Listen on UDP port 27015

// Or connect as a client
network.Connect("192.168.1.100", 27015);

// Register a message handler
network.RegisterHandler(MessageType::Chat,
    [](ClientID sender, const NetworkMessage& msg) {
        std::string text = msg.ReadString();
        LOG("Chat from " + std::to_string(sender) + ": " + text);
    });

// Send messages on different channels
NetworkMessage chatMsg;
chatMsg.WriteString("Hello team!");
network.Send(chatMsg, MessageType::Chat, Channel::Reliable);

NetworkMessage posMsg;
posMsg.WriteFloat3(playerPosition);
network.Send(posMsg, MessageType::Position, Channel::Unreliable);

// Disconnect cleanly
network.Disconnect();
network.StopServer();
```

## Network Statistics

The `NetworkManager` tracks real-time statistics:

| Metric | Description |
|--------|-------------|
| Ping | Round-trip time (ms) |
| Jitter | Ping variation (ms) |
| Packet Loss | Percentage of lost packets |
| Bandwidth | Bytes sent/received per second |

```cpp
// Query network stats for display in HUD
float ping       = network.GetPing();
float jitter     = network.GetJitter();
float packetLoss = network.GetPacketLoss();  // 0.0 to 1.0
int bytesSent    = network.GetBytesSentPerSecond();
int bytesRecv    = network.GetBytesReceivedPerSecond();
```

## Serialization

Network messages use built-in serialization helpers for efficient packing and unpacking of game state data.

---

## See Also

- [Entity Component System](Entity-Component-System) — NetworkIdentity component
- [Gameplay Systems](Gameplay-Systems) — Multiplayer game modes
- [Event System](Event-System) — Network event handling
- [Scene Management](Scene-Management) — Networked scene transitions
- [Physics](Physics) — Server-authoritative physics
- [Animation](Animation) — Replicated animation states
- [Input System](Input-System) — Client input processing and prediction
