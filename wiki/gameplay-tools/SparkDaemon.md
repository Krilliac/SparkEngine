# SparkDaemon

Long-lived background process that owns shared mutable state (asset cache,
shader cache, and the optional orchestration control plane) and serves multiple
engine / editor / tool instances over local IPC. Collaboration is implemented
by the separately isolated `SparkCollabServer`; build-watcher events remain a
future service.

Inspired by Wine's wineserver: one process serialises shared state, every
client is a thin handle. The daemon is always **optional** — every engine
subsystem that uses it retains an in-process fallback, and removing the
daemon is never more than turning a CVar off.

## Table of contents

1. [Why a daemon?](#why-a-daemon)
2. [Architecture](#architecture)
3. [Wire protocol](#wire-protocol)
4. [Services shipped today](#services-shipped-today)
5. [Running the daemon](#running-the-daemon)
6. [Enabling it from the engine](#enabling-it-from-the-engine)
7. [Adding a new service](#adding-a-new-service)
8. [Platform support](#platform-support)
9. [Ops and debugging](#ops-and-debugging)
10. [Future phases](#future-phases)

---

## Why a daemon?

Four problems the daemon is designed to solve:

- **Two editors can't share a warm shader cache.** Every engine instance
  recompiles DXC / SPIRV-Cross variants from scratch on first boot, even
  though the bytecode is identical.
- **Cooked asset blobs aren't shareable across tools.** Editor, headless
  cook tool, and game preview each carry their own in-process `AssetCache`.
- **Collaborative editing is P2P with no persistence.** Host crash loses
  all session state.
- **File watching is per-process.** Every running engine polls timestamps
  on its own schedule.

A single long-lived process that owns the shared state and multiplexes
multiple clients solves all four with the same pattern.

## Architecture

```
┌─────────────┐  ┌─────────────┐  ┌──────────────┐
│  Editor #1  │  │  Editor #2  │  │ CLI cook tool│
│  (client)   │  │  (client)   │  │  (client)    │
└──────┬──────┘  └──────┬──────┘  └──────┬───────┘
       │                │                │
       └────────┬───────┴────────┬───────┘
                │  AF_UNIX       │
          ┌─────┴────────────────┴─────┐
          │       SparkDaemon          │
          │  ┌──────────┐ ┌─────────┐  │
          │  │ Control  │ │ Shader  │  │
          │  │  (ping,  │ │ (cache) │  │
          │  │  stats,  │ ├─────────┤  │
          │  │  shut-   │ │ Asset   │  │
          │  │  down)   │ │ (cache) │  │
          │  └──────────┘ └─────────┘  │
          └────────────────────────────┘

          SparkCollabServer (separate process/trust boundary)
```

Every client holds one `DaemonClient` (a single AF_UNIX socket). Each
subsystem wraps that client with a typed facade: `ShaderServiceClient`,
`AssetServiceClient`. The daemon binds one socket, accepts connections,
and dispatches incoming framed messages to the registered service that
matches their `ServiceId`.

Key source files:

| Layer | File |
|------|------|
| Wire protocol | `SparkEngine/Source/Utils/DaemonProtocol.h` |
| Framing helpers | `SparkEngine/Source/Utils/DaemonFraming.h` |
| Engine-side client | `SparkEngine/Source/Utils/DaemonClient.{h,cpp}` |
| Process-wide client lifecycle | `SparkEngine/Source/Utils/DaemonConnection.{h,cpp}` |
| Engine lifecycle glue | `SparkEngine/Source/Utils/DaemonLifecycle.{h,cpp}` |
| Daemon executable | `SparkDaemon/src/main.cpp` |
| Daemon server | `SparkDaemon/src/DaemonServer.{h,cpp}` |
| Service base | `SparkDaemon/src/ServiceBase.h` |
| Shader service (daemon) | `SparkDaemon/src/ShaderService.{h,cpp}` |
| Asset service (daemon) | `SparkDaemon/src/AssetService.{h,cpp}` |
| Control service (daemon) | `SparkDaemon/src/ControlService.{h,cpp}` |
| Shader client wrapper (engine) | `SparkEngine/Source/Utils/ShaderServiceClient.{h,cpp}` |
| Asset client wrapper (engine) | `SparkEngine/Source/Utils/AssetServiceClient.{h,cpp}` |
| CompiledShaderBlob bridge | `SparkEngine/Source/Graphics/ShaderDaemonBridge.{h,cpp}` |

## Wire protocol

Frames are length-prefixed little-endian binary:

```
[4 bytes: payload length][2 bytes: service ID][2 bytes: message type][N bytes: payload]
```

Fixed-size header = `kFrameHeaderSize` = 8 bytes. Max payload =
`kMaxPayloadSize` = 16 MiB — frames over this are rejected as malformed.

Service IDs (`DaemonProtocol.h::ServiceId`):

| ID | Service | Status |
|----|---------|--------|
| 0x0000 | Control | shipped (ping, version, stats, shutdown) |
| 0x0001 | Asset | shipped (cache get/put/invalidate/stats) |
| 0x0002 | Shader | shipped (cache get/put/clear/stats) |
| 0x0003 | Collab | shipped in standalone `SparkCollabServer` |
| 0x0004 | Build | reserved — future phase |
| 0x0005 | Orchestration | shipped, explicit opt-in in `SparkDaemon` |

Message type `0x00FF` is **reserved across every service** as an error
reply. Any service that can't handle a request returns a response with
`messageType = 0x00FF` and a UTF-8 error string in the payload.
`DaemonClient::Request` surfaces those uniformly as `std::unexpected()`.

Protocol version string = `Spark::Daemon::kProtocolVersion` =
`"1.0.0"`. Returned by `Control::VersionRequest` and
`Control::StatsRequest`. Bump when the frame header layout changes.

## Services shipped today

### Control

Always registered. Uses empty payloads for all requests except
`ErrorResponse` and `StatsResponse`.

| Request | Response | Payload |
|---------|----------|---------|
| `PingRequest` (0x0001) | `PingResponse` | string `"pong"` |
| `VersionRequest` (0x0003) | `VersionResponse` | protocol version string |
| `ShutdownRequest` (0x0005) | `ShutdownAck` | empty |
| `StatsRequest` (0x0007) | `StatsResponse` | `DaemonStats` (uptime, version, registered IDs) |

### Shader

Domain-agnostic blob cache keyed by `(sourceHash: uint64, target: uint8, stage: uint8)`.

| Request | Response | Notes |
|---------|----------|-------|
| `GetCacheEntry` (0x0001) | `GetCacheEntryResponse` | miss returns `found=false` |
| `PutCacheEntry` (0x0003) | `PutCacheEntryResponse` | overwrites existing entry |
| `ClearCache` (0x0005) | `ClearCacheResponse` | drops all entries, resets stats |
| `GetCacheStats` (0x0007) | `GetCacheStatsResponse` | entry count, total bytes, hits, misses |

Optional persistence via `--cache-dir <path>`. On disk each entry is a
file `<hash16>_<target3>_<stage3>.blob` containing raw bytecode.

The engine-side bridge (`ShaderDaemonBridge`) serialises the full
`CompiledShaderBlob` (bytecode + target + stage + entry point + errors
+ reflection counters) into a single byte buffer so the daemon sees
only opaque bytes.

**LRU eviction (Phase 5):** pass `--shader-cache-max-mb <N>` to cap
the in-memory working set. When a `PutCacheEntry` pushes total payload
bytes over the limit, the service evicts oldest entries (tracked as a
linked list in insertion order) until the budget is satisfied, deleting
the corresponding `.blob` files in lockstep. The eviction counter is
exposed on the wire (`ShaderCacheStats.evictionCount`, appended after
the original four fields; older daemons that don't emit it are
tolerated by the decoder).

### Asset

Domain-agnostic blob cache keyed by `(path: std::string, platform: uint8)`.

| Request | Response | Notes |
|---------|----------|-------|
| `GetAsset` (0x0001) | `GetAssetResponse` | miss returns `found=false` |
| `PutAsset` (0x0003) | `PutAssetResponse` | overwrites existing entry |
| `InvalidateAsset` (0x0005) | `InvalidateAssetResponse` | drops every platform variant for a path; returns removed count |
| `ClearCache` (0x0007) | `ClearCacheResponse` | drops all entries, resets stats |
| `GetCacheStats` (0x0009) | `GetCacheStatsResponse` | entry count, total bytes, hits, misses, evictions |

Optional persistence via `--asset-cache-dir <path>`. On disk each entry
is a file `<pathHash16>_<platform3>.asset` containing a `[u32 pathLen]
[pathBytes][blob]` layout so arbitrary-length paths round-trip through
the 255-byte NAME_MAX limit (tested with 500+ char paths).

LRU eviction is enabled with `--asset-cache-max-mb <N>` (same semantics
as the shader service) and `AssetCacheStats.evictionCount` is tracked
on the wire.

### Collaboration

`SparkCollabServer` deliberately runs outside `SparkDaemon`. Its collaboration
service uses capability tokens for session administration and peer operations,
maintains authoritative presence and node locks, keeps bounded edit history,
expires inactive peers, and releases their locks. The standalone binary uses
the same local IPC framing and Control service as the daemon.

## Running the daemon

```bash
# In-memory only (entries lost on restart):
./build/linux-gcc-release/bin/SparkDaemon

# Persistent shader + asset caches (typical dev setup):
./build/linux-gcc-release/bin/SparkDaemon \
    --socket /tmp/spark-daemon.sock \
    --cache-dir ~/.cache/spark/shaders \
    --asset-cache-dir ~/.cache/spark/assets
```

Options:

| Flag | Default | Purpose |
|------|---------|---------|
| `--socket <path>` | `./.spark-daemon.sock` | AF_UNIX socket path (perm 0600) |
| `--cache-dir <path>` | disabled | Shader cache directory — enables persistence |
| `--asset-cache-dir <path>` | disabled | Asset cache directory — enables persistence |
| `--shader-cache-max-mb <N>` | `0` (unbounded) | LRU eviction threshold for the shader service |
| `--asset-cache-max-mb <N>` | `0` (unbounded) | LRU eviction threshold for the asset service |
| `--help`, `-h` | — | Print usage |

The daemon writes a single-line startup banner on stdout:

```
SparkDaemon: listening on /tmp/spark-daemon.sock
SparkDaemon: shader cache /home/alice/.cache/spark/shaders (142 entries loaded)
SparkDaemon: asset cache /home/alice/.cache/spark/assets (891 entries loaded)
```

On `SIGINT` or `SIGTERM` (or a client's `Control::ShutdownRequest`) it
exits cleanly, unlinking the socket file.

## Enabling it from the engine

Two CVars control daemon use from the engine side:

| CVar | Default | Purpose |
|------|---------|---------|
| `spark.daemon.enabled` | false | Master switch — when false the engine never talks to a daemon |
| `spark.daemon.socket_path` | empty | Override socket path (empty = `./.spark-daemon.sock`) |
| `spark.daemon.auto_spawn` | false | If no daemon is running, launch one as a detached subprocess |
| `spark.daemon.binary_path` | empty | Override path to the `SparkDaemon` executable (empty = `./SparkDaemon`) |
| `spark.daemon.clear_on_startup` | false | After a successful connect, run `daemon.clear_cache all` — useful when reattaching a daemon that has stale cooked blobs from a previous source tree |

Typical power-user run:

```bash
# Terminal 1: start the daemon
./build/linux-gcc-release/bin/SparkDaemon \
    --cache-dir ~/.cache/spark/shaders

# Terminal 2: run the engine pointed at it
./build/linux-gcc-release/bin/SparkEngine \
    +spark.daemon.enabled 1
```

The engine calls `Spark::Daemon::InitializeDaemonLifecycle()` from
`InitConsole()` (see `SparkEngine.cpp`). That helper:

1. If `spark.daemon.enabled` is false, returns immediately.
2. Calls `DaemonConnection::TryConnect(socketPath)`. If successful,
   proceeds to step 4.
3. If `spark.daemon.auto_spawn` is true, launches `SparkDaemon` as a
   detached subprocess, waits up to 2 seconds for the socket to
   appear, then retries connect.
4. On success, builds a process-wide `ShaderServiceClient` and wires
   it into `GetShaderDiskCache()` via `SetDaemonClient(&client)`.
5. On failure, logs a warning and returns — engine continues with
   in-process caches only.

`Spark::Daemon::ShutdownDaemonLifecycle()` reverses all of that at
shutdown.

## Adding a new service

The pattern is established by `ShaderService` (Phase 2) and
`AssetService` (Phase 3a). To add a service:

1. Pick a new `ServiceId` value (extend the enum in `DaemonProtocol.h`).
2. Write `<Svc>ServiceProtocol.h` in `SparkEngine/Source/Utils/`:
   - Message enum (one value per request/response pair)
   - Request / response structs
   - `Encode*` / `Decode*` helpers built on `BinaryWriter` / `BinaryReader`
3. Write `<Svc>ServiceClient.{h,cpp}` in `SparkEngine/Source/Utils/`:
   - Constructor takes `DaemonClient&` (not owning)
   - One method per RPC, each returning `std::expected<Response, std::string>`
   - Each method encodes the request, calls `m_client.Request()`,
     decodes the response
4. Write `<Svc>Service.{h,cpp}` in `SparkDaemon/src/`:
   - Inherit from `ServiceBase`
   - `GetServiceId()` returns your ID
   - `HandleMessage(type, payload)` dispatches on message type, decodes
     payload, returns a `ServiceResponse`
   - On error, return a `ServiceResponse` with
     `messageType = ControlMessage::ErrorResponse` and a string payload
5. Register in `SparkDaemon/src/main.cpp` alongside `ControlService`.
6. Add a service-client test file under `Tests/`, following the existing
   fixture pattern (spin up a `DaemonServer` in a background thread,
   connect a `DaemonClient`, exercise the RPCs).
7. If the service persists to disk, add a `--<svc>-cache-dir` CLI flag
   to `SparkDaemon` and an `Initialize(path)` method that scans the
   directory on startup.
8. Wire into the engine via `DaemonLifecycle` when engine-side
   integration is needed (follow the `ShaderDiskCache` pattern for the
   typed subsystem → daemon wiring).

## Platform support

| Platform | Transport | Daemon binary | Client |
|----------|-----------|---------------|--------|
| Linux | AF_UNIX sockets | ✅ | ✅ |
| macOS | AF_UNIX sockets | ✅ (experimental, untested in CI) | ✅ |
| Windows | Named pipes | ✅ | ✅ |

Windows targets are built by `SparkDaemon/CMakeLists.txt`. The server uses
same-user local named pipes and the engine client connects through
`DaemonClient`; Unix platforms use owner-restricted domain sockets.

## Console commands

When the daemon is connected, three commands are registered on the
engine-side `CommandRegistry` and usable from the in-game console:

| Command | Purpose |
|---------|---------|
| `daemon.stats` | Formats the Control `StatsResponse` plus per-service `GetCacheStats` (shader + asset) into a single human-readable report — uptime, protocol version, registered service IDs, entry count / bytes / hit-rate / evictions for each cache. The formatter lives in `DaemonDiagnostics::FormatDaemonStats` and is unit-tested directly against a `DaemonStatsSnapshot` so the command shell has no test surface of its own. |
| `daemon.clear_cache <shader\|asset\|all>` | Wipes one or both caches via RPC. The bitmask parser (`DaemonCacheScope`) accepts the three literals and rejects everything else with a usage string. |
| `daemon.invalidate <path>` | Calls `AssetService::InvalidateAsset` for the supplied source path, dropping every platform variant in one RPC and reporting the removed count. |

All three degrade gracefully if the daemon is not connected — they
log a warning and do nothing, so scripts can fire them unconditionally
at startup.

## Ops and debugging

### Is the daemon running?

```bash
ls -l /tmp/spark-daemon.sock
# srw------- 1 alice alice 0 Apr 16 15:42 /tmp/spark-daemon.sock
```

Live socket = daemon is up. Missing socket = either it crashed or it
was never started.

### Is the engine using it?

From an engine console:

```
> spark.daemon.enabled
spark.daemon.enabled = true

> spark.daemon.socket_path
spark.daemon.socket_path =
```

Or inspect the log — `InitializeDaemonLifecycle` logs the outcome:

```
[Core] Daemon wired: shader cache sharing via ./.spark-daemon.sock
```

or

```
[Core] Daemon requested but unreachable — engine will run with in-process caches only
```

### What's the daemon doing?

Send a `StatsRequest` from any connected client:

```cpp
auto response = client.Request(ServiceId::Control,
    static_cast<uint16_t>(ControlMessage::StatsRequest), {});
DaemonStats stats;
DecodeDaemonStats(response->payload, stats);
// stats.uptimeSeconds
// stats.protocolVersion
// stats.registeredIds
```

Per-service stats come from each service's own `GetCacheStats` RPC —
see `ShaderServiceClient::GetCacheStats` and
`AssetServiceClient::GetCacheStats`.

### Shutting down cleanly

From a client:

```cpp
client.Request(ServiceId::Control,
    static_cast<uint16_t>(ControlMessage::ShutdownRequest), {});
```

From the shell: `kill -TERM $(pgrep -f SparkDaemon)`.

Both unlink the socket file on exit.

### When things go wrong

- **Engine logs "Daemon requested but unreachable":** socket path is
  wrong or daemon isn't running. Check `spark.daemon.socket_path` and
  `ls /tmp/spark-daemon.sock` (or whatever path is set).
- **Daemon launches but engine can't connect:** check the socket path
  matches on both sides. The daemon's CWD determines where
  `.spark-daemon.sock` lives if no `--socket` is passed.
- **Auto-spawn logs "binary not found":** set
  `spark.daemon.binary_path` to an absolute path, or run the engine
  with `SparkDaemon` in the CWD.
- **Stale cache entries after a source change:** the Asset service's
  `InvalidateAsset(path)` drops all platform variants in one RPC. The
  Shader service's `ClearCache` drops everything. File-watching
  auto-invalidation is a future phase.

## Implemented phases (recap)

Phase history is tracked in `.claude/knowledge/daemon-*-2026-04-16.md`
but summarised here for convenience:

| Phase | Scope | Status |
|-------|-------|--------|
| 1 | Protocol + framing + `DaemonClient` + `DaemonServer` + `ControlService` + `SparkDaemon` executable | ✅ |
| 2a | `ShaderService` (get / put / clear / stats) | ✅ |
| 2b | `ShaderService` disk persistence (`--cache-dir`, atomic rename writes, reload-on-restart) | ✅ |
| 3a | `AssetService` with `InvalidateAsset` | ✅ |
| 3b | Engine-side wiring: `ShaderDiskCache` consults daemon, falls through to local disk | ✅ |
| 3c | Engine lifecycle: `spark.daemon.enabled` CVar + `DaemonLifecycle::Initialize` called from `InitConsole` | ✅ |
| 4   | `spark.daemon.auto_spawn`, `Control::StatsRequest`, concurrent-clients test | ✅ |
| 5   | LRU eviction in both services + `--shader-cache-max-mb` / `--asset-cache-max-mb` CLI flags + `evictionCount` on the wire | ✅ |
| 6   | `daemon.stats` / `daemon.clear_cache` / `daemon.invalidate` console commands + `spark.daemon.clear_on_startup` | ✅ |

## Future phases

Still tracked in `.claude/knowledge/daemon-services-architecture-2026-04-16.md`:

- **Build monitor.** Watches CMake + source files, reports
  incremental rebuild events to editors.
- **File watching + push notifications.** inotify/FSEvents/kqueue
  abstraction so the daemon can push `ShaderReloaded` / `AssetChanged`
  events to connected clients instead of every client polling.
- **AssetPipeline engine wiring.** Bridge between the engine's typed
  `shared_ptr<Asset>` and the daemon's raw-blob Asset service.
- **Actual shader compilation inside the daemon.** Warm DXC /
  glslang / SPIRV-Cross workers, parallel variant compile, push
  notifications for completed batches.
