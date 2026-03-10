# SparkEngine — Networking Subsystem Gap Analysis

> Audit of the `Engine/Networking/` subsystem covering the `NetworkManager`,
> serialization, replication, lag compensation, and related networking features.
> Statuses are based on code inspection of `NetworkManager.h`,
> `NetworkManager.cpp`, and `Tests/TestNetBuffer.cpp`.

**Summary**: 7 DONE | 3 PARTIAL | 1 STUB | 3 MISSING

---

## Status Legend

| Tag         | Meaning                                                    |
|-------------|------------------------------------------------------------|
| **DONE**    | Fully implemented, tested, and production-ready            |
| **PARTIAL** | Core path works but edge cases / features are incomplete   |
| **STUB**    | Header or signature exists; body is empty or returns dummy |
| **MISSING** | Not yet created; no header, no source                      |

---

## 15. Networking

### 15.1 Serialization (`NetBuffer`)
- [x] `NetBuffer` read/write primitives (uint8, uint16, uint32, float, string, Vector3, raw bytes) — **DONE** — Full implementation with little-endian encoding, bounds-checked reads, and error flag propagation
- [x] Wire-format message serialization (`SerializeMessage` / `DeserializeMessage`) — **DONE** — Custom binary protocol with magic number (`0x5350524B`), header fields, and payload; guarded by `ENABLE_NETWORKING`

### 15.2 NetworkManager Client/Server
- [x] `NetworkManager` singleton with `Initialize()` / `Shutdown()` lifecycle — **DONE** — Platform socket init (WSAStartup on Windows), built-in handler registration, clean teardown of all resources
- [x] `StartServer()` — bind UDP socket, set role to Server — **DONE** — Configurable port and max clients, auto-initializes if needed
- [x] `Connect()` — resolve address, set role to Client, send connect request — **DONE** — Sends player name in connect payload, transitions through `Connecting` state
- [x] `Disconnect()` / `StopServer()` — graceful disconnect with notification — **DONE** — Server broadcasts disconnect reason to all clients before closing; client sends disconnect message to server

### 15.3 Entity Replication
- [x] `ReplicatedEntity` and `ReplicatedProperty` data model — **PARTIAL** — Property registration with typed serialize/deserialize callbacks and dirty flags exists; replication tick runs at configurable rate (default 20 Hz). However, only dirty-property delta sync is implemented — there is no authority model, no interest management, and no bandwidth prioritization per entity.
- [x] `SerializeEntityState()` / `DeserializeEntityState()` — **DONE** — Serializes network ID, entity type, position, rotation, velocity, and all registered properties into `NetBuffer`; client-side deserialization applies updates to local replicated entities
- [x] `SendFullEntitySync()` — **PARTIAL** — Sends all replicated entities to a target client on connect; works for initial sync but lacks incremental join-in-progress handling for late joiners

### 15.4 Lag Compensation
- [x] `LagCompensator` — snapshot recording and history pruning — **DONE** — Stores per-tick snapshots with configurable history duration (default 1 second), prunes old entries automatically
- [x] `RewindToTime()` — server-side hitbox rewinding — **PARTIAL** — Interpolates between two bracketing snapshots using `XMVectorLerp` for position, rotation, and AABB bounds. Edge cases (single snapshot, exact match) are handled. However, there is no integration with the physics/hit-detection pipeline — the rewind produces a snapshot but nothing consumes it for actual hit validation yet.

### 15.5 Client-Side Prediction
- [ ] Client input history for prediction/reconciliation — **STUB** — `m_inputHistory` vector and `ClientInputState` struct exist with full FPS input fields (move, look, jump, fire, reload, sprint, crouch). `SendClientInput()` serializes and sends input to the server. However, the client-side prediction loop (local simulation + server correction reconciliation) is not implemented — inputs are recorded but never replayed.

### 15.6 Reliable / Unreliable Channels
- [x] `ChannelType` enum (Unreliable, Reliable, ReliableOrdered) — **PARTIAL** — Enum and per-message channel field are defined. Reliable messages are tracked in `m_unacknowledgedMessages` with a retransmit interval (`0.5s`). However, acknowledgment processing, retransmission logic, and ordered delivery enforcement are not implemented — reliable messages are sent once and tracked but never actually retransmitted or acknowledged.

### 15.7 UDP Socket Layer
- [x] `CreateSocket()` — non-blocking UDP socket creation and binding — **DONE** — Platform-correct implementation for both Windows (`ioctlsocket`) and POSIX (`fcntl`); configures send/receive buffer sizes (64 KB)
- [x] `SendRawTo()` / `ReceiveRaw()` — low-level send/receive — **DONE** — `sendto`/`recvfrom` with bandwidth tracking and packet drop counting; max packet size 4096 bytes
- [x] `CloseSocket()` — platform-correct socket teardown — **DONE** — `closesocket` on Windows, `close` on POSIX

### 15.8 Connection Management
- [x] `HandleConnect()` — server accepts new clients, assigns ClientID — **DONE** — Validates max client limit, creates `ClientInfo`, sends `ConnectAccepted` with assigned ID, stores client socket address
- [x] `HandleDisconnect()` — removes client, cleans up replicated entities — **DONE** — Unregisters all entities owned by disconnecting client, removes from client map
- [x] Heartbeat system — **DONE** — Periodic heartbeat messages at configurable interval (default 1s); server tracks `lastHeartbeatTime` per client
- [ ] Connection timeout detection — **MISSING** — `m_connectionTimeout` field exists (10s default) but no code checks elapsed time since last heartbeat to detect and drop timed-out clients

### 15.9 Message Handler Registration
- [x] `RegisterHandler()` — type-based message dispatch — **DONE** — Thread-safe handler map (protected by `m_handlerMutex`); supports one handler per `MessageType`; built-in handlers for Connect, Disconnect, ConnectAccepted, Heartbeat, EntityStateUpdate, and ClientInput are registered during `Initialize()`

### 15.10 Network Testing
- [x] `TestNetBuffer.cpp` — NetBuffer serialization tests — **DONE** — 18 unit tests covering round-trip for all primitive types, mixed-type sequences, boundary conditions (empty buffer, exact capacity, reset), and overrun error detection/propagation
- [ ] NetworkManager integration tests — **MISSING** — No tests for connection lifecycle, replication, message dispatch, lag compensation, or multi-client scenarios
- [ ] Loopback / mock socket tests — **MISSING** — No mock socket layer or loopback transport to enable testing without real network I/O

### 15.11 Cross-Platform Socket Support
- [x] Windows (WinSock2) and POSIX (BSD sockets) — **DONE** — Platform headers conditionally included; `SOCKET` type aliased on POSIX; `INVALID_SOCKET` and `SOCKET_ERROR` defined; non-blocking mode set via platform-appropriate API
- [x] `NetworkManagerStub` for disabled builds — **DONE** — Header-only no-op stub provided when `ENABLE_NETWORKING` is not defined, allowing the engine to compile and link without the networking implementation

---

## Summary Table

| Item                              | Status      |
|-----------------------------------|-------------|
| NetBuffer serialization           | DONE        |
| Wire-format message protocol      | DONE        |
| NetworkManager lifecycle          | DONE        |
| Client/server connect/disconnect  | DONE        |
| Entity replication data model     | PARTIAL     |
| Entity state serialization        | DONE        |
| Lag compensation (LagCompensator) | PARTIAL     |
| Client-side prediction            | STUB        |
| Reliable/unreliable channels      | PARTIAL     |
| UDP socket layer                  | DONE        |
| Connection management             | DONE        |
| Connection timeout detection      | MISSING     |
| Message handler registration      | DONE        |
| NetBuffer unit tests              | DONE        |
| NetworkManager integration tests  | MISSING     |
| Mock/loopback transport tests     | MISSING     |
| Cross-platform socket support     | DONE        |
| NetworkManagerStub (disabled)     | DONE        |

---

## Next Steps (Prioritized)

1. **Implement reliable channel retransmission and ACK processing** — The infrastructure (sequence numbers, unacknowledged map, retransmit interval) is in place but the actual retransmit loop and ACK handling are missing. This is critical for any reliable game state communication.
2. **Implement connection timeout detection** — Add a check in `Update()` that compares `m_serverTime - client.lastHeartbeatTime` against `m_connectionTimeout` and disconnects stale clients.
3. **Complete client-side prediction** — Implement the local simulation + server reconciliation loop so that `m_inputHistory` is replayed on correction. This is essential for responsive FPS gameplay.
4. **Integrate lag compensation with hit detection** — `RewindToTime()` produces interpolated snapshots but nothing in the physics or combat system uses them. Wire it into the server-side hit validation path.
5. **Add NetworkManager integration tests** — Create loopback or mock-socket tests for connection lifecycle, entity replication, and message dispatch to catch regressions.
6. **Add entity authority and interest management** — The replication system lacks ownership authority checks and per-client relevance filtering, which will be needed for scalable multiplayer.
