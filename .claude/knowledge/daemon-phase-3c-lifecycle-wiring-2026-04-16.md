# SparkDaemon Phase 3c — Engine-Level Lifecycle Wiring

**Last updated:** 2026-04-16
**Type:** Observation
**Status:** Active
**Cross-references:** `daemon-phase-3b-shader-disk-cache-wiring-2026-04-16.md`

## TL;DR

Engine now connects to the daemon on startup when `spark.daemon.enabled=1`
and automatically wires the `ShaderDiskCache` into the daemon's
`ShaderService`. Phase 3b's plumbing is finally exercised by the real
engine boot path. 5 new tests, full suite 5545/5546.

## What changed

| File | Change |
|---|---|
| `SparkEngine/Source/Utils/DaemonLifecycle.{h,cpp}` | New — two free functions `InitializeDaemonLifecycle` / `ShutdownDaemonLifecycle` that probe the CVar, try-connect, attach `ShaderServiceClient` to the global `ShaderDiskCache` |
| `SparkEngine/Source/Core/SparkEngine.cpp` | Call `InitializeDaemonLifecycle` after `InitGameplaySystems` in `InitConsole()`; call `ShutdownDaemonLifecycle` in `ShutdownEngine` before `ShutdownGameplaySystems` |
| `Tests/TestDaemonLifecycle.cpp` | 5 tests — disabled-by-default noop, missing-socket noop, attach-when-available, idempotent Init, idempotent Shutdown |

## CVars

```
spark.daemon.enabled     (bool,   default false)  Connect to a running SparkDaemon on startup
spark.daemon.socket_path (string, default "")      Override socket path (empty → ./.spark-daemon.sock)
```

Both are `CVarFlags::Save` so they persist across engine runs.

## Integration point

Exactly one call site for init, one for shutdown — no abstraction layer:

```cpp
// InitConsole (after InitGameplaySystems)
Spark::Daemon::InitializeDaemonLifecycle();

// ShutdownEngine (before ShutdownGameplaySystems)
Spark::Daemon::ShutdownDaemonLifecycle();
```

The helper is file-scope `static unique_ptr<ShaderServiceClient>` plus a
guard bool under a mutex. No class, no extra header beyond the two free
functions.

## Design decisions

### CVar default: off, not on

The daemon is a power-user optimisation. Turning it on by default would
silently change the caching semantics for every engine instance — if
someone's running the daemon with a stale cache, the engine would
suddenly return stale bytecode. Off-by-default means the existing
in-process behaviour is preserved until an operator explicitly flips
the CVar.

### Clear cache pointer before destroying the client

`ShutdownDaemonLifecycle` calls `SetDaemonClient(nullptr)` **before**
`unique_ptr.reset()`. If the order were reversed, another thread
mid-`Lookup` could hold a dangling `ShaderServiceClient*`. Matches
the ordering rule in `ShaderDiskCache`'s header comment.

### Init is idempotent via a `g_active` guard

Test `DaemonLifecycle_InitializeIsIdempotent` — second call returns
early without creating a second `ShaderServiceClient` or reconnecting
the `DaemonConnection`. Important because the boot path could be
re-entered in some test harnesses.

### Shutdown is idempotent too

Safe to call `ShutdownDaemonLifecycle` from a destructor or teardown
path even if `InitializeDaemonLifecycle` was never called. `g_active`
short-circuits the whole function to a no-op in that case.

## Running it end-to-end

```
# Terminal 1: start the daemon with a persistent shader cache
SparkDaemon --cache-dir ~/.cache/spark/shaders &

# Terminal 2: enable the daemon for a particular engine run
SparkEngine +spark.daemon.enabled 1
```

First engine run compiles shaders and pushes them to the daemon. A
second engine (editor, headless cook tool, a different game module)
started while the same daemon is still up sees the cached bytecode
instantly — no per-process cold compile.

## Still not wired

- **AssetPipeline** does not consult the daemon's `AssetService` yet.
  Different shape (typed `shared_ptr<Asset>` not raw blobs) so it's
  a distinct integration problem.
- **Engine does not auto-launch the daemon.** If it isn't running,
  `TryConnect` returns false and the engine continues with in-process
  caches. Auto-spawn via `Process::Builder::Detached()` is a separate
  slice — needs a policy for "is there already a daemon on this
  socket?".
- **File watching / hot reload push events** from the daemon are
  unimplemented. `ShaderHotReload` still polls mtimes on the main
  thread.
