# Networking

SparkEngine includes a UDP-based networking system for multiplayer games with entity replication, client-side prediction, lag compensation, pluggable transports, packet encryption, and dedicated server support.

**Source:** `SparkEngine/Source/Engine/Networking/`

> **Note:** Networking is enabled by default (`ENABLE_NETWORKING=ON`). It uses raw UDP sockets with no external dependencies. When disabled via `-DENABLE_NETWORKING=OFF`, a minimal `NetworkManagerStub` is compiled so the rest of the engine links without errors.

## Architecture

The networking subsystem is composed of several layered modules that work together to provide a complete multiplayer stack:

```
┌────────────────────────────────────────────────────────────────────┐
│                          Game Code                                 │
│          (registers handlers, sends messages, queries stats)       │
├────────────────────────────────────────────────────────────────────┤
│                     DedicatedServer                                │
│ (tick loop, map rotation, local admin, LAN discovery, match state)│
├────────────────────────────────────────────────────────────────────┤
│                      NetworkManager                                │
│   (message routing, entity replication, connection management)     │
├────────────────────────────────────────────────────────────────────┤
│  ClientPrediction  │  LagCompensator  │  NetworkSecurity           │
│  (input buffering, │  (history buffer, │  (XOR encryption, token   │
│   reconciliation)  │   hitbox rewind)  │   auth, rate limiting)    │
├────────────────────┴─────────────────┬┴───────────────────────────┤
│                     NetworkStack                                   │
│           (transport + security integration layer)                 │
├────────────────────────────────────────────────────────────────────┤
│                     ITransport (abstract)                          │
│        ┌───────────────────┬───────────────────────┐              │
│        │   UDPTransport    │    SteamTransport      │              │
│        │  (BSD/Winsock)    │   (stub, future SDK)   │              │
│        └───────────────────┴───────────────────────┘              │
└────────────────────────────────────────────────────────────────────┘
```

### Source Files

| File | Responsibility |
|------|---------------|
| `NetworkManager.h` | Core singleton: message routing, entity replication, connection lifecycle |
| `ITransport.h` | Abstract transport interface for packet I/O |
| `UDPTransport.h` | Concrete UDP socket transport (default) |
| `SteamTransport.h` | Stub transport for future Steam Networking Sockets |
| `ClientPrediction.h` | Client-side prediction and server reconciliation |
| `NetworkSecurity.h` | XOR encryption, connection token generation/validation |
| `NetworkEncryption.h` | Per-connection session keys, HMAC integrity, replay protection, rate limiting |
| `NetworkIntegration.h` | `NetworkStack` -- combines transport + security into unified stack |
| `DedicatedServer.h` | Headless server: tick loop, local admin commands, map rotation, LAN broadcast |
| `AreaServer.h` | Per-area server process for scalable multiplayer worlds |
| `WorldServer.h` | Central coordinator for area-based multiplayer architecture |

> **Area Server Architecture** — For MMO-scale multiplayer with multiple area servers coordinated by a WorldServer, see [Area Server Architecture](Area-Server-Architecture.md).

### Namespace

All networking types reside in `Spark::Net`. The `ClientPrediction` class resides in `Spark`.

## Network Types and Constants

```cpp
namespace Spark::Net
{
    using ClientID = uint32_t;
    using SequenceNumber = uint32_t;
    using NetworkTime = float;

    constexpr ClientID INVALID_CLIENT = 0;
    constexpr uint16_t DEFAULT_PORT = 27015;
}
```

### ChannelType

| Value | Description | Use Case |
|-------|-------------|----------|
| `Unreliable` | Fire-and-forget, no delivery guarantee | Position updates, movement data |
| `Reliable` | Guaranteed delivery with ordering | Chat messages, state changes |
| `ReliableOrdered` | Guaranteed delivery, strict in-order | Important game events, score updates |

### NetworkRole

| Value | Description |
|-------|-------------|
| `None` | Not connected |
| `Server` | Acting as authoritative server |
| `Client` | Acting as client connected to server |

### ConnectionState

| Value | Description |
|-------|-------------|
| `Disconnected` | No active connection |
| `Connecting` | Handshake in progress |
| `Connected` | Fully connected and ready |
| `Disconnecting` | Graceful disconnect in progress |

## Message Types

All messages are tagged with a `MessageType` enum that determines routing:

```cpp
enum class MessageType : uint16_t
{
    // Connection lifecycle
    Connect = 1,
    ConnectAccepted,
    ConnectRejected,
    Disconnect,
    Heartbeat,

    // Entity replication
    EntitySpawn,
    EntityDestroy,
    EntityStateUpdate,
    EntityRPC,

    // Input prediction
    ClientInput,
    InputAck,

    // Game logic
    ChatMessage,
    GameStateSync,
    MatchStart,
    MatchEnd,
    PlayerRespawn,
    ScoreUpdate,

    // Custom user-defined messages start at 1000
    UserDefined = 1000
};
```

### NetworkMessage Structure

```cpp
struct NetworkMessage
{
    MessageType type;
    ChannelType channel = ChannelType::Unreliable;
    ClientID senderID = INVALID_CLIENT;
    SequenceNumber sequence = 0;
    std::vector<uint8_t> payload;
    float timestamp = 0.0f;
};
```

## NetBuffer -- Serialization

The `NetBuffer` class provides type-safe binary serialization for network payloads:

| Write Method | Read Method | Data Type |
|-------------|-------------|-----------|
| `WriteUint8(uint8_t)` | `ReadUint8()` | 1 byte unsigned |
| `WriteUint16(uint16_t)` | `ReadUint16()` | 2 byte unsigned |
| `WriteUint32(uint32_t)` | `ReadUint32()` | 4 byte unsigned |
| `WriteFloat(float)` | `ReadFloat()` | 4 byte float |
| `WriteString(const std::string&)` | `ReadString()` | Length-prefixed string |
| `WriteVector3(const XMFLOAT3&)` | `ReadVector3()` | 3 floats (12 bytes) |
| `WriteBytes(const void*, size_t)` | `ReadBytes(void*, size_t)` | Raw byte range |

**Safety methods:**

| Method | Description |
|--------|-------------|
| `CanRead(size_t bytes)` | Check if `bytes` can be read without overrun |
| `HasError()` | True if any read overran the buffer |
| `IsValid()` | Opposite of `HasError()` |
| `RemainingBytes()` | Bytes left to read |
| `GetReadPosition()` | Current read cursor position |
| `Reset()` | Clear data and reset read position |

```cpp
// Writing a player position update
NetBuffer buf;
buf.WriteUint32(playerNetworkID);
buf.WriteVector3(position);
buf.WriteVector3(velocity);
buf.WriteFloat(yaw);

// Reading it back
uint32_t id = buf.ReadUint32();
XMFLOAT3 pos = buf.ReadVector3();
XMFLOAT3 vel = buf.ReadVector3();
float yaw = buf.ReadFloat();
if (buf.HasError()) { /* handle truncated packet */ }
```

## Entity Replication

Entities with replicated state are tracked by the `NetworkManager` via `ReplicatedEntity`:

```cpp
struct ReplicatedProperty
{
    std::string name;
    enum class Type { Int, Float, Vector3, String, Bool } type;
    std::function<void(NetBuffer&)> serialize;
    std::function<void(NetBuffer&)> deserialize;
    bool dirty = false;
};

struct ReplicatedEntity
{
    uint32_t networkID;
    ClientID ownerID;
    std::string entityType;
    std::vector<ReplicatedProperty> properties;
    XMFLOAT3 position{0, 0, 0};
    XMFLOAT3 rotation{0, 0, 0};
    XMFLOAT3 velocity{0, 0, 0};
    float lastUpdateTime = 0.0f;
    bool needsFullSync = true;
};
```

### Replication API

```cpp
// Register an entity for replication (server)
uint32_t netId = network.RegisterReplicatedEntity(entity);

// Mark a property as changed (triggers delta update on next replication tick)
network.MarkPropertyDirty(netId, "health");

// Remove from replication
network.UnregisterReplicatedEntity(netId);

// Get a replicated entity by network ID
ReplicatedEntity* ent = network.GetReplicatedEntity(netId);

// Full sync to a newly connected client
network.SendFullEntitySync(newClientId);

// Manual serialization/deserialization
NetBuffer buf;
network.SerializeEntityState(netId, buf);
network.DeserializeEntityState(buf);
```

The server replicates entity state at a configurable rate (default: **20 Hz**, controlled by `m_replicationInterval = 0.05f`). Only properties marked dirty are sent in delta updates; full syncs are sent when `needsFullSync` is true (e.g., on initial spawn or when a new client connects).

## Client-Side Prediction

The `ClientPrediction` class (in `Spark` namespace) provides responsive FPS movement by predicting locally while reconciling with authoritative server state.

### PredictedInput

```cpp
struct PredictedInput
{
    uint32_t sequenceNumber = 0;
    float timestamp = 0.0f;
    DirectX::XMFLOAT3 moveDirection{0, 0, 0};
    float lookYaw = 0.0f;
    float lookPitch = 0.0f;
    bool jump = false;
    bool crouch = false;
    bool sprint = false;
    bool fire = false;
    bool reload = false;
    bool interact = false;
};
```

### PredictedState

```cpp
struct PredictedState
{
    uint32_t lastProcessedInput = 0;
    DirectX::XMFLOAT3 position{0, 0, 0};
    DirectX::XMFLOAT3 velocity{0, 0, 0};
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool isGrounded = true;
    bool isCrouching = false;
    bool isSprinting = false;
};
```

### Prediction Workflow

```
Client Tick:
  1. RecordInput(input)         --> assigns sequence number, stores in buffer
  2. ApplyPrediction(state, input, dt) --> runs movement locally
  3. Send input to server

Server State Received:
  4. Reconcile(serverState, dt) --> snap to server, re-apply unACKed inputs
```

```cpp
ClientPrediction prediction;
prediction.SetMaxPendingInputs(128);
prediction.SetSmoothCorrection(true, 10.0f);

// Optionally override the movement simulator
prediction.SetMovementSimulator(
    [](PredictedState& state, const PredictedInput& input, float dt) {
        // Custom FPS movement logic
    });

// Each tick
PredictedInput input;
input.moveDirection = GetInputDirection();
input.jump = IsJumpPressed();
uint32_t seq = prediction.RecordInput(input);

PredictedState state = prediction.GetState();
prediction.ApplyPrediction(state, input, deltaTime);

// On server correction
prediction.Reconcile(serverState, fixedDeltaTime);

// Query
size_t pending = prediction.GetPendingInputCount();
float correction = prediction.GetLastCorrectionMagnitude();
```

### Smooth Correction

When reconciliation produces a large position correction, the system can interpolate smoothly rather than snapping:

```cpp
prediction.SetSmoothCorrection(true, 10.0f);  // speed = 10 means ~0.1s to converge
```

The correction offset is maintained internally and blended toward zero each frame.

## Lag Compensation

For hit detection in fast-paced FPS [gameplay](../gameplay-tools/Gameplay-Systems.md), the server maintains a sliding window of entity position history:

```cpp
struct HistorySnapshot
{
    float timestamp;
    struct EntityState
    {
        uint32_t networkID;
        XMFLOAT3 position;
        XMFLOAT3 rotation;
        XMFLOAT3 boundsMin;  // AABB min for hitbox
        XMFLOAT3 boundsMax;  // AABB max for hitbox
    };
    std::vector<EntityState> entities;
};

class LagCompensator
{
public:
    void RecordSnapshot(const HistorySnapshot& snapshot);
    bool RewindToTime(float targetTime, HistorySnapshot& outSnapshot) const;
    void SetMaxHistoryDuration(float seconds);  // Default: 1.0s
    void Clear();
};
```

### How It Works

1. Each server tick, a `HistorySnapshot` is recorded containing all entity positions and AABB hitboxes
2. When processing a shot, the server calls `RewindToTime(shooterTimestamp)` to get the world state at the time the shooter fired
3. Hit detection is performed against the rewound positions
4. Default history duration is **1 second** (configurable via `SetMaxHistoryDuration`)

```cpp
LagCompensator& lag = network.GetLagCompensator();
lag.SetMaxHistoryDuration(1.0f);

// Each tick (server)
HistorySnapshot snap;
snap.timestamp = network.GetServerTime();
// ... populate entity states ...
lag.RecordSnapshot(snap);

// When processing a shot
HistorySnapshot rewound;
if (lag.RewindToTime(clientShootTimestamp, rewound))
{
    // Perform hit detection against rewound.entities
}
```

## Transport Layer

The `ITransport` interface decouples `NetworkManager` from any specific socket implementation:

```cpp
class ITransport
{
public:
    virtual bool Initialize(uint16_t port) = 0;
    virtual void Shutdown() = 0;
    virtual bool Send(const uint8_t* data, size_t size,
                      const std::string& address, uint16_t port) = 0;
    virtual int Receive(uint8_t* buffer, size_t bufferSize,
                        std::string& fromAddress, uint16_t& fromPort) = 0;
    virtual bool IsReady() const = 0;
    virtual std::string GetTransportName() const = 0;
};
```

### UDPTransport (Default)

The default transport uses platform BSD/Winsock UDP sockets:

- Creates a **non-blocking** UDP socket
- Enlarges OS send/receive buffers to **64 KB** each for game traffic
- Supports binding to a specific port or ephemeral port (port 0)
- Cross-platform: Winsock on Windows, POSIX sockets on Linux/macOS

### SteamTransport (Stub)

A placeholder for future Steam Networking Sockets integration. Currently all methods return failure. When the Steamworks SDK is linked, this will use `ISteamNetworkingSockets` for relay-based, NAT-traversing packet I/O.

## NetworkStack -- Integrated Transport + Security

The `NetworkStack` class combines transport selection with the security layer:

```cpp
struct NetworkStackConfig
{
    enum class TransportType { UDP, Steam };

    TransportType transport = TransportType::UDP;
    std::string serverAddress = "127.0.0.1";
    uint16_t serverPort = 27015;
    bool enableEncryption = true;
    std::string encryptionKey = "SparkEngine_DefaultKey_ChangeMe!";
    uint32_t tokenLifetimeSeconds = 300;
};
```

```cpp
NetworkStack stack;
NetworkStackConfig config;
config.transport = NetworkStackConfig::TransportType::UDP;
config.enableEncryption = true;
stack.Initialize(config);

// Encrypt outgoing data
auto encrypted = stack.Encrypt(rawPayload);

// Decrypt incoming data
auto decrypted = stack.Decrypt(encryptedPayload);

// Generate/validate connection tokens
auto token = stack.GenerateConnectionToken(clientId);
bool valid = stack.ValidateToken(token);

// Access underlying layers
ITransport* transport = stack.GetTransport();
NetworkSecurity* security = stack.GetSecurity();
```

## Security Layer

### NetworkSecurity

Provides XOR-based packet encryption and single-use connection tokens:

```cpp
static constexpr size_t SECURITY_KEY_SIZE = 32;       // 256-bit keys
static constexpr size_t CONNECTION_TOKEN_SIZE = 16;    // 128-bit tokens
static constexpr float CONNECTION_TOKEN_LIFETIME = 30.0f; // 30 second expiry
```

| Method | Description |
|--------|-------------|
| `PacketEncrypt(data, size, key)` | XOR encrypt in-place |
| `PacketDecrypt(data, size, key)` | XOR decrypt in-place (symmetric) |
| `Encrypt(plaintext, key)` | Return encrypted copy |
| `Decrypt(ciphertext, key)` | Return decrypted copy |
| `GenerateConnectionToken()` | Generate random 128-bit single-use token |
| `ValidateConnectionToken(token)` | Validate and consume a token |
| `GenerateKey(outKey)` | Generate random 256-bit key |
| `SetEncryptionEnabled(bool)` | Enable/disable encryption path |

> **Warning:** XOR encryption is a placeholder. Production games should replace it with DTLS or AES-GCM.

### NetworkEncryption (Advanced)

Adds per-connection session keys with replay protection and rate limiting:

```cpp
constexpr size_t SESSION_KEY_SIZE = 32;        // 256-bit session key
constexpr size_t NONCE_SIZE = 8;               // 64-bit sequence-based nonce
constexpr size_t HMAC_SIZE = 4;                // 32-bit truncated integrity tag
constexpr size_t TOKEN_SIZE = 16;              // 128-bit connection token
constexpr size_t ENCRYPTION_OVERHEAD = 12;     // NONCE_SIZE + HMAC_SIZE per packet
```

**Packet layout:** `[nonce (8B)] [encrypted payload] [hmac (4B)]`

| Function | Description |
|----------|-------------|
| `GenerateSessionKey()` | Create random 256-bit session key |
| `GenerateConnectionToken()` | Create random 128-bit token |
| `EncryptPacket(key, sequence, payload)` | Encrypt with nonce + HMAC |
| `DecryptPacket(key, packet, outPayload, outSeq)` | Decrypt and verify integrity |
| `ValidateToken(expected, received)` | Constant-time token comparison |

### RateLimiter

```cpp
class RateLimiter
{
public:
    explicit RateLimiter(uint32_t maxPacketsPerSecond = 100,
                         uint32_t burstAllowance = 20);
    bool AllowPacket(uint64_t addressHash);
    void ResetClient(uint64_t addressHash);
    void Clear();
    uint32_t GetPacketCount(uint64_t addressHash) const;
};
```

Uses a sliding window approach per source IP:port hash. Rejects packets exceeding the configured rate.

### ReplayProtection

```cpp
class ReplayProtection
{
public:
    static constexpr size_t WINDOW_SIZE = 256;
    bool Accept(uint64_t sequence);  // Returns false for replayed sequences
    void Reset();
};
```

Tracks received sequence numbers in a 256-entry sliding window. Rejects packets with previously seen or too-old sequence numbers.

## Dedicated Server

The `DedicatedServer` class provides a complete headless server:

### ServerConfig

```cpp
struct ServerConfig
{
    // Identity
    std::string serverName = "Spark Dedicated Server";
    std::string motd;                          // Message of the day

    // Network
    uint16_t port = DEFAULT_PORT;              // 27015
    int maxClients = 32;
    float tickRate = 60.0f;                    // Ticks per second
    float clientTimeoutSeconds = 30.0f;
    float heartbeatIntervalSeconds = 1.0f;
    bool lanOnly = false;

    // Game
    GameModeType gameMode = GameModeType::Deathmatch;
    std::string customGameModeName;
    int scoreLimit = 50;
    float timeLimitMinutes = 15.0f;
    int roundCount = 1;
    bool friendlyFire = false;
    bool autoBalanceTeams = true;

    // Map rotation
    std::vector<std::string> mapRotation;
    bool randomizeMapOrder = false;

    // Administration
    std::string rconPassword;                  // Reserved; currently ignored
    uint16_t rconPort = 0;                     // Reserved; currently ignored
    bool enableLogging = true;
    std::string logFilePath = "server.log";

    // LAN discovery
    bool enableLanBroadcast = true;
    uint16_t lanBroadcastPort = 27016;

    // Performance
    bool enableAntiCheat = false;
    float replicationRate = 20.0f;             // Entity replication Hz
    int snapshotHistorySize = 64;              // Lag compensation snapshots
};
```

### GameModeType

| Value | Description |
|-------|-------------|
| `Deathmatch` | Free-for-all deathmatch |
| `TeamDeathmatch` | Team-based deathmatch |
| `CaptureTheFlag` | Capture the flag mode |
| `Domination` | Zone control mode |
| `SearchAndDestroy` | Attack/defend objectives |
| `FreeForAll` | Free-for-all variant |
| `Custom` | Custom game mode (use `customGameModeName`) |

### Server Lifecycle

```cpp
DedicatedServer server;

ServerConfig config;
config.serverName = "My FPS Server";
config.port = 27015;
config.maxClients = 16;
config.tickRate = 60.0f;
config.gameMode = GameModeType::TeamDeathmatch;
config.mapRotation = {"dm_warehouse", "dm_canyon", "dm_rooftops"};

// Start with background tick loop
server.Start(config);

// Or: initialize + drive externally
server.InitializeOnly(config);
while (running) { server.Tick(deltaTime); }

// Graceful shutdown
server.Stop();
```

### Server Callbacks

```cpp
struct ServerCallbacks
{
    std::function<void()> onServerStarted;
    std::function<void()> onServerStopped;
    std::function<void(ClientID, const std::string&)> onClientConnected;
    std::function<void(ClientID, const std::string&)> onClientDisconnected;
    std::function<void(const std::string&)> onMapChanged;
    std::function<void(const std::string&)> onChatMessage;
    std::function<void(const std::string&, const std::string&)> onRconCommand;
    std::function<void(const std::string&)> onLogMessage;
};
```

### Local Administration Commands (legacy RCON API names)

There is currently no remote RCON listener. `ExecuteRcon` is for trusted
in-process host/control code only; network chat never dispatches admin commands,
and the compatibility fields `rconPassword`/`rconPort` are inactive.

```cpp
// Register custom local administration commands
server.RegisterRconCommand("restart", "Restart the current match",
    [&](const std::vector<std::string>& args) -> std::string {
        server.EndMatch();
        server.StartMatch();
        return "Match restarted.";
    });

// Dispatch from trusted host code
std::string response = server.ExecuteRcon("kick 3 cheating");
```

Built-in commands are registered automatically: `help`, `status`, `kick`, `ban`, `map`, `say`, `players`, `endmatch`, and `nextmap`. There is no `quit` command; the owning control thread must call `Stop()`.

### LAN Discovery

```cpp
// Server side: broadcast presence
server.StartLanBroadcast();  // Broadcasts every 3 seconds on port 27016

// Client side: discover servers
auto servers = DedicatedServer::DiscoverLanServers(27016, 2000);
for (const auto& info : servers)
{
    // info.serverName, info.mapName, info.currentPlayers, info.maxPlayers, info.ping
}
```

### ServerStats

```cpp
struct ServerStats
{
    float uptimeSeconds = 0.0f;
    uint64_t totalTicksProcessed = 0;
    float averageTickMs = 0.0f;
    float peakTickMs = 0.0f;
    uint32_t currentPlayers = 0;
    uint32_t peakPlayers = 0;
    uint64_t totalBytesIn = 0;
    uint64_t totalBytesOut = 0;
    uint32_t totalConnectionsServed = 0;
    float currentTickRate = 0.0f;
    std::string currentMap;
    int currentMapIndex = 0;
    float matchTimeRemaining = 0.0f;
    int currentRound = 1;
};
```

## NetworkManager API Reference

### Singleton Access

```cpp
static NetworkManager& GetInstance();
```

### Lifecycle

| Method | Description |
|--------|-------------|
| `Initialize()` | Initialize platform sockets. Must be called first. |
| `Shutdown()` | Release all resources. |
| `StartServer(port, maxClients)` | Listen on UDP port (default 27015, max 32 clients) |
| `StopServer()` | Stop server, disconnect all clients |
| `Connect(address, port, name)` | Connect to a server as client |
| `Disconnect()` | Disconnect from server or shut down |
| `Update(deltaTime)` | Process incoming/outgoing messages (call each frame) |

### Sending Messages

| Method | Description |
|--------|-------------|
| `SendMessage(msg)` | Send to connected server (client) or broadcast (server) |
| `SendToClient(id, msg)` | Send to specific client (server only) |
| `SendToAll(msg)` | Broadcast to all connected clients |
| `SendToAllExcept(id, msg)` | Broadcast excluding one client |
| `BroadcastMessage(msg)` | Alias for `SendToAll` |

### Message Handling

```cpp
using MessageHandler = std::function<void(const NetworkMessage&)>;
void RegisterHandler(MessageType type, MessageHandler handler);
```

### State Queries

| Method | Return Type | Description |
|--------|-------------|-------------|
| `GetRole()` | `NetworkRole` | Server, Client, or None |
| `GetConnectionState()` | `ConnectionState` | Current connection state |
| `GetLocalClientID()` | `ClientID` | Local client's assigned ID |
| `GetServerTime()` | `float` | Server clock time |
| `GetStats()` | `const NetworkStats&` | Bandwidth/latency stats |
| `IsInitialized()` | `bool` | Whether Initialize() succeeded |
| `GetClients()` | `const map<ClientID, ClientInfo>&` | Connected clients (server) |

### Network Statistics

```cpp
struct NetworkStats
{
    float ping = 0.0f;            // Round-trip time (ms)
    float jitter = 0.0f;          // Ping variance (ms)
    float packetLoss = 0.0f;      // 0.0 to 1.0
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    uint32_t packetsSent = 0;
    uint32_t packetsReceived = 0;
    uint32_t packetsDropped = 0;
    float bandwidthUp = 0.0f;     // KB/s
    float bandwidthDown = 0.0f;   // KB/s
};
```

### ClientInfo

```cpp
struct ClientInfo
{
    ClientID id = INVALID_CLIENT;
    std::string name;
    ConnectionState state = ConnectionState::Disconnected;
    NetworkStats stats;
    float lastHeartbeatTime = 0.0f;
    uint32_t playerEntityNetworkID = 0;
};
```

## Internal Implementation Details

### Connection Handshake

1. Client sends `MessageType::Connect` with player name
2. Server validates (max clients, bans), assigns `ClientID`
3. Server sends `ConnectAccepted` with assigned ID, or `ConnectRejected` with reason
4. Server calls `SendFullEntitySync` to replicate existing entities to new client

### Heartbeat System

- Default interval: **1 second** (`m_heartbeatInterval = 1.0f`)
- Connection timeout: **10 seconds** (`m_connectionTimeout = 10.0f`)
- `UpdateHeartbeat()` is called each frame; timed-out clients are disconnected

### Reliable Message Delivery

The reliable channel provides guaranteed delivery with duplicate detection and ordered delivery:

- **ACK tracking**: Receiver tracks the highest received sequence number and a 32-bit bitfield encoding the previous 32 sequences. ACKs are sent at ~30 Hz.
- **Retransmission**: Unacknowledged messages are retransmitted with exponential backoff (base interval doubles each retry, capped at 8x). Configurable via `SetMaxReliableRetries()` (default: 10).
- **Duplicate detection**: Receiver maintains a set of recently received sequence numbers (pruned after 30 seconds). Duplicate packets are silently dropped.
- **Ordered delivery**: `ReliableOrdered` messages are buffered and delivered in sequence order. Out-of-order packets are held until the gap is filled.
- **RTT estimation**: Jacobson/Karels algorithm (RFC 6298) computes smoothed RTT and variance. Karn's algorithm skips retransmitted packets for RTT samples.
- **Connection failure**: After `m_maxReliableRetries` retransmissions, the message is dropped and `packetsDropped` is incremented.

### Server-Side Hit Validation (Lag Compensation)

The `ValidateHit()` method integrates lag compensation with hit detection:

1. Server receives a hitscan request from a client with the client's timestamp
2. Rewinds entity positions to `clientTimestamp - halfRTT` using `LagCompensator::RewindToTime()`
3. Interpolates hitbox positions between bracketing snapshots for sub-frame accuracy
4. Performs a ray-AABB intersection test against rewound hitboxes
5. Returns `HitValidationResult` with hit status, entity ID, and hit point

This ensures clients see fair hit registration despite network latency.

### Bandwidth Tracking

- Samples are taken via `std::chrono::steady_clock`
- `m_bytesSentSinceSample` and `m_bytesReceivedSinceSample` accumulate between samples
- Results are stored in `m_stats.bandwidthUp` and `m_stats.bandwidthDown` (KB/s)

## Thread Safety

| Component | Thread Safety | Details |
|-----------|--------------|---------|
| `NetworkManager` | Queue mutex | `m_queueMutex` protects `m_incomingQueue` and `m_outgoingQueue`; `m_handlerMutex` protects handler registration |
| `DedicatedServer` | Internal mutexes | Local admin registry (`m_rconMutex`), bans (`m_banMutex`), logging (`m_logMutex`). Tick loop runs on `m_tickThread`. |
| `UDPTransport` | Not thread-safe | Socket operations should be called from the network thread only |
| `NetworkSecurity` | Not thread-safe | Token map is not mutex-protected; call from single thread |
| `ClientPrediction` | Not thread-safe | Call from main game thread only |
| `RateLimiter` | Not thread-safe | Access from network thread only |

## Error Handling

- `Initialize()`, `StartServer()`, `Connect()` return `bool` -- false on failure
- Socket creation failures log errors and return false
- Invalid message deserialization sets `NetBuffer::m_error` flag
- `CanRead()` prevents buffer overruns during deserialization
- Connection token validation is constant-time to prevent timing attacks
- Rate limiter rejects packets exceeding the per-client threshold silently

## Performance Considerations

| Parameter | Default | Description |
|-----------|---------|-------------|
| Replication rate | 20 Hz | `m_replicationInterval = 0.05f` |
| Heartbeat interval | 1.0s | `m_heartbeatInterval` |
| Connection timeout | 10.0s | `m_connectionTimeout` |
| Reliable retransmit | 0.5s | `m_reliableRetransmitInterval` |
| Max reliable retries | 10 | `m_maxReliableRetries` |
| Max clients | 32 | `m_maxClients` |
| Lag history | 1.0s | `m_maxHistoryDuration` |
| Socket buffer | 64 KB | Send and receive buffers |
| Rate limit | 100 pkt/s | `RateLimiter` default |
| Burst allowance | 20 pkt | Extra packets in short bursts |
| Replay window | 256 | `ReplayProtection::WINDOW_SIZE` |
| Max pending inputs | 128 | `ClientPrediction::m_maxPendingInputs` |
| Server tick rate | 60 Hz | `ServerConfig::tickRate` |

## Console Commands

```
net_status           # Show NetworkManager connection state and role
net_clients          # List connected clients with stats (server only)
net_stats            # Show bandwidth, ping, jitter, packet loss
net_stack_status     # Show NetworkStack transport and encryption status
prediction_status    # Show prediction pending count and correction magnitude
server_status        # Show DedicatedServer uptime, players, map, match state
```

## Troubleshooting

| Symptom | Possible Cause | Solution |
|---------|---------------|----------|
| `Initialize()` returns false | `ENABLE_NETWORKING=OFF` | Rebuild with `-DENABLE_NETWORKING=ON` |
| Connection timeout | Firewall blocking UDP 27015 | Open port in firewall; check `lanOnly` flag |
| High packet loss | Network congestion or buffer overflow | Increase socket buffer size; reduce replication rate |
| Rubber-banding | Large prediction corrections | Tune `SetSmoothCorrection` speed; reduce server tick interval |
| Stale entity state | Property not marked dirty | Call `MarkPropertyDirty()` after modifying replicated properties |
| Token validation fails | Token expired (30s lifetime) | Ensure client connects within token lifetime |
| HMAC mismatch | Key mismatch between client/server | Verify both sides use the same session key |
| `SendMessage` compile error on Windows | Windows macro conflict | The header `#undef SendMessage` handles this automatically |

## Stub Behavior (ENABLE_NETWORKING=OFF)

When networking is disabled, `NetworkManagerStub` is provided:

```cpp
class NetworkManagerStub
{
public:
    static NetworkManagerStub& GetInstance();
    bool Initialize() { return false; }
    void Shutdown() {}
    bool StartServer(...) { return false; }
    void StopServer() {}
    bool Connect(...) { return false; }
    void Disconnect() {}
    void Update(float) {}
    NetworkRole GetRole() const { return NetworkRole::None; }
    ConnectionState GetConnectionState() const { return ConnectionState::Disconnected; }
    bool IsInitialized() const { return false; }
};
```

All calls are no-ops. `GetRole()` always returns `None`. This allows game code to compile and run in single-player mode without `#ifdef` guards everywhere.

---

## See Also

- [Entity Component System](Entity-Component-System.md) -- NetworkIdentity component
- [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md) -- Multiplayer game modes
- [Event System](Event-System.md) -- Network event handling
- [Scene Management](Scene-Management.md) -- Networked scene transitions
- [Physics](Physics.md) -- Server-authoritative physics
- [Animation](Animation.md) -- Replicated animation states
- [Input System](Input-System.md) -- Client input processing and prediction
