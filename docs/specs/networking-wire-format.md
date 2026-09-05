# SparkEngine Networking Wire Format Specification

**Version:** 1.0  
**Date:** 2026-04-01  
**Status:** Reference  

## Overview

SparkEngine uses a custom UDP-based networking protocol for multiplayer games. The system supports client/server architecture with entity state replication, client-side prediction, server reconciliation, and lag compensation.

All networking code is guarded by `ENABLE_NETWORKING` (ON by default). When disabled, a stub NetworkManager compiles without linker errors.

## Transport Layer

- **Protocol:** UDP (User Datagram Protocol)
- **Default Port:** 27015
- **Byte Order:** Little-endian (matches x86/x64 native order)
- **Max Packet Size:** MTU-safe (< 1400 bytes recommended)
- **Platforms:** Winsock2 (Windows), POSIX sockets (Linux/macOS)

## Core Types

| Type | Width | Description |
|------|-------|-------------|
| `ClientID` | `uint32_t` | Unique client identifier. `0` = `INVALID_CLIENT` |
| `SequenceNumber` | `uint32_t` | Monotonic counter for reliable ordering |
| `NetworkTime` | `float` | Server time in seconds |

## Packet Structure

Each UDP packet contains one `NetworkMessage`:

```
+------------------+-------------------+--------------------+
| Header (fixed)   | Metadata          | Payload (variable) |
+------------------+-------------------+--------------------+
```

### Header Fields

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 4 | `uint32_t` | `magic` | `0x5350524B` (`SPRK`) |
| 4 | 2 | `uint16_t` | `type` | Message type enum value |
| 6 | 1 | `uint8_t` | `channel` | Channel type (0=Unreliable, 1=Reliable, 2=ReliableOrdered) |
| 7 | 4 | `uint32_t` | `senderID` | Originating client ID |
| 11 | 4 | `uint32_t` | `sequence` | Sequence number for reliable ordering |
| 15 | 4 | `float` | `timestamp` | Server time when created |
| 19 | 4 | `uint32_t` | `payloadSize` | Length of payload in bytes |
| 23 | N | `uint8_t[]` | `payload` | Raw serialized message body |

**Total header size:** 23 bytes (fixed) + variable payload

Sensitive-payload ownership is intentionally absent from the wire format.
Senders mark their local `NetworkMessage` copies with `sensitive`; receivers
independently classify sensitive message types with `RegisterSensitiveHandler`.
This promptly erases queue, retransmit, dispatch, and serialization buffers
without changing the version-1 datagram or trusting sender-controlled metadata.
All channel bytes other than 0, 1, and 2 (including values with high bits set)
are rejected before dispatch.

## Channel Types

### Unreliable (0)
- Fire-and-forget delivery
- No ordering guarantees
- No retransmission
- Use for: position updates, movement, frequent state updates

### Reliable (1)
- Guaranteed delivery via ACK/retransmission
- May arrive out of order
- Retransmits unacknowledged messages
- Use for: chat messages, state changes, score updates

### ReliableOrdered (2)
- Guaranteed delivery AND in-order processing
- Messages buffered until gaps are filled
- Highest overhead
- Use for: important game events, match state transitions

## Message Types

### Connection Messages

| Type | Value | Direction | Payload | Description |
|------|-------|-----------|---------|-------------|
| `Connect` | 1 | C→S | Client info (version, name) | Connection request |
| `ConnectAccepted` | 2 | S→C | Assigned ClientID, server time | Connection accepted |
| `ConnectRejected` | 3 | S→C | Rejection reason (string) | Connection denied |
| `Disconnect` | 4 | Both | None | Clean disconnect |
| `Heartbeat` | 5 | Both | None | Keep-alive ping |

### Reliability Messages

| Type | Value | Direction | Payload | Description |
|------|-------|-----------|---------|-------------|
| `Ack` | 6 | Both | Sequence number + ACK bitfield | Acknowledges reliable messages |

### Entity Replication Messages

| Type | Value | Direction | Payload | Description |
|------|-------|-----------|---------|-------------|
| `EntitySpawn` | 7 | S→C | Network ID, entity type, initial state | New entity created |
| `EntityDestroy` | 8 | S→C | Network ID | Entity removed |
| `EntityStateUpdate` | 9 | S→C | Network ID, delta properties | Entity state changed |
| `EntityRPC` | 10 | Both | Network ID, RPC name, args | Remote procedure call |

### Input Messages

| Type | Value | Direction | Payload | Description |
|------|-------|-----------|---------|-------------|
| `ClientInput` | 11 | C→S | Input sequence, movement, actions | Player input state |
| `InputAck` | 12 | S→C | Last processed input sequence | Server confirms input |

### Game Messages

| Type | Value | Direction | Payload | Description |
|------|-------|-----------|---------|-------------|
| `ChatMessage` | 13 | Both | Sender name, text | Chat message |
| `GameStateSync` | 14 | S→C | Full game state snapshot | Periodic full sync |
| `MatchStart` | 15 | S→C | Match config | Match begins |
| `MatchEnd` | 16 | S→C | Results | Match ends |
| `PlayerRespawn` | 17 | S→C | Spawn position, health | Player respawns |
| `ScoreUpdate` | 18 | S→C | Player scores | Score change |
| `UserDefined` | 1000+ | Both | Custom | Game-specific messages |

A module whose payload prepends its own header (a channel byte, a sub-type tag) must claim a type in
the `UserDefined` range and register a schema for **that** type with the matching
`stringFieldOffset`. Re-registering a built-in type's schema replaces a process-wide entry that is
never restored on module shutdown, so every other producer and consumer of that type — the engine
included, after the module unloads — would keep validating against the module's layout.
`GameModules/SparkGameMMO` does this correctly: `UserDefined + 1` with `stringFieldOffset = 1`.

## Handshake Sequence

```
Client                          Server
  |                               |
  |--- Connect (version, name) -->|
  |                               | Validate version, check slots
  |<-- ConnectAccepted (ID, time)-|  (or ConnectRejected)
  |                               |
  |--- Heartbeat ---------------->|  Periodic keep-alive
  |<-- Heartbeat -----------------|
  |                               |
  |<-- EntitySpawn (world state)->|  Initial world sync
  |<-- EntitySpawn ...           -|
  |<-- GameStateSync ------------>|
  |                               |
  |--- ClientInput -------------->|  Gameplay begins
  |<-- InputAck, EntityUpdates ---|
```

### Ingress trust boundary

- A client accepts gameplay datagrams only from the exact IPv4 address and port configured by `Connect`.
- A server derives `ClientID` from its endpoint-to-client table; the wire-supplied sender ID is never authentication.
- Undefined channel bytes and malformed built-in payload sizes are rejected before dispatch.
- User-defined message types remain schema-optional for compatibility, but are accepted only from an established endpoint. Register a `MessageSchema` to enforce their size and direction.

Endpoint binding prevents off-endpoint injection, but it is not cryptographic peer authentication and does not support transparent endpoint migration. A reconnect is required when the server tuple changes.

## Entity Replication

### Replicated Properties

Each replicated entity has a set of properties tracked for synchronization:

| Field | Type | Description |
|-------|------|-------------|
| `networkID` | `uint32_t` | Unique network entity ID |
| `ownerID` | `ClientID` | Client that owns this entity |
| `entityType` | `string` | Type identifier for spawning |
| `position` | `XMFLOAT3` | World position |
| `rotation` | `XMFLOAT3` | Euler rotation |
| `velocity` | `XMFLOAT3` | Linear velocity |
| `properties` | `ReplicatedProperty[]` | Custom named properties |

### Delta Compression

`EntityStateUpdate` only sends properties marked dirty (`needsFullSync = false`). Each `ReplicatedProperty` has:
- `name`: Property identifier
- `type`: Int, Float, Vector3, String, Bool
- `serialize`/`deserialize`: Callbacks
- `dirty`: Changed since last sync

### Full Sync

Periodic `GameStateSync` sends complete entity state. `needsFullSync = true` triggers a full property send for a specific entity.

## Client Input State

```cpp
struct ClientInputState {
    uint32_t inputSequence;  // Monotonic input counter
    float moveForward;       // -1 to 1
    float moveRight;         // -1 to 1
    float lookYaw;           // Degrees
    float lookPitch;         // Degrees
    bool jump;
    bool fire;
    bool reload;
    bool sprint;
    bool crouch;
    float deltaTime;         // Client frame time
    float timestamp;         // Client timestamp
};
```

## Lag Compensation

The `LagCompensator` records `HistorySnapshot` frames (position/state at a given timestamp). When processing a client's attack:

1. Server receives hit request with client timestamp
2. `RewindToTime(clientTimestamp)` interpolates entity positions
3. Hit detection runs against rewound positions
4. World restored to current state

**Max history duration** is configurable via `SetMaxHistoryDuration()`.

## Serialization (NetBuffer)

All message payloads are serialized using `NetBuffer`:

| Method | Bytes | Description |
|--------|-------|-------------|
| `WriteUint8/ReadUint8` | 1 | Unsigned byte |
| `WriteUint16/ReadUint16` | 2 | Unsigned short |
| `WriteUint32/ReadUint32` | 4 | Unsigned int |
| `WriteFloat/ReadFloat` | 4 | IEEE 754 float |
| `WriteString/ReadString` | 4+N | Length-prefixed UTF-8 string |
| `WriteVector3/ReadVector3` | 12 | 3x float (x, y, z) |
| `WriteBytes/ReadBytes` | N | Raw byte array |

Strings are length-prefixed: 4-byte `uint32_t` length followed by UTF-8 bytes (no null terminator).

**Error handling:** `HasError()` returns true if any read exceeds buffer bounds. Subsequent reads return zero/empty. `CanRead(N)` checks before reading.

## Network Statistics

The `NetworkStats` struct tracks:

| Field | Type | Description |
|-------|------|-------------|
| `ping` | `float` | Round-trip time (ms) |
| `jitter` | `float` | Ping variance (ms) |
| `packetLoss` | `float` | Loss ratio (0.0-1.0) |
| `bytesSent` | `uint64_t` | Total bytes transmitted |
| `bytesReceived` | `uint64_t` | Total bytes received |
| `packetsSent` | `uint64_t` | Packets transmitted |
| `packetsReceived` | `uint64_t` | Packets received |

## Connection Management

- **Timeout:** Clients disconnected after no heartbeat for `m_connectionTimeout` seconds
- **Heartbeat interval:** Configurable, resets timeout counter
- **Auto-reconnect:** Client attempts reconnection with exponential backoff
- **Max reconnect attempts:** Configurable limit before giving up
- **Thread safety:** Queue mutex protects message I/O and handler registration
