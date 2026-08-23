# Networking Wire Format

This page documents the binary wire format used by SparkEngine's UDP networking layer for all client-server communication.

**Source:** `SparkEngine/Source/Engine/Networking/NetworkManager.h`, `EntityReplicator.h`, `ReplicationFields.h`

> **Note:** Networking requires `ENABLE_NETWORKING=ON` during CMake configuration.

---

## Packet Structure

All packets use **little-endian** byte order. Every packet begins with the same 23-byte header:

```
Offset  Size  Type       Field
──────  ────  ─────────  ──────────────────────────
0       4     uint32     Magic (0x5350524B = "SPRK")
4       2     uint16     MessageType
6       1     uint8      ChannelType
7       4     uint32     SenderID (ClientID)
11      4     uint32     SequenceNumber
15      4     float32    Timestamp (server time)
19      4     uint32     PayloadLength (N)
23      N     bytes      Payload
```

- **Minimum packet size:** 23 bytes (empty payload)
- **Maximum payload size:** 64,512 bytes (~63 KB)
- **Magic number:** `0x5350524B` — ASCII `"SPRK"`. Packets with incorrect magic are silently dropped.

---

## Message Types

```cpp
enum class MessageType : uint16_t
{
    // Connection lifecycle
    Connect          = 1,    // Client → Server: request to join
    ConnectAccepted  = 2,    // Server → Client: connection approved
    ConnectRejected  = 3,    // Server → Client: connection denied
    Disconnect       = 4,    // Either direction: graceful close
    Heartbeat        = 5,    // Either direction: keepalive

    // Reliability layer
    Ack              = 6,    // Acknowledges reliable messages (sequence + bitfield)

    // Entity replication
    EntitySpawn       = 7,   // Server → Client: new entity
    EntityDestroy     = 8,   // Server → Client: entity removed
    EntityStateUpdate = 9,   // Server → Client: delta state
    EntityRPC         = 10,  // Either direction: remote procedure call

    // Input
    ClientInput      = 11,   // Client → Server: input state
    InputAck         = 12,   // Server → Client: input acknowledged

    // Game events
    ChatMessage      = 13,
    GameStateSync    = 14,
    MatchStart       = 15,
    MatchEnd         = 16,
    PlayerRespawn    = 17,
    ScoreUpdate      = 18,

    // Extension point
    UserDefined      = 1000  // Game-specific messages start here
};
```

---

## Channel Types

Each message specifies a delivery guarantee:

| Value | Channel | Behavior |
|-------|---------|----------|
| 0 | `Unreliable` | Fire-and-forget. Used for position updates, movement. No retransmission. |
| 1 | `Reliable` | Guaranteed delivery with acknowledgment. Retransmitted until acked. |
| 2 | `ReliableOrdered` | Guaranteed delivery in send order. Messages queued until predecessors arrive. |

---

## Entity Replication Protocol

Entity state is replicated using a field-level dirty bitmask system. Each entity can have up to **64 replicated fields** (one bit per field in a `uint64_t` mask).

### Full State Packet (EntitySpawn)

Sent when an entity first enters a client's relevance set:

```
Offset  Size  Type       Field
──────  ────  ─────────  ──────────────────────────
0       4     uint32     EntityID (network ID)
1       1     uint8      FieldCount
5       8     uint64     AllFieldsMask (visibility)
13      var   bytes      Field0 data
...     var   bytes      FieldN data
```

### Delta Update Packet (EntityStateUpdate)

Sent each tick for entities with changed fields:

```
Offset  Size  Type       Field
──────  ────  ─────────  ──────────────────────────
0       4     uint32     EntityID
4       8     uint64     DirtyMask (changed + visible fields only)
12      var   bytes      Dirty field data (in bit order)
```

Only fields whose corresponding bit is set in `DirtyMask` are serialized. Fields are written in ascending bit order.

### Field Visibility

Each replicated field has a visibility level controlling which clients receive it:

| Level | Description |
|-------|-------------|
| `Public` | All connected clients |
| `Private` | Owner client only (e.g., ammo count, inventory) |
| `Party` | Owner's party/squad members |
| `Spectator` | Spectator clients only |

### Replicated Field Types

Fields must be trivially copyable. Supported types:

| Type | Wire Size |
|------|-----------|
| `bool` | 1 byte |
| `int32_t` | 4 bytes |
| `uint32_t` | 4 bytes |
| `float` | 4 bytes |
| `XMFLOAT3` | 12 bytes |

---

## Client Input Packet

Sent from client to server every frame:

```
Offset  Size  Type       Field
──────  ────  ─────────  ──────────────────────────
0       4     uint32     InputSequence
4       4     float32    MoveForward [-1, 1]
8       4     float32    MoveRight [-1, 1]
12      4     float32    LookYaw (degrees)
16      4     float32    LookPitch (degrees)
20      1     uint8      ButtonFlags (jump|fire|reload|sprint|crouch)
21      4     float32    DeltaTime (client frame dt)
25      4     float32    Timestamp (client-local time)
```

The `InputSequence` is a monotonically increasing counter used for server reconciliation during client-side prediction.

---

## Lag Compensation

The server maintains a rolling history of entity snapshots for hit verification:

```cpp
struct HistorySnapshot
{
    float timestamp;
    struct EntityState
    {
        uint32_t networkID;
        XMFLOAT3 position;
        XMFLOAT3 rotation;
        XMFLOAT3 boundsMin;  // AABB for hitbox rewind
        XMFLOAT3 boundsMax;
    };
    std::vector<EntityState> entities;
};
```

When a client reports a hit, the server rewinds entity positions to the client's timestamp and verifies the shot against historical AABBs.

---

## Acknowledgment and Reliability

Reliable messages use a sliding-window acknowledgment scheme:

1. Sender assigns a `SequenceNumber` to each reliable message
2. Receiver sends `Ack` messages containing the highest received sequence plus a 32-bit bitfield for the previous 32 sequences
3. Sender retransmits unacknowledged messages after a configurable timeout

---

## Connection Handshake

```
Client                          Server
  │                               │
  │──── Connect ─────────────────>│
  │     (token, version)          │
  │                               │
  │<─── ConnectAccepted ──────────│
  │     (clientID, serverTime)    │
  │                               │
  │<─── GameStateSync ────────────│
  │     (full world state)        │
  │                               │
  │──── Heartbeat ───────────────>│
  │<─── Heartbeat ────────────────│
  │     (ongoing keepalive)       │
```

If the server rejects the connection (version mismatch, server full, banned), it sends `ConnectRejected` with a reason string in the payload.

---

## Transport Layer

The wire format is transport-agnostic. Two transports are available:

| Transport | Status | Description |
|-----------|--------|-------------|
| `UDPTransport` | Active | Raw BSD/Winsock UDP sockets |
| `SteamTransport` | Stub | Steamworks P2P relay (not yet implemented) |

Transports implement the `ITransport` interface and are selected via `TransportType` enum at initialization.

---

## Security

The active UDP path binds client traffic to the configured server address and port, and binds server-side client identity to the endpoint recorded during `Connect`. Wire-supplied sender IDs are not trusted. Undefined channels, malformed built-in payload sizes, and unauthenticated custom messages are rejected before dispatch.

This tuple binding is a spoofing defense, not cryptographic authentication; transparent endpoint migration is unsupported and requires reconnecting. The experimental `NetworkSecurity` helpers are not integrated strongly enough to make confidentiality or authenticated-encryption claims for production traffic.

Planned security work includes:

- Token-based authentication
- Rate limiting (packets per second per client)
- Authenticated encryption and session key rotation

See [Networking](../subsystems/Networking.md) for the full networking architecture overview.
