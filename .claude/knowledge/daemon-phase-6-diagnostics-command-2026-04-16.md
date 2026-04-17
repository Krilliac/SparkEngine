# SparkDaemon Phase 6 — `daemon.stats` Console Command + Diagnostics Formatter

**Last updated:** 2026-04-16
**Type:** Observation
**Status:** Active
**Cross-references:** `daemon-phase-3b-shader-disk-cache-wiring-2026-04-16.md`, `daemon-phase-3c-lifecycle-wiring-2026-04-16.md`, `daemon-phase-5-eviction-count-wire-2026-04-16.md`

## TL;DR

The engine now exposes a `daemon.stats` InGameConsole command that
queries the daemon's Control / Shader / Asset services and renders a
human-readable multi-line summary. A pure `FormatDaemonStats`
formatter in `Utils/DaemonDiagnostics.{h,cpp}` does the rendering so
the output is unit-testable without a live daemon. `DaemonLifecycle`
now also constructs an `AssetServiceClient` alongside the existing
`ShaderServiceClient`. 8 new tests, full suite 5578/5578.

## Why this slice

Until now the only way to know the daemon was reachable was to read
the engine log line at startup. Phase 5 added `evictionCount` to the
wire, but nothing displayed any of the cache stats. Operators
running with `spark.daemon.enabled 1` had no way to check "is the
cache actually doing anything useful" without attaching a debugger.

This phase closes that loop: one command, one round-trip per service,
all the counters laid out in a shape an editor HUD or a CI log grep
can consume.

## New components

| File | Role |
|---|---|
| `SparkEngine/Source/Utils/DaemonDiagnostics.h` | `DaemonStatsSnapshot` struct + `FormatDaemonStats()` declaration + `ServiceIdName()` |
| `SparkEngine/Source/Utils/DaemonDiagnostics.cpp` | Byte / uptime / hit-rate rendering; snapshot → multi-line string |
| `Tests/TestDaemonDiagnostics.cpp` | 8 unit tests for the formatter (pure input → string) |

## Modified components

- `DaemonLifecycle.cpp` — new `g_assetClient` (mirrors `g_shaderClient`);
  new `EnsureStatsCommandRegistered` / `UnregisterStatsCommand` helpers;
  `Initialize` / `Shutdown` register/unregister the command
  idempotently; a new `RunStatsCommand` helper does the per-service
  RPC fan-out and calls the pure formatter.

## Example output

```
daemon: connected
  socket:   /tmp/spark.sock
  version:  1.0.0
  uptime:   2.3h
  services: control, shader, asset

shader cache:
  entries:   142
  bytes:     3.2 MiB
  hit/miss:  981/73 (93.1%)
  evictions: 5

asset cache:
  entries:   64
  bytes:     128.0 MiB
  hit/miss:  233/12 (95.1%)
  evictions: 0
```

When disconnected, the whole output collapses to a single line:

```
daemon: not connected
```

## Design decisions

### 1. Register the command regardless of connection state

`EnsureStatsCommandRegistered` runs unconditionally inside
`InitializeDaemonLifecycle`, before the early-return on the CVar
check. The handler itself detects "not connected" and renders an
appropriate message. This way:

- Operators can always type `daemon.stats` to find out why it's not
  working (rather than the command silently not existing).
- Flipping the CVar at runtime and calling `Initialize` again doesn't
  create duplicate registrations (idempotent via `g_statsCommandRegistered`).

### 2. Pure formatter, independently testable

`FormatDaemonStats(const DaemonStatsSnapshot&)` has no access to
sockets, cvars, singletons, or system clock. Tests construct the
snapshot struct directly and assert on the exact rendered text —
234us for 8 tests. If we ever decide to expose the snapshot as a
wire-format for editor HUDs or CI tooling, the struct is already the
right shape.

### 3. Lazy one-shot clients for cache stats

If `g_shaderClient` / `g_assetClient` aren't up (e.g. lifecycle not
yet initialized when command fires), `RunStatsCommand` constructs a
one-shot client on the stack. Cheap: they're ~1 pointer wrappers.
Avoids a null-pointer branch in the hot path without requiring the
lifecycle to be fully wired before the command works.

### 4. `hasShader` / `hasAsset` check reads the StatsResponse

Which services to poll is driven by what the daemon's own
`DaemonStats.registeredIds` reports, not by "I constructed a client".
If a future daemon runs without the Asset service (e.g. a minimal
shader-only build), the command silently skips the asset section
instead of timing out on a request to an unregistered service.

### 5. Snapshot-first API

`RunStatsCommand` populates a local `DaemonStatsSnapshot` and then
calls `FormatDaemonStats(snap)`. Separating the "collect" and
"render" phases makes the command trivial to refactor into a typed
HUD later — the data shape is already plucked out of the wire.

## Test coverage

Pure-formatter tests (no daemon, no connection):

| Test | Verifies |
|---|---|
| `DaemonDiagnostics_DisconnectedRendersSingleLine` | Empty `socketPath` → `"daemon: not connected"`, nothing else |
| `DaemonDiagnostics_ConnectedRendersHeader` | Header renders socket / version / uptime / services list |
| `DaemonDiagnostics_UptimeFormatsAcrossRanges` | 30s / 120s / 7200s / 172800s render as `s` / `m` / `h` / `d` |
| `DaemonDiagnostics_ShaderCacheRendersCounts` | Entry/byte/hit-rate/eviction lines render in expected shape; asset section hidden when optional unset |
| `DaemonDiagnostics_BothCachesBothRender` | Both sections appear; byte suffix drops to `B` below 1 KiB; zero-traffic hit rate shows `—` |
| `DaemonDiagnostics_EmptyRegisteredServicesListRendersNone` | No registered services → `"services: (none)"` |
| `DaemonDiagnostics_UnknownServiceIdRendersHex` | Unknown `ServiceId` renders as `"unknown(0xNNNN)"` |
| `DaemonDiagnostics_ServiceIdNameMapping` | All known enumerators map to their lowercase name |

Not currently covered (deferred):

- End-to-end integration test for `daemon.stats` fired through the
  InGameConsole command dispatcher. That needs the full lifecycle
  spun up with a live daemon; `TestDaemonLifecycle.cpp` has the
  scaffolding for it and is the right home for a follow-up.
- Rendering numeric values (uptime 86400+, byte 1 TiB, etc.) —
  formatter handles them, tests just sample one per branch.

## Follow-up candidates

1. **Editor HUD panel** that calls `FormatDaemonStats` every second
   (or exposes `DaemonStatsSnapshot` for a structured widget).
2. **`daemon.clear_cache {shader,asset}`** companion command — the
   plumbing (`ClearCacheRequest` on both services) already exists.
3. **Collab + Build service support** — when those services land,
   the formatter gets two more `optional<...CacheStats>` fields and
   the snapshot loop picks them up automatically.
