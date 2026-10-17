# Networking Wire Format

This page documents the binary wire format used by SparkEngine's UDP networking layer for all client-server communication.

**Source:** `SparkEngine/Source/Engine/Networking/NetworkManager.h`, `EntityReplicator.h`, `ReplicationFields.h`

> **Note:** Networking requires `ENABLE_NETWORKING=ON` during CMake configuration.

---

## Packet Structure

All packets use **little-endian** byte order. Every packet begins with the same 23-byte header;
a `ReliableOrdered` packet (channel 2) carries a 4-byte ordered sequence after it (protocol v3):

```
Offset  Size  Type       Field
──────  ────  ─────────  ──────────────────────────
0       4     uint32     Magic (0x5350524B = "SPRK")
4       2     uint16     MessageType
6       1     uint8      ChannelType
7       4     uint32     SenderID (ClientID)
11      4     uint32     SequenceNumber (reliability/ACK)
15      4     float32    Timestamp (server time)
19      4     uint32     PayloadLength (N)
23      4     uint32     OrderedSequence   -- ReliableOrdered only
23|27   N     bytes      Payload
```

- **Minimum packet size:** 23 bytes (empty payload; 27 for ReliableOrdered)
- **Maximum payload size:** 65,453 bytes on every channel (`MAX_NETWORK_MESSAGE_PAYLOAD_SIZE`)
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

1. Sender assigns a `SequenceNumber` to each reliable message (Reliable and ReliableOrdered share this
   reliability stream). A ReliableOrdered message also takes an `OrderedSequence` from a separate
   per-peer stream that starts at 1; only that one drives in-order delivery, so Reliable traffic can
   never leave a gap in the ordered stream (the protocol-v2 bug that stalled every ordered message sent
   after a Reliable one)
2. Receiver sends `Ack` messages containing the highest received sequence plus a 32-bit bitfield for the previous 32 sequences
3. Sender retransmits unacknowledged messages after a configurable timeout

---

## Connection Handshake

```
Client                          Server
  │                               │
  │──── Connect ─────────────────>│  plaintext frame
  │     (ClientHello)             │
  │                               │
  │<─── ConnectAccepted ──────────│  plaintext frame
  │  (clientID, time, ver,        │
  │   signed ServerHello)         │
  │                               │
  │═══► ClientFinished (name) ═══>│  sealed from here on
  │                               │
  │<─── GameStateSync ────────────│
  │     (full world state)        │
  │                               │
  │──── Heartbeat ───────────────>│
  │<─── Heartbeat ────────────────│
  │     (ongoing keepalive)       │
```

Protocol version 2 (NET-100) frames every datagram: a first byte of `0x01` marks a plaintext
handshake frame (only `Connect`, `ConnectAccepted`, `ConnectRejected`), and `0x02` marks a sealed
frame (a ChaCha20-Poly1305 `SecureChannel` packet around the message). The `Connect` payload is
exactly the 55-byte ClientHello: handshake magic `0x484E5053` ("SPNH"), the `uint16`
`NETWORK_PROTOCOL_VERSION` (currently `2`), the suite byte, an X25519 ephemeral key and a nonce;
the player name no longer travels in `Connect`. Before it considers a client slot, the server
rejects a missing (`ProtocolMissing`), older (`ProtocolTooOld`), newer (`ProtocolTooNew`),
malformed (`MalformedHandshake`) or wrong-suite (`UnsupportedSuite`) handshake, and it rejects with
`ServerFull` when every slot is taken; a small-order ephemeral key is `MalformedHandshake` found by
comparison, before any curve or signature work. `ConnectAccepted` echoes the version and carries the
server's signed ServerHello; the signature (transcript label `SPNH-v3`) also covers the 10-byte
accept prefix, so the assigned client id cannot be rewritten in flight. The client checks the server
key against its pin or known_hosts (`ServerIdentityMismatch`) and the signature
(`HandshakeAuthFailed`), then sends the player name in a sealed `ClientFinished`, which is what
admits it. Until then the slot is `Securing`: it is not listed by `GetClients()`, and `SendToClient`
refuses it everything but the handshake answer. Unadmitted `Connect`s are budgeted per source IPv4
address (`ConnectRateLimiter`; excess is dropped unanswered). A version-1 client's unframed `Connect` gets
an unframed typed `ProtocolTooOld`. The exact layouts are in `docs/specs/networking-wire-format.md`;
the evidence is the CTests `NetworkSessionCompatibility` (`Tests/TestSessionCompatibilityReal.cpp`)
and `NetworkSecureTransportWired` (`Tests/TestSecureTransportWired.cpp`).

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

The active UDP path binds to loopback or one canonical RFC1918 interface/prefix and admits only concrete peers in the captured subnet. Missing/invalid prefixes, exact network/directed-broadcast addresses, wildcard/public/test/multicast/limited-broadcast/CGNAT values, mapped IPv6, and alternate textual encodings fail closed. Peer scope is checked before packet deserialization and again on all gameplay send/retry paths. Client traffic is bound to the configured server address and port, and server-side client identity is bound to the endpoint recorded during `Connect`. Wire-supplied sender IDs are not trusted. Undefined channels, malformed built-in payload sizes, and unauthenticated custom messages are rejected before dispatch.

Since protocol version 2 (NET-100) every datagram after the handshake is sealed with libsodium's ChaCha20-Poly1305 in a `SecureChannel` keyed by the signed-ephemeral X25519 handshake ([Networking](../subsystems/Networking.md#securehandshake-key-agreement-in-connect)); OD-06: libsodium supplies every primitive. The client authenticates the server's Ed25519 identity (a pinned key, or trust on first use recorded in `known_hosts`), and each sealed frame is authenticated, replay-checked in a 256-packet window, and opened only with the channel of the peer its endpoint belongs to. A plaintext gameplay frame, a frame sealed under another peer's key, and any tampered, replayed, truncated or out-of-epoch frame are dropped and counted (`NetworkStats::securityDrops`, `plaintextFramesDropped`). There is no plaintext mode, and a message marked `sensitive` is refused unless a channel to its destination exists. Send keys rotate every 600 s or 2^31 packets.

The transport does not authenticate players: account authentication runs inside the channel (TERRAFRONT uses SCRAM, so no password crosses the wire). SparkGateway admission tickets are not yet bound to the UDP session; the decided design presents the ticket inside the sealed `ClientFinished`. The implementation has not had an independent cryptographic review, and anti-amplification cookies are not implemented (a `ConnectAccepted` is about 2.3 times its `Connect`; the per-source Connect budget bounds how often one address can trigger it), so NET-100 stays open until both land.

Planned security work includes:

- Gateway ticket presented in `ClientFinished` and checked by the area server
- Anti-amplification cookies for `Connect`
- Independent review of the handshake and channel composition

See [Networking](../subsystems/Networking.md) for the full networking architecture overview.
