# SparkEngine Daemon Services — Architecture Plan

**Last updated:** 2026-04-16
**Type:** Plan
**Status:** Active
**Cross-references:** `codebase-observations.md`, `workflow-patterns.md`

## TL;DR

A **SparkDaemon** is a long-lived background process (inspired by Wine's
wineserver) that outlives any single engine/editor instance. It owns shared
mutable state — caches, file watches, session coordination — and serves
multiple clients over Unix domain sockets (Linux/macOS) or named pipes
(Windows). Four services: Asset, Shader, Collab Broker, Build Monitor.

## Motivation

Wine's wineserver exists because Windows kernel semantics (handles, registry,
kernel objects) have no POSIX equivalent. SparkEngine doesn't need that, but
the **daemon pattern** — a single process serializing shared state for
multiple clients — solves real problems:

- Two editor instances can't share compiled assets (each has its own in-process `AssetCache`)
- `ShaderHotReload` polls timestamps on the main thread; variant compilation is serial
- `CollaborativeEditSession` is P2P with no persistence — host crash loses all state
- No way to share a warm shader cache across editor + headless cook + game preview

## Architecture

```
┌─────────────┐  ┌─────────────┐  ┌──────────────┐
│  Editor #1  │  │  Editor #2  │  │ CLI cook tool│
│  (client)   │  │  (client)   │  │  (client)    │
└──────┬──────┘  └──────┬──────┘  └──────┬───────┘
       │                │               │
       └────────┬───────┴───────┬───────┘
                │  Unix socket  │
          ┌─────┴───────────────┴─────┐
          │       SparkDaemon         │
          │  ┌─────────┐ ┌─────────┐  │
          │  │ Asset    │ │ Shader  │  │
          │  │ Service  │ │ Service │  │
          │  ├─────────┤ ├─────────┤  │
          │  │ Collab   │ │ Build   │  │
          │  │ Service  │ │ Monitor │  │
          │  └─────────┘ └─────────┘  │
          └───────────────────────────┘
```

## Wire Protocol

Length-prefixed binary frames over the socket, reusing the serialization
pattern from `CollaborativeEditSession`:

```
[4 bytes: payload length][2 bytes: service ID][2 bytes: message type][N bytes: payload]
```

Service IDs: `0x01` = Asset, `0x02` = Shader, `0x03` = Collab, `0x04` = BuildMonitor

## Service 1: Asset Daemon

**Problem:** `AssetPipeline` runs in-process. Two editor instances can't
share compiled assets. Hot-reload watches are per-process.

**What the daemon owns:**
- Compiled asset cache (currently `AssetCache` with 512MB LRU)
- File watchers for source assets (replaces per-process timestamp polling)
- Import/cook pipeline (mesh compression, texture mip gen, audio transcode)

**Client API:**

| Request | Response |
|---------|----------|
| `GetAsset(path, platform)` | Compiled blob or `Cooking` status |
| `CookAsset(path, platform, force)` | Job ID |
| `WatchDirectory(path)` | Stream of `AssetChanged` notifications |
| `GetCacheStats()` | Hit rate, size, eviction count |
| `InvalidateCache(path)` | Ack |

**Engine-side changes:**
- `AssetPipeline::LoadAssetAsync` becomes a daemon RPC
- VFS gets a `DaemonProvider` mount for cache access
- Multiple editors share one warm cache

## Service 2: Shader Compilation Daemon

**Problem:** `SparkShaderCompiler` is a cold-start CLI tool.
`ShaderHotReload` polls timestamps on the main thread. Variant explosion
(N materials × M backends × K quality) is serial.

**What the daemon owns:**
- Warm compiler state (DXC/glslang/SPIRV-Cross loaded once)
- `ShaderDiskCache` (persistent blob store)
- File watchers for `.hlsl`/`.glsl` source files
- Thread pool for parallel variant compilation

**Client API:**

| Request | Response |
|---------|----------|
| `CompileShader(source, stage, backend, defines)` | `ShaderCompileResult` |
| `CompileBatch(requests[])` | Stream of results |
| `WatchShaders(directory)` | Push `ShaderReloaded` events |
| `GetCacheEntry(sourceHash, target)` | Cached blob or miss |
| `WarmCache(shaderList, backends[])` | Background job + push notifications |

**Engine-side changes:**
- `ShaderHotReload::Update()` receives push events (no polling)
- `Shader::Initialize()` does daemon RPC for cache lookup
- Editor fires `WarmCache` on project open

## Service 3: Collaborative Editing Broker

**Problem:** `CollaborativeEditSession` is P2P TCP. No persistence — host
crash loses state. Every editor needs the full conflict resolution stack.

**What the daemon owns:**
- Authoritative scene state (server in client-server)
- Lock table (replaces per-peer pessimistic locking)
- Operation history (undo across editors, crash recovery)
- Presence tracking

**Client API:**

| Request | Response |
|---------|----------|
| `JoinSession(userName)` | State snapshot + peer list |
| `AcquireLock(nodeId)` | Granted / Denied |
| `SubmitEdit(EditMessage)` | Ack + sequence number |
| `Subscribe(nodeFilter)` | Stream of `EditBroadcast` events |
| `GetHistory(since)` | Edit ops since sequence N |

**Engine-side changes:**
- `CollaborativeEditSession::Host()` launches daemon service
- Lock arbitration is instant (single process, no network RTT)
- Session survives editor crashes — reconnect and resume

## Service 4: Build Monitor (lightweight)

Watches `CMakeLists.txt` and source files, triggers incremental rebuilds,
reports errors to connected editors in real-time.

**Client API:** `WatchBuild(preset)` → stream of `BuildStatus` events

## Implementation Phases

### Phase 1 — Foundation (daemon framework)
- `SparkEngine/Source/Utils/DaemonClient.{h,cpp}` — connect, send, receive, reconnect
- `SparkDaemon/src/DaemonServer.{h,cpp}` — accept connections, dispatch to services
- `SparkEngine/Source/Utils/DaemonProtocol.h` — wire format (shared)
- Launch via `Process::Builder::Detached()`, discover via `build/.spark-daemon.sock`

### Phase 2 — Shader service
- Move `ShaderDiskCache` + `CompileShader()` into daemon
- `ShaderHotReload` becomes thin client receiving push events
- `Shader::Initialize()` does daemon RPC for cache lookup

### Phase 3 — Asset service
- Move `AssetCache` + cook pipeline into daemon
- `AssetPipeline::LoadAssetAsync()` becomes daemon RPC
- VFS gets `DaemonProvider` mount

### Phase 4 — Collab broker
- Refactor `CollaborativeEditSession` from P2P to client-daemon
- Add operation history and crash recovery

Each phase is independently shippable — the engine falls back to in-process
behavior when no daemon is running.

## Existing Code to Build On

| Component | Current State | Daemon Role |
|-----------|--------------|-------------|
| `AssetPipeline` (`Graphics/AssetPipeline.h`) | In-process queue + worker threads | Asset service backend |
| `SparkShaderCompiler` (`SparkShaderCompiler/src/`) | Offline CLI tool | Shader service backend |
| `ShaderHotReload` (`Graphics/ShaderHotReload.h`) | Main-thread timestamp polling | Becomes daemon client |
| `ShaderDiskCache` (`Graphics/ShaderDiskCache.h`) | Per-process binary cache | Moves into shader daemon |
| `CollaborativeEditSession` (`Communication/`) | P2P TCP, pessimistic locking | Collab service backend |
| `ConsoleProcessManager` (`Utils/`) | Subprocess pipe IPC | Pattern for daemon launch |
| `Process::Builder` (`Utils/Process.h`) | RAII subprocess with Detached() | Daemon process launcher |
| `VirtualFileSystem` (`Engine/`) | Mount-priority layered FS | Gets DaemonProvider mount |

## IPC Foundation

Currently no Unix domain socket or named pipe code in the codebase.
`NetworkManager` is UDP-only for remote networking. New IPC layer needed.

`ConsoleProcessManager` pattern (pipe-based subprocess) can bootstrap the
daemon launch. The daemon itself should listen on a Unix domain socket
(`AF_UNIX`) for persistent, multiplexed client connections — pipes are
one-to-one and can't serve multiple clients.
