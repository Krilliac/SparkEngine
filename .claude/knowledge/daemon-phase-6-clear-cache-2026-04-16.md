# SparkDaemon Phase 6 Follow-up — `daemon.clear_cache` Command + AssetService ClearCache RPC

**Last updated:** 2026-04-16
**Type:** Observation
**Status:** Active
**Cross-references:** `daemon-phase-6-diagnostics-command-2026-04-16.md`

## TL;DR

Added a `daemon.clear_cache <shader|asset|all>` InGameConsole command
that asks the daemon to drop entries from its caches. Required adding
a `ClearCache` RPC to the asset service (the shader service already
had one since Phase 2a). Fully symmetrical with shader: in-memory map
wiped, stats zeroed, disk files removed when disk-backed. 9 new
tests (3 formatter parser, 3 asset-service wire, 3 end-to-end),
5584/5584 suite.

## Wire addition: `AssetMessage::ClearCacheRequest`

```cpp
enum class AssetMessage : uint16_t
{
    …
    GetCacheStatsRequest   = 0x0007,
    GetCacheStatsResponse  = 0x0008,
    ClearCacheRequest      = 0x0009, // NEW
    ClearCacheResponse     = 0x000A, // NEW
};
```

Empty payloads both directions. `AssetServiceClient::ClearCache()`
returns `std::expected<void, std::string>` — same idiom as the
shader counterpart. `HandleClearCache()` in the service mirrors the
shader implementation bit-for-bit: snapshot `m_diskBacked` under the
mutex, clear the LRU + index + size counter under the mutex, zero the
atomic stats (hit / miss / eviction), call `DeleteAllBlobFiles()`
outside the mutex if disk-backed.

New `DeleteAllBlobFiles()` helper on `AssetService` iterates the
cache dir and removes every `.asset` file (best-effort — individual
`remove` failures are ignored because a daemon shouldn't crash just
because a file was locked by an antivirus scan).

## Command handler

```
daemon.clear_cache shader   → { ClearCache on shader }
daemon.clear_cache asset    → { ClearCache on asset }
daemon.clear_cache all      → { ClearCache on shader, then asset }
daemon.clear_cache <other>  → usage hint
daemon.clear_cache          → usage hint
```

Success output: `daemon.clear_cache: cleared [shader, asset]`
Partial failure: `daemon.clear_cache: cleared [shader] failed [asset]`
Disconnected: `daemon: not connected` (matches `daemon.stats`)

The handler:
1. Parses arg 0 with `ParseDaemonCacheScope()` (pure helper in
   `DaemonDiagnostics`). Anything other than `shader` / `asset` / `all`
   yields `None`, which re-renders the usage hint.
2. If `(scope & Shader) != 0`, calls `ShaderServiceClient::ClearCache()`
   through either the lifecycle-owned `g_shaderClient` or a stack-local
   one-shot (same pattern as `daemon.stats`).
3. Same for asset.
4. Accumulates `cleared` / `failed` labels and formats a one-line
   summary.

## New components

| File / symbol | Role |
|---|---|
| `AssetMessage::ClearCacheRequest / Response` | New wire messages (0x0009 / 0x000A) |
| `AssetService::HandleClearCache`, `DeleteAllBlobFiles` | Service-side implementation |
| `AssetServiceClient::ClearCache` | Typed RPC wrapper |
| `DaemonCacheScope` + `ParseDaemonCacheScope` (`DaemonDiagnostics.h`) | Pure parser — `"shader"` / `"asset"` / `"all"` to bitmask |
| `RunClearCacheCommand` (anon in `DaemonLifecycle.cpp`) | Dispatches the RPC fan-out for the selected scopes |
| `daemon.clear_cache` InGameConsole command | User surface |

## Design decisions

### 1. Scope bitmask instead of enum-per-target

`DaemonCacheScope : uint8_t { None, Shader=1, Asset=2, All=3 }` —
the parser returns `Shader | Asset` for `"all"` and the handler does
`(scope & Shader) != 0`. Lets future services (`Collab`, `Build`)
extend the enum without reworking the dispatch, and the test
`DaemonDiagnostics_CacheScopeAllIsBitwiseUnion` pins the bit math
so a future enum rearrangement can't silently break the OR-union.

### 2. Same "always register the command" invariant as `daemon.stats`

`EnsureClearCacheCommandRegistered()` runs inside
`InitializeDaemonLifecycle` unconditionally, before any CVar check.
If the daemon is disabled or unreachable, the handler itself prints
`daemon: not connected`. Operators always have a way to discover
what's wrong without also memorising how to toggle the CVar.

### 3. Partial-failure reporting instead of all-or-nothing

If `clear_cache all` succeeds on shader but times out on asset, the
summary is `cleared [shader] failed [asset]`. No exceptions, no
hidden retries — the operator sees exactly what happened. The
shader-first order is not load-bearing; it just keeps output stable
for docs and tests.

### 4. Stats zeroed alongside the data

`ClearCache` also zeros `hitCount` / `missCount` / `evictionCount`.
This matches shader behaviour (established back in Phase 2a) and it's
what operators expect: "clear the cache" should also reset the rate
counters, otherwise the next `daemon.stats` misleadingly shows a
100% hit rate against an empty cache.

## Test coverage

### Pure formatter / parser (DaemonDiagnostics)
| Test | Verifies |
|---|---|
| `DaemonDiagnostics_ParseCacheScopeKnownValues` | `shader` / `asset` / `all` map to the expected enum values |
| `DaemonDiagnostics_ParseCacheScopeInvalidReturnsNone` | Empty arg, wrong case (`Shader`), plural (`assets`), unknown token → `None` |
| `DaemonDiagnostics_CacheScopeAllIsBitwiseUnion` | `All == Shader \| Asset`, `Shader & Asset == 0` |

### AssetService wire-level (live daemon + client)
| Test | Verifies |
|---|---|
| `AssetService_ClearCacheDropsAllEntriesAndZerosStats` | Two entries + hits → `ClearCache` → stats all zero, entries gone |
| `AssetService_ClearCacheRemovesDiskFiles` | Disk-backed cache has `.asset` files pre-clear, zero post-clear |
| `AssetService_ClearCacheOnEmptyCacheIsNoOp` | Clear on empty cache succeeds without error |

Full suite 5584 passed / 0 failed (1 pre-existing flaky warned),
123796/123796 assertions.

## Follow-up candidates

1. **`daemon.invalidate <path>`** companion command that fronts
   `AssetServiceClient::InvalidateAsset(path)` — clean one-liner
   using the same dispatch pattern.
2. **Editor HUD button** that calls `RunClearCacheCommand` directly
   — already testable against the existing RPC layer.
3. **`spark.daemon.clear_on_startup` CVar** — auto-clear the cache
   when the engine connects. Relevant when a developer's local
   shader source has diverged from whatever the shared daemon
   remembers.
