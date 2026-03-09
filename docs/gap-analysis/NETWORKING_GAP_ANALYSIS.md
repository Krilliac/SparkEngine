# SparkEngine Networking — Gap Analysis

> **Scope**: `SparkEngine/Source/Engine/Networking/` (NetworkManager.h/cpp)
> **Date**: 2026-03-09
> **Methodology**: Static source-code audit of `NetworkManager.h` and `NetworkManager.cpp`.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The Networking subsystem is disabled by default (`ENABLE_NETWORKING=OFF` in CMake). It declares a comprehensive UDP-based client/server architecture for multiplayer FPS games in `NetworkManager.h` with:
- Client/server roles with connection state management
- Reliable and unreliable message channels
- Entity state replication with dirty property tracking
- Client-side prediction and input history
- Lag compensation (hitbox rewinding via `LagCompensator`)
- Serialization buffer (`NetBuffer`) with typed read/write operations
- Network statistics tracking

The `NetworkManager.cpp` implements `NetBuffer` serialization and some helper methods, but the core networking (socket I/O, connection handshake, replication) is largely absent.

---

## Critical Gaps

### GAP-N01 — No Socket Implementation

**Files**: `Engine/Networking/NetworkManager.h`, `Engine/Networking/NetworkManager.cpp`

**Impact**: The `NetworkManager` declares `StartServer()`, `Connect()`, `SendMessage()`, and `Update()` but there is no socket code. No `#include <winsock2.h>`, no `#include <sys/socket.h>`, no `bind()`, `sendto()`, `recvfrom()`, or any networking library.

**Evidence**: The `.cpp` file implements `NetBuffer` serialization (read/write integers, floats, strings, vectors) and console helper methods, but `StartServer()`, `Connect()`, `ProcessIncoming()`, and `ProcessOutgoing()` have 3 stub patterns.

**What is needed**: Implement UDP socket layer:
- Windows: Winsock2 (`WSAStartup`, `socket`, `bind`, `sendto`, `recvfrom`)
- Linux: POSIX sockets
- Or use a library like ENet, GameNetworkingSockets, or custom UDP

---

### GAP-N02 — No Connection Handshake Protocol

**Files**: `Engine/Networking/NetworkManager.h`

**Impact**: `MessageType::Connect`, `ConnectAccepted`, `ConnectRejected` are declared but no handshake protocol is implemented. Without this, clients cannot establish authenticated connections to servers.

**What is needed**: Implement a connection handshake:
1. Client sends `Connect` with player name and protocol version
2. Server validates (player limit, bans, version match)
3. Server responds with `ConnectAccepted` (assigns ClientID) or `ConnectRejected` (reason)
4. Client transitions to `Connected` state

---

### GAP-N03 — Reliable Message Delivery Not Implemented

**Files**: `Engine/Networking/NetworkManager.h`

**Impact**: `ChannelType::Reliable` and `ChannelType::ReliableOrdered` are declared but no acknowledgment, retransmission, or sequencing logic exists. All messages are effectively fire-and-forget.

**What is needed**: Implement reliable delivery:
- Sequence numbers per channel
- ACK/NACK tracking with bitfield
- Retransmission with configurable timeout
- Ordering buffer for `ReliableOrdered`

---

## Major Gaps

### GAP-N04 — Entity Replication Has No Actual Network Send

**Files**: `Engine/Networking/NetworkManager.h`

**Impact**: `ReplicatedEntity` structs with dirty property tracking are defined, but `UpdateReplication()` (which should serialize dirty properties and send them) is empty. Entities exist in the data structures but are never transmitted over the network.

**What is needed**: Implement delta compression: serialize only dirty properties, pack into `EntityStateUpdate` messages, and send via the unreliable channel. Periodically send full state syncs for correction.

---

### GAP-N05 — Client-Side Prediction Not Implemented

**Files**: `Engine/Networking/NetworkManager.h`

**Impact**: `ClientInputState` and input history (`m_inputHistory`) are declared for client-side prediction and server reconciliation, but the prediction loop (apply input locally, wait for server confirmation, reconcile) is not implemented.

**What is needed**: Implement:
1. Client applies input locally (prediction)
2. Client sends input to server with sequence number
3. Server processes input, sends authoritative state back
4. Client compares predicted vs. authoritative state
5. On mismatch, rewind to last confirmed state and re-apply pending inputs

---

### GAP-N06 — Lag Compensation Has Minimal Implementation

**Files**: `Engine/Networking/NetworkManager.h`, `Engine/Networking/NetworkManager.cpp`

**Impact**: `LagCompensator` declares `RecordSnapshot()` and `RewindToTime()` but the rewind implementation needs verification. For FPS hit validation, the server must rewind entity positions to the time the shooting client saw them.

**What is needed**: Verify that `RewindToTime()` correctly interpolates between snapshots and that the snapshot recording happens at the right frequency. Implement hit validation using rewound positions.

---

### GAP-N07 — No Encryption or Authentication

**Files**: `Engine/Networking/NetworkManager.h`

**Impact**: No packet encryption, no player authentication, no anti-cheat measures. Packets can be sniffed, injected, or replayed.

**What is needed**: At minimum, implement:
- Challenge-response authentication during handshake
- Packet encryption (DTLS or custom symmetric encryption)
- Server-side input validation (movement speed limits, fire rate limits)

---

## Moderate Gaps

### GAP-N08 — No Packet Fragmentation

**Files**: `Engine/Networking/NetworkManager.h`

**Impact**: No mechanism to split large messages across multiple UDP packets. Full state syncs or large payloads will exceed MTU (~1400 bytes) and be dropped.

**What is needed**: Implement message fragmentation and reassembly for messages exceeding MTU.

---

### GAP-N09 — No Bandwidth Throttling

**Files**: `Engine/Networking/NetworkManager.h`

**Impact**: `NetworkStats` tracks bandwidth but there is no rate limiting. The server could flood clients with updates exceeding their bandwidth.

**What is needed**: Implement per-client send rate limiting based on measured bandwidth and priority-based update scheduling.

---

### GAP-N10 — No Lobby/Matchmaking

**Files**: `Engine/Networking/NetworkManager.h`

**Impact**: No server browser, no lobby system, no matchmaking. Clients must know the server IP to connect.

**What is needed**: At minimum, implement a simple lobby with player list, ready state, and map selection. Server browser can use a master server or LAN broadcast.

---

### GAP-N11 — No Voice Chat Integration

**Impact**: No voice communication system for multiplayer FPS. This is expected at this stage but worth noting.

---

## Minor Gaps

### GAP-N12 — NetBuffer Has No Bounds Checking on Write

**File**: `Engine/Networking/NetworkManager.cpp`

**Impact**: `ReadUint8()`, `ReadUint16()`, `ReadUint32()` check bounds (returning 0 on overflow), but write operations have no maximum size check. A malicious or buggy caller could write unbounded data.

---

### GAP-N13 — NetworkManager Is a Singleton

**Impact**: Uses `GetInstance()` instead of `EngineContext` service locator, inconsistent with project architecture.

---

### GAP-N14 — No Network Simulation Tools

**Impact**: No artificial latency, packet loss, or jitter injection for testing. Makes it impossible to test network code under poor conditions during development.

---

## Summary Table

| ID | Severity | Gap | Impact |
|---|---|---|---|
| GAP-N01 | Critical | No socket implementation | No network transport |
| GAP-N02 | Critical | No connection handshake | Clients can't connect |
| GAP-N03 | Critical | No reliable delivery | Messages lost silently |
| GAP-N04 | Major | No replication send | Entities not synced |
| GAP-N05 | Major | No client prediction | Unplayable latency |
| GAP-N06 | Major | Lag comp incomplete | Unfair hit detection |
| GAP-N07 | Major | No encryption/auth | Security vulnerability |
| GAP-N08 | Moderate | No packet fragmentation | Large messages dropped |
| GAP-N09 | Moderate | No bandwidth throttling | Client flooding |
| GAP-N10 | Moderate | No lobby/matchmaking | No server discovery |
| GAP-N11 | Moderate | No voice chat | Missing feature |
| GAP-N12 | Minor | No write bounds checking | Potential overflow |
| GAP-N13 | Minor | Singleton pattern | Architecture inconsistency |
| GAP-N14 | Minor | No network simulation | Testing difficulty |

---

## Recommended Priority Order

1. **GAP-N01** — Socket implementation (unblocks everything)
2. **GAP-N02** — Connection handshake
3. **GAP-N03** — Reliable message delivery
4. **GAP-N04** — Entity replication
5. **GAP-N05** — Client-side prediction
6. **GAP-N06** — Lag compensation verification
7. **GAP-N07** — Basic authentication
8. Everything else
