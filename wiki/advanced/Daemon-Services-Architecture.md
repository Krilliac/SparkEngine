# Daemon Services Architecture

> **Audience:** Programmers
>
> **Thread Context:** The daemon is a separate long-lived process. The server accepts multiple client connections and dispatches to per-service handlers; engine-side clients connect over the framed IPC transport. The engine falls back to in-process behavior when no daemon is running.
>
> **Platform/Backend Scope:** Cross-platform IPC — Unix domain sockets on Linux/macOS, named pipes on Windows.

## Overview

A **SparkDaemon** is a long-lived background process (inspired by Wine's `wineserver`) that outlives any single engine or editor instance. It owns shared mutable state — caches, file watches, session coordination — and serves multiple clients over a framed length-prefixed protocol. It exists because several engine subsystems currently keep per-process state that would be better shared: two editor instances can't share a compiled-asset cache, shader hot-reload polls timestamps on the main thread, and collaborative editing is P2P with no persistence.

Originally a plan, the daemon framework has since been implemented. The engine-side client and the standalone daemon both exist.

## Current Status (as of 2026-08-26)

**Overall: Implemented across Windows named pipes and Unix-domain sockets. Control, Asset, Shader, and optional Orchestration run in `SparkDaemon`; collaboration runs in the separately isolated `SparkCollabServer`. Build Monitor remains reserved.**

| Component | Plan | Status | Evidence |
|-----------|------|--------|----------|
| Daemon framework | Phase 1 | **Implemented** | `SparkDaemon/src/` has `DaemonServer.{h,cpp}`, `ServiceBase.h`, `main.cpp` |
| Wire protocol / framing | Phase 1 | **Implemented** | `SparkEngine/Source/Utils/DaemonProtocol.h`, `DaemonFraming.h` |
| Engine-side client | Phase 1 | **Implemented** | `DaemonClient.{h,cpp}`, `DaemonConnection.{h,cpp}`, `DaemonLifecycle.{h,cpp}`, `DaemonDiagnostics.{h,cpp}` |
| Control service | (new, not in original plan) | **Implemented** | `SparkDaemon/src/ControlService.{h,cpp}` — ping, shutdown, version, stats |
| Asset service | Phase 3 | **Implemented** | `SparkDaemon/src/AssetService.{h,cpp}` |
| Shader service | Phase 2 | **Implemented** | `SparkDaemon/src/ShaderService.{h,cpp}` |
| Collab broker | Phase 4 | **Implemented, isolated process** | `CollaborationService.{h,cpp}`, `CollaborationProtocol.h`, and `SparkCollabServer` provide token-authenticated sessions, presence, locks, and edit history |
| Build Monitor | (lightweight) | **Reserved, not built** | `ServiceId::Build = 0x0004` is allocated; no service implementation is present |
| Orchestration | external-process expansion | **Implemented, opt-in** | `OrchestrationService`, journal, identity, and `SparkOrchestrator`; registered only when explicitly configured |
| CMake wiring | — | **Wired** | `CMakeLists.txt:996` — `add_subdirectory(SparkDaemon)` guarded by `EXISTS SparkDaemon/CMakeLists.txt` |

### Service IDs (as implemented)

The implemented `ServiceId` enum (`DaemonProtocol.h:35`) differs from the plan's `0x01–0x04` numbering — a built-in Control service was added at `0x0000`:

```
Control = 0x0000   // built-in: ping, shutdown, version query
Asset   = 0x0001   // compiled-asset cache, cook pipeline
Shader  = 0x0002   // compile, disk cache, hot reload
Collab  = 0x0003   // collaborative editing broker (SparkCollabServer)
Build   = 0x0004   // reserved build-monitor protocol slot
Orchestration = 0x0005 // trusted opt-in process control plane
```

## Architecture

```
┌─────────────┐  ┌─────────────┐  ┌──────────────┐
│  Editor #1  │  │  Editor #2  │  │ CLI cook tool│
│  (client)   │  │  (client)   │  │  (client)    │
└──────┬──────┘  └──────┬──────┘  └──────┬───────┘
       │                │               │
       └────────┬───────┴───────┬───────┘
                │  socket/pipe  │
          ┌─────┴───────────────┴─────┐
          │       SparkDaemon         │
          │  ┌─────────┐ ┌─────────┐  │
          │  │ Control │ │ Asset   │  │
          │  │ Service │ │ Service │  │
          │  ├─────────┤ ├─────────┤  │
          │  │ Shader  │ │ Orchestr.│ │
          │  │ Service │ │ (opt-in) │  │
          │  └─────────┘ └─────────┘  │
          └───────────────────────────┘

          SparkCollabServer (separate trust boundary)
          └─ token-authenticated sessions, locks, presence, edit history
```

## Wire Protocol

Length-prefixed binary frames, reusing the serialization pattern from `CollaborativeEditSession`:

```
[4 bytes: payload length][2 bytes: service ID][2 bytes: message type][N bytes: payload]
```

Framing lives in `DaemonFraming.h`; message/service definitions in `DaemonProtocol.h`.

## Services

### Control (built-in)

Handles `PingRequest`, `ShutdownRequest`, version query, and `StatsRequest`. `DaemonServer` may share its stats/shutdown state with `ControlService` so a `ShutdownRequest` cleanly stops the server.

### Asset Service

Backend for the compiled-asset cache and cook pipeline (mesh compression, texture mip gen, audio transcode). Lets multiple editors share one warm cache and replaces per-process timestamp polling with file watchers. Client requests: `GetAsset`, `CookAsset`, `WatchDirectory`, `GetCacheStats`, `InvalidateCache`.

### Shader Service

Owns warm compiler state, a persistent shader disk cache, and a thread pool for parallel variant compilation. `ShaderHotReload` becomes a thin client receiving push events instead of polling. Client requests: `CompileShader`, `CompileBatch`, `WatchShaders`, `GetCacheEntry`, `WarmCache`.

### Collaborative Editing Broker

`SparkCollabServer` hosts `CollaborationService` in a separate process because collaborative traffic has a different trust class from local cache and orchestration traffic. It provides capability-token-authenticated session creation/join/delete, presence, node locks, bounded operation history, snapshots, peer expiry, and lock cleanup. The legacy editor P2P path remains a distinct compatibility surface; it is not the daemon broker.

### Build Monitor (not built)

Planned lightweight service to watch `CMakeLists.txt`/source files and stream `BuildStatus` events. The service ID is reserved, but no backend is present.

## Existing Code It Builds On

| Component | Role |
|-----------|------|
| `AssetPipeline` (`Graphics/AssetPipeline.h`) | Asset service backend |
| `SparkShaderCompiler/src/` | Shader service backend |
| `ShaderHotReload` (`Graphics/ShaderHotReload.h`) | Becomes a daemon client |
| `CollaborationService` / `CollaborationProtocol` | Standalone authoritative collaboration broker |
| `ConsoleProcessManager` (`Utils/`) | Pattern for daemon launch |
| `Process::Builder` (`Utils/Process.h`) | Daemon process launcher (`Detached()`) |
| `VirtualFileSystem` (`Engine/`) | Gets a `DaemonProvider` mount |

## Fallback Behavior

Each service is independently shippable, and the engine falls back to in-process behavior when no daemon is running — so adding a service never breaks the no-daemon path.

## Source & Freshness

- **Original entry date:** 2026-04-16 (`daemon-services-architecture-2026-04-16.md`, type: Plan)
- **Verified against codebase 2026-08-26.**
- Status bullets:
  - **Framework Implemented** — `SparkDaemon/src/` (DaemonServer, ServiceBase, main) plus engine-side `DaemonClient/Connection/Protocol/Framing/Lifecycle/Diagnostics` under `SparkEngine/Source/Utils/`.
  - **Control / Asset / Shader services exist**; a built-in Control service (`0x0000`) was added that wasn't in the original plan.
  - **Service-ID numbering changed** from the plan's `0x01–0x04` to `Control=0x0000, Asset=0x0001, Shader=0x0002, Collab=0x0003`.
  - **Collaboration is implemented in isolated `SparkCollabServer`**; **Build Monitor remains reserved/not built**.
  - Windows named-pipe and Unix-domain-socket transports are both implemented.
  - CMake wiring confirmed at `CMakeLists.txt:996`.

## Related Pages

- [SparkBuild In-Tree](SparkBuild-In-Tree.md) — sibling developer tooling
- [Wine Role and Fallback Tiers](Wine-Role-and-Fallback-Tiers.md) — the `wineserver` pattern that inspired this
