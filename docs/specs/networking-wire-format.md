# SparkEngine Networking Wire Format Specification

**Version:** 3.0 (protocol version 3; framing from NET-100, protocol version 2)  
**Date:** 2026-09-29  
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
| `SequenceNumber` | `uint32_t` | Per-peer, per-direction counter: reliability/ACK sequence, and the separate ordered-stream sequence |
| `NetworkTime` | `float` | Server time in seconds |

## Datagram Framing (protocol v2, NET-100)

Every datagram is one **frame**. Its first byte is the frame kind:

| Kind | Value | Body | Carries |
|------|-------|------|---------|
| Handshake | `0x01` | one plaintext `NetworkMessage` (layout below) | only `Connect`, `ConnectAccepted`, `ConnectRejected` |
| Sealed | `0x02` | one `SecureChannel` packet: `[version u8 = 1][key epoch u8][sequence u64][ciphertext][tag 16]` | every other message; the plaintext is the serialized `NetworkMessage` |

A sealed frame is ChaCha20-Poly1305 (libsodium IETF AEAD). The associated data is the 10-byte
SecureChannel header followed by the frame-kind byte, so the kind, version, epoch and sequence
are all authenticated. Per-direction keys come from the handshake below; nothing else keys a
channel.

Receive rules (`NetworkManager::ProcessIncoming`), applied before anything is parsed:

- A sealed frame is opened only with the channel of the peer its source endpoint belongs to
  (the server's endpoint-to-client table; a client's single server). No channel, or any
  `OpenResult` other than `Ok` (Malformed, UnsupportedVersion, UnknownKeyEpoch,
  AuthenticationFailed, Replayed), drops it and counts it in `NetworkStats::securityDrops`.
- A sealed frame that decrypts to a handshake message type is dropped.
- A handshake frame is accepted only for `Connect` at a server, or for `ConnectAccepted` /
  `ConnectRejected` at a client that is still `Connecting`. Everything else, including any
  gameplay message in plaintext and any unknown frame kind, is dropped and counted in
  `plaintextFramesDropped`.
- An unframed datagram that starts with the `SPRK` magic is a pre-v2 peer. A server answers an
  unframed `Connect` (payload of at least 8 bytes) with one unframed typed `ConnectRejected`
  (`ProtocolTooOld`, `ProtocolMissing` or `ProtocolTooNew`) and keeps no state for it.

Send rules (`NetworkManager::SendFrameTo`): handshake types are framed in plaintext; every other
message is sealed at transmit time with the destination's channel (so delayed and retransmitted
copies get fresh sequence numbers) or not sent at all (`unsealedSendsRefused`). There is no
plaintext mode. A message marked `sensitive` is refused before queueing unless a channel to its
destination exists.

**Nonce discipline.** The AEAD nonce is `[epoch][0 0 0][sequence u64 LE]`. Each direction has its
own key, the sender's sequence strictly increases from 1 and is never reused within an epoch, and
each epoch has its own key (one-way HKDF ratchet). The receiver keeps a 256-packet sliding window
that is committed only after the tag verifies, so reordering inside the window is delivered and
any duplicate is `Replayed`. A sender rotates its key after 2^31 packets or 600 s of session time
(`SECURE_ROTATE_AFTER_PACKETS`, `SECURE_ROTATE_AFTER_SECONDS`); the receiver follows on the first
authenticated packet of the next epoch. When the 8-bit epoch space is spent the channel is
dropped and the session ends by timeout, so a (key, nonce) pair can never repeat.

**Size.** Frame overhead is 27 bytes (`NETWORK_FRAME_OVERHEAD`) and the largest message header is
27 bytes (23 fixed + the 4-byte ordered sequence of a ReliableOrdered message), so
`MAX_NETWORK_MESSAGE_PAYLOAD_SIZE = 65507 - 27 - 27 = 65453` on every channel.

## Message Structure

Each frame contains one `NetworkMessage`:

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
| 11 | 4 | `uint32_t` | `sequence` | Reliability sequence (dedup, ACK, retransmit); 0 = untracked |
| 15 | 4 | `float` | `timestamp` | Server time when created |
| 19 | 4 | `uint32_t` | `payloadSize` | Length of payload in bytes |
| 23 | 4 | `uint32_t` | `orderedSequence` | **ReliableOrdered (channel 2) only**: ordered-stream sequence; 0 = deliver unordered |
| 23 or 27 | N | `uint8_t[]` | `payload` | Raw serialized message body |

**Total header size:** 23 bytes for Unreliable and Reliable, 27 bytes for ReliableOrdered, plus
the variable payload. `payloadSize` must equal the bytes that remain after the header.

**Two sequence spaces (protocol v3).** Each peer and direction numbers two streams, both starting
at 1 and wrapping from `0xFFFFFFFF` to 1 (0 is never assigned). Every Reliable and ReliableOrdered
message takes the next *reliability* sequence, which `Ack` acknowledges. A ReliableOrdered message
also takes the next *ordered* sequence, and only that sequence drives in-order delivery. Protocol
v2 used the reliability sequence for ordering, so the first ReliableOrdered message after any
Reliable one (every client's `ClientFinished`, for example) left a gap the receiver waited on
forever. Both streams restart when a session ends (disconnect, kick, reconnect).

Sensitive-payload ownership is intentionally absent from the wire format.
Senders mark their local `NetworkMessage` copies with `sensitive`; receivers
independently classify sensitive message types with `RegisterSensitiveHandler`.
This promptly erases queue, retransmit, dispatch, and serialization buffers
without changing the message layout or trusting sender-controlled metadata.
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
- Guaranteed delivery AND in-order processing, ordered by the header's `orderedSequence`
- Messages buffered until gaps in the ordered stream are filled (at most 4096 per peer); a copy
  older than the next expected ordered sequence is acknowledged and dropped
- Highest overhead
- Use for: important game events, match state transitions

## Message Types

### Connection Messages

| Type | Value | Direction | Payload | Description |
|------|-------|-----------|---------|-------------|
| `Connect` | 1 | C→S | ClientHello (55 bytes) | Connection request (plaintext frame) |
| `ConnectAccepted` | 2 | S→C | Assigned ClientID, server time, echoed protocol version, ServerHello | Handshake answer (plaintext frame) |
| `ConnectRejected` | 3 | S→C | Reason text, typed reason code, server protocol version | Connection denied |
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
| `DeltaAck` | 19 | C→S | Applied delta sequence | Delta replication acknowledgement |
| `ClientFinished` | 20 | C→S | Player name (length-prefixed, at most 64 bytes) | First sealed message; completes admission |
| `UserDefined` | 1000+ | Both | Custom | Game-specific messages |

A module whose payload prepends its own header (a channel byte, a sub-type tag) must claim a type in
the `UserDefined` range and register a schema for **that** type with the matching
`stringFieldOffset`. Re-registering a built-in type's schema replaces a process-wide entry that is
never restored on module shutdown, so every other producer and consumer of that type — the engine
included, after the module unloads — would keep validating against the module's layout.
`GameModules/SparkGameMMO` does this correctly: `UserDefined + 1` with `stringFieldOffset = 1`.

## Handshake Sequence

```
Client                                     Server
  |                                          |
  |--- Connect (ClientHello) --------------->| plaintext frame; version, size, suite,
  |                                          | slots; sign transcript; state Securing
  |<-- ConnectAccepted (ID, ServerHello) ----| plaintext frame (or ConnectRejected)
  | verify key (pin / known_hosts)           |
  | and signature; derive channel            |
  |=== ClientFinished (name) ===============>| sealed; state Connected, admission event
  |                                          |
  |=== Heartbeat ===========================>|  every frame from here on is sealed
  |<== Heartbeat ============================|
  |                                          |
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

### Handshake and protocol-version negotiation

The session protocol version is `NETWORK_PROTOCOL_VERSION` in `NetworkManager.h` (currently `2`).
Bump it on every incompatible change to this document; peers must match exactly, and there is no
silent downgrade: the version and the cipher suite sit inside the ClientHello, which is part of
the transcript the server signs, so rewriting either breaks the handshake.

| Message | Payload layout (little-endian) | Size |
|---------|--------------------------------|------|
| `Connect` | ClientHello: `uint32 magic = 0x484E5053` ("SPNH"), `uint16 protocolVersion`, `uint8 suite = 1`, X25519 ephemeral key (32), client nonce (16) | exactly 55 |
| `ConnectAccepted` | `uint32 clientID`, `float serverTime`, `uint16 protocolVersion` (echo), ServerHello: `uint8 suite`, server Ed25519 identity key (32), server X25519 ephemeral key (32), server nonce (16), Ed25519 signature (64) | exactly 155 |
| `ConnectRejected` | `uint16 textLength`, text bytes, `uint8 reason`, `uint16 serverProtocolVersion` | max 256 |
| `ClientFinished` (sealed) | `uint16 nameLength`, name bytes | 2 + at most 64 |

The signature covers
`SHA-256("SPNH-v3" || ClientHello || accept prefix || ServerHello without its signature)`, where
the accept prefix is the first 10 bytes of `ConnectAccepted` (`clientID`, `serverTime`, echoed
`protocolVersion`). The client adopts the assigned `clientID` only after that signature verifies,
so an on-path rewrite of the id (handing the client another player's identity) fails the
handshake. The session secret is `HKDF-SHA256(salt = transcript hash, X25519 shared secret)`.
Details and the primitive list: `SparkEngine/Source/Engine/Networking/SecureHandshake.h`.

Server (`NetworkManager::HandleConnect`) checks the handshake **before** it considers a client slot
or does any curve or signature work. A small-order client ephemeral key is found by comparison
against libsodium's blocklist, not by a scalar multiplication; and `RespondToClientHello` derives
the shared secret before it signs, so no refused hello ever costs an Ed25519 signature:

| Condition | `ConnectRejectReason` |
|-----------|-----------------------|
| Payload below 8 bytes | none: the `PacketValidator` schema drops it before `HandleConnect`, with no reply and no slot |
| Magic absent (a pre-negotiation client sending only its name) | `ProtocolMissing` (2) |
| Client version older than the server's | `ProtocolTooOld` (3) |
| Client version newer than the server's | `ProtocolTooNew` (4) |
| Payload is not exactly one 55-byte ClientHello, or its ephemeral key is low-order | `MalformedHandshake` (5) |
| Suite byte is not suite 1 | `UnsupportedSuite` (7) |
| Every slot is occupied | `ServerFull` (1) |

Each rejection is sent only to the pending endpoint, whose pre-registered address is then
forgotten, so a rejected peer never occupies server state. An accepted hello creates a slot in
state `Securing` with its channel. A `Securing` slot is not a player: `NetworkManager::GetClients()`
lists only `Connected` clients (`GetClientSlots()` lists every slot), and `SendToClient` refuses
every message except `ConnectAccepted`/`ConnectRejected` to a slot that is not `Connected`
(`NetworkStats::unadmittedSendsRefused`), so replication, full syncs, chat and game broadcasts
cannot reach it. The client is surfaced to observers (the `Connect` admission event) only after
its sealed `ClientFinished` opens under that channel. A `Securing` slot that never finishes is
removed by the connection timeout.
`ConnectAccepted` is unreliable: a client that lost it retransmits the identical `Connect`, and the
server answers that once more, so the server never sends more than one datagram per datagram it
receives (the reply is about 2.3 times the request; anti-amplification cookies are later NET-100
work).

**Per-source Connect budget.** Every `Connect` from an endpoint without an admitted session
(including legacy unframed ones and retransmits) is charged to a token bucket keyed by the source
IPv4 address (`ConnectRateLimiter`, `NetworkSecurityConfig::connectRate`, default burst 16 and
4 per second). A source over budget is dropped with **no reply**, so it can neither make the
server do handshake work nor use it to reflect datagrams (`NetworkStats::connectsRateLimited`).
At most 4096 sources are tracked; when the table is full, sources whose buckets have refilled are
forgotten first, and a new source is refused while none has.

Client: `ConnectAccepted` is verified before anything else happens.

| Condition | Client outcome (`GetLastConnectRejectReason()`) |
|-----------|--------------------------------------------------|
| Echoed version differs | `ProtocolMismatch` (6) |
| Not exactly 155 bytes (for example a legacy echo with no ServerHello) | ignored; the handshake is not consumed |
| ServerHello names a key other than the pinned or recorded one | `ServerIdentityMismatch` (8) |
| Bad signature, rewritten suite or accept prefix (client id, server time), low-order ephemeral key, or an unusable known_hosts | `HandshakeAuthFailed` (9) |

Any of these returns the client to `Disconnected`, closes its socket, and discards queued
lifecycle traffic, just as `ConnectRejected` does. A `ConnectRejected` without a readable typed
trailer still ends the attempt with reason `Unspecified` (0).

**Server identity and client trust** (`NetworkTrustStore.h`). A server cannot start without an
Ed25519 identity (`NetworkManager::SetSecurityConfig`, or `UseDefaultSecurityConfig`, which loads
or creates the owner-only `<user data>/net/server_identity.key`). A client cannot connect without
a usable `ServerTrust`: either a pinned key, or trust-on-first-use backed by
`<user data>/net/known_hosts`, which records a key only after its signature verified and never
replaces it silently.

Mixed deployments: a version-1 client sends unframed datagrams; the server answers its `Connect`
with an unframed typed `ProtocolTooOld`, which the old client can parse. A version-2 client
talking to a version-1 server gets no answer (the old server cannot parse a frame), so it times
out; no session forms either way.

Evidence: `Tests/TestSessionCompatibilityReal.cpp` (CTest `NetworkSessionCompatibility`) and
`Tests/TestSecureTransportWired.cpp` (CTest `NetworkSecureTransportWired`), both labelled
`network-security`, run the production `NetworkManager` in both roles over real loopback UDP.

### Ingress trust boundary

- A client accepts gameplay datagrams only from the exact IPv4 address and port configured by `Connect`.
- A server derives `ClientID` from its endpoint-to-client table; the wire-supplied sender ID is never authentication.
- Undefined channel bytes and malformed built-in payload sizes are rejected before dispatch.
- User-defined message types remain schema-optional for compatibility, but are accepted only from an established endpoint. Register a `MessageSchema` to enforce their size and direction.

Endpoint binding selects which peer's channel a datagram is opened with; the channel is what
authenticates it. A datagram from the right endpoint sealed under another peer's key fails with
`AuthenticationFailed`. Transparent endpoint migration is not supported: a reconnect (and a new
handshake) is required when the tuple changes.

**What the handshake authenticates.** The client authenticates the server (its identity key). The
server does not authenticate the player at the transport layer: any client can complete a
handshake, and player or account authentication runs inside the channel (for TERRAFRONT, SCRAM;
see `docs/specs/terrafront-onboarding-design.md`). SparkGateway admission tickets are not yet bound
to the UDP session; the decided design is that a ticket is presented inside the sealed channel
(in `ClientFinished`, so it is bound to this session's keys and cannot be replayed on another
session) and checked by the area server before admission. Until that lands, gameplay UDP admission
does not consult SparkGateway, and a deployment that relies on the gateway for admission control
must also restrict who can reach the area servers.

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
