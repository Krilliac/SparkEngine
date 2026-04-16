# SparkDaemon Phase 2a — Shader Cache Service

**Last updated:** 2026-04-16
**Type:** Observation
**Status:** Active
**Cross-references:** `daemon-services-architecture-2026-04-16.md`, `daemon-phase-1-foundation-2026-04-16.md`

## TL;DR

Phase 2a ships a shader-blob cache service over the daemon: engines can
store and retrieve compiled bytecode keyed by `(sourceHash, target, stage)`.
In-memory only — persistent disk storage and actual compilation / file
watching are future phases. 6 new loopback tests pass; full suite 5518/5518.

## API surface

`ShaderServiceClient` (engine side, wraps a `DaemonClient&`):

| Method | Wire message | Notes |
|---|---|---|
| `GetCacheEntry(hash, target, stage)` | `0x0001 → 0x0002` | Miss returns `found=false, blob={}`; counts miss |
| `PutCacheEntry(hash, target, stage, blob)` | `0x0003 → 0x0004` | Overwrites existing entry under same key |
| `ClearCache()` | `0x0005 → 0x0006` | Drops all entries, resets stats |
| `GetCacheStats()` | `0x0007 → 0x0008` | Entry count, total bytes, hit / miss counters |

Target and stage are opaque `uint8_t` on the wire — the daemon does not
know what DXIL / DXBC / SPIR-V / Vertex / Pixel mean. Callers pass
`static_cast<uint8_t>(Spark::Graphics::ShaderTarget::DXBC)` etc.

## Design decisions

### 1. Error responses are service-agnostic

Message type `0x00FF` is now **reserved across every service** as an
error reply — not just the Control service. A service returning an
error constructs a `ServiceResponse{messageType=ErrorResponse,
payload=errorString}`. The framing layer sends it under the service's
own ID; `DaemonClient::Request` detects `messageType == 0x00FF`
regardless of service ID and surfaces it uniformly.

This avoids forcing every service to route errors through the Control
service and avoids adding a service-ID override to the `ServiceResponse`
struct.

### 2. Payload codecs are free functions in the shared header

`ShaderServiceProtocol.h` defines both `struct GetCacheEntryRequest` and
`EncodeGetCacheEntryRequest(req) → vector<uint8_t>` / matching decode
function. Both the engine client and the daemon service include the
same header and call the same free functions. No code duplication.

`Spark::BinaryWriter` / `Spark::BinaryReader` (Serializer.h) is
header-only, so pulling it into the daemon costs nothing.

### 3. Keep target/stage as `uint8_t` not an enum

Pros:
- Daemon has zero coupling to `Spark::Graphics::ShaderTarget`
- Adding a new target in the graphics layer doesn't force a daemon rebuild
- Daemon can be upgraded independently of the engine

Cons:
- Caller site loses enum type-safety; needs `static_cast<uint8_t>(...)`

Chose the decoupling — this is the architectural boundary, and the cast
is explicit at call sites.

### 4. Hit / miss counters are relaxed atomics

The entry map is `mutex`-guarded, but bumping the hit / miss counter
doesn't need to be under that lock. Using `std::atomic<uint64_t>` with
`memory_order_relaxed` avoids the extra serialization and is correct for
"approximate stats" semantics.

## Not in Phase 2a

- **On-disk persistence** — cache is lost when the daemon exits. Next slice.
- **HLSL → DXBC / DXIL / SPIR-V compilation in the daemon** — this is the
  big payoff (warm DXC, parallel variant compile) but much larger scope.
- **File watching / push notifications on `.hlsl` changes** — needs an
  inotify / FSEvents / kqueue abstraction we don't have yet.
- **Engine integration** — `Shader::Initialize` and `ShaderDiskCache`
  still work entirely in-process; no daemon consultation. A follow-up
  will wire `ShaderServiceClient` into the shader lookup path with a
  fall-through to the local cache when no daemon is reachable.

## Client wrapper pattern — template for future services

The `ShaderServiceClient` structure is the template other service
wrappers (Asset, Collab, Build) should copy:

1. Header with a typed method per RPC, each returning
   `std::expected<ResponseType, std::string>`.
2. Constructor takes `DaemonClient&` by reference — the wrapper does
   not own the transport.
3. Each method builds the request struct, encodes via the shared
   protocol header, calls `m_client.Request(...)`, then decodes.
4. Transport errors are wrapped with the method name so callers can
   distinguish which RPC failed.
5. All state lives in the daemon; the wrapper is stateless, so it's
   trivially thread-safe on top of `DaemonClient`'s existing mutex.

## File list

| File | Purpose |
|------|---------|
| `SparkEngine/Source/Utils/ShaderServiceProtocol.h` | Message enum + request/response structs + encode/decode helpers |
| `SparkEngine/Source/Utils/ShaderServiceClient.{h,cpp}` | Typed engine-side facade over `DaemonClient` |
| `SparkDaemon/src/ShaderService.{h,cpp}` | In-memory blob store keyed by `(hash, target, stage)` |
| `Tests/TestShaderServiceClient.cpp` | 6 loopback tests — roundtrip, miss, stats, clear, overwrite, target-distinctness |

## Touched Phase 1 files

- `DaemonClient.cpp`: ErrorResponse detection now triggers on message type
  alone rather than service + type, per reservation decision above.
- `DaemonProtocol.h`: `ControlMessage::ErrorResponse` comment updated to
  document the reservation.
- `SparkDaemon/src/main.cpp`: registers `ShaderService` alongside
  `ControlService` at startup.
