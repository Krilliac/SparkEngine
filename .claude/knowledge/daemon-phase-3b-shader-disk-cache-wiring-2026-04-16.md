# SparkDaemon Phase 3b — Engine-side Wiring: ShaderDiskCache ↔ Daemon

**Last updated:** 2026-04-16
**Type:** Observation
**Status:** Active
**Cross-references:** `daemon-phase-2a-shader-service-2026-04-16.md`, `daemon-phase-2b-shader-persistence-2026-04-16.md`, `daemon-phase-3a-asset-service-2026-04-16.md`

## TL;DR

First engine-side integration of the daemon. `ShaderDiskCache::Lookup` now
consults an optional `ShaderServiceClient` before reading local disk;
`Store` writes locally and pushes to the daemon in parallel. Two engine
instances pointed at the same daemon share a warm shader cache
cross-process. Opt-in and backwards-compatible — unwired caches behave
exactly as before. 8 new tests, full suite 5541 / 5541.

## What's wired

```
┌─────────────────────────┐
│  Engine A (editor)      │
│    ShaderDiskCache      │
│         │ GetCacheEntry │
│         │ PutCacheEntry │
│         ▼               │
│   ShaderServiceClient   │
│         │               │
│   DaemonConnection──┐   │
└─────────────────────┤   │
                      ▼
                  ┌────────────┐
                  │ SparkDaemon│
                  │ ShaderSvc  │
                  └─────┬──────┘
                        ▲
                        │
┌───────────────────────┤
│  Engine B (CLI tool)  │
│    ShaderDiskCache    │
│    ShaderServiceClient│
│    DaemonConnection───┘
└───────────────────────┘
```

Both engine instances point `ShaderDiskCache::SetDaemonClient` at their
own `ShaderServiceClient` wrapping the same daemon. A Store on engine A
lands in the daemon; engine B's next Lookup finds it there without
needing its own local disk copy.

## New components

| File | Role |
|---|---|
| `SparkEngine/Source/Utils/DaemonConnection.{h,cpp}` | Process-wide singleton that owns a `DaemonClient`. `TryConnect(socketPath)` probes `stat()` before attempting `connect()` so missing daemons are cheap to detect. Idempotent — calling `TryConnect` while already connected is a no-op |
| `SparkEngine/Source/Graphics/ShaderDaemonBridge.{h,cpp}` | `EncodeCompiledShaderBlob` / `DecodeCompiledShaderBlob` helpers. The daemon stores opaque bytes; this layer lives next to `ShaderDiskCache` and serialises the full `CompiledShaderBlob` struct (bytecode + entry point + errors + reflection counters) with a version byte (`kShaderDaemonBlobVersion = 1`) so newer daemons with older engines miss cleanly instead of decoding garbage |

## Modified components

- `ShaderDiskCache` — added optional `Spark::Daemon::ShaderServiceClient*`
  pointer plus `SetDaemonClient`, `GetDaemonHits`, `GetDaemonMisses`. Forward-declared
  in the header so most translation units don't pull in daemon types.
- `ShaderDiskCache::Lookup` — consults daemon first if wired; on hit,
  decodes full blob via `ShaderDaemonBridge` and returns; on miss OR
  decode failure OR transport error, falls through to local disk. The
  client pointer is snapshotted under the mutex and then used unlocked
  — holding the cache mutex across a network round-trip would serialise
  local disk ops behind the slowest daemon call.
- `ShaderDiskCache::Store` — writes to local disk under the mutex
  (unchanged), then outside the mutex pushes the full blob to the
  daemon. Transport errors are swallowed: local disk is authoritative.

## Design decisions

### 1. Keep `ShaderDiskCache` bytecode-only on local disk

Local on-disk format is unchanged — `<hash>_<target>_<stage>.blob`
containing raw bytecode. Only the daemon path carries the full
`CompiledShaderBlob` (with reflection metadata). This preserves
backwards compatibility with every existing cached shader file and
keeps the local-disk round-trip cheap. When we transition fully to
daemon-primary we can rethink the local format, but not this slice.

### 2. Dual-write, daemon-eventual

`Store` writes local first, then daemon. If the daemon push fails
silently (no daemon running, network hiccup), local cache is
unaffected. The invariant: **the local disk always reflects what the
engine just compiled.** The daemon is a speed optimisation layered
on top, never the source of truth.

### 3. Pointer, not `shared_ptr`

`ShaderServiceClient` has reference semantics — one per daemon
connection, held by the singleton `DaemonConnection`. Letting
`ShaderDiskCache` hold a raw pointer keeps ownership explicit:
whoever configures the cache is responsible for clearing the pointer
(`SetDaemonClient(nullptr)`) before tearing down the client. Matches
the engine's existing "non-owning raw pointer" convention per
CLAUDE.md.

### 4. `DaemonConnection` as a singleton, not injected

The engine has many subsystems that'll eventually talk to the daemon
(ShaderDiskCache, AssetCache, CollabSession, ...). Threading
`DaemonClient&` through the entire `EngineContext` / subsystem graph
would be intrusive; a process-wide accessor with `Instance()` is
cheap, idiomatic for this codebase (matches `GetShaderDiskCache()`,
`GetConsoleProcessManagerInstance()`), and leaves subsystems free to
query on demand.

## Test coverage added

| Test | Verifies |
|---|---|
| `ShaderDaemonBridge_EncodeDecodeRoundTrip` | Full `CompiledShaderBlob` struct round-trips through bytes (bytecode + metadata + reflection counters + errors string) |
| `ShaderDaemonBridge_RejectsUnknownVersion` | Decoder rejects payloads with `version != 1` |
| `ShaderDiskCache_StoreAlsoPushesToDaemon` | One cache stores → a **second cache** (empty local disk) gets the same blob via daemon hit, metadata intact |
| `ShaderDiskCache_DaemonMissFallsThroughToLocalDisk` | Daemon reports miss → local disk read succeeds; `daemonMisses` counter bumps |
| `ShaderDiskCache_NoDaemonBehavesAsBefore` | Without `SetDaemonClient`, `daemonHits`/`daemonMisses` both stay zero and everything works |
| `DaemonConnection_TryConnectReturnsFalseWhenSocketMissing` | Missing socket file is a cheap, non-error failure |
| `DaemonConnection_TryConnectSucceedsOnLiveDaemon` | Live daemon → connection holds, `Ping()` through the shared client works |
| `DaemonConnection_TryConnectIsIdempotent` | Second `TryConnect` call with an already-connected state returns true without disturbing the existing connection |

## What's deliberately not in this phase

- **Engine startup call** — `DaemonConnection::TryConnect` is not yet
  called from `SparkEngine.cpp` or `EngineContext::Initialize`. Every
  subsystem stays opt-in until someone decides where in the startup
  path to probe for the daemon. (Reasonable candidates: after
  `EngineContext::Initialize`, guarded by a CVar or command-line flag.)
- **Asset cache integration** — `AssetServiceClient` exists and works
  (Phase 3a) but `AssetCache` doesn't yet call it. Same pattern as this
  phase will apply when we wire it in.
- **Hot-reload push events** — the original plan calls for the daemon
  to push `ShaderReloaded` events when `.hlsl` files change. That
  requires file watching on the daemon side (inotify / FSEvents /
  kqueue) and a streaming RPC pattern beyond the current
  request / response model.

## Follow-up candidates

1. Wire `DaemonConnection::TryConnect` into engine startup behind a
   CVar (e.g. `spark.daemon.enabled`), with `Shutdown()` on exit.
2. Mirror this phase for `AssetCache` → `AssetServiceClient`.
3. Add an "approximate hit rate" reporting path so editors can display
   "shader cache: 95% hit via daemon" in the status bar.
4. Phase 4 (Collab broker) — completely different shape, exercises
   the daemon framework's streaming / long-lived session semantics
   that the cache services don't use.
