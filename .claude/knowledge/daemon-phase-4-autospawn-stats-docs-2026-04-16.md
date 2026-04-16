# SparkDaemon Phase 4 — Auto-Spawn + Stats + Docs

**Last updated:** 2026-04-16
**Type:** Observation
**Status:** Active
**Cross-references:** `daemon-services-architecture-2026-04-16.md`, `daemon-phase-3c-lifecycle-wiring-2026-04-16.md`

## TL;DR

Closing-out slice for the daemon feature branch: engine now auto-spawns
the daemon when asked, the Control service exposes uptime + service
inventory via a new `StatsRequest`, a concurrent-clients test pins down
the multi-client story, and `wiki/SparkDaemon.md` documents the whole
design. 5 new tests, full suite 5550/5551.

## Added

| Piece | Files |
|-------|-------|
| Auto-spawn | `SparkEngine/Source/Utils/DaemonLifecycle.cpp` (new CVars + `TrySpawnDaemon`) |
| `Control::StatsRequest/Response` | `SparkEngine/Source/Utils/DaemonProtocol.h` (`DaemonStats` struct + codec), `SparkDaemon/src/ControlService.{h,cpp}`, `SparkDaemon/src/DaemonServer.{h,cpp}` (`SnapshotStats`), `SparkDaemon/src/main.cpp` (wires the provider) |
| Concurrent-clients test | `Tests/TestDaemonConcurrent.cpp` (5 tests including codec round-trip) |
| Documentation | `wiki/SparkDaemon.md`, `wiki/_Sidebar.md` link |

## New CVars

| Name | Default | Purpose |
|------|---------|---------|
| `spark.daemon.auto_spawn` | false | If `TryConnect` fails, launch `SparkDaemon` as a detached subprocess and retry |
| `spark.daemon.binary_path` | empty | Override the executable path; empty = `./SparkDaemon` |

Auto-spawn is default off because operators may want to control launch
themselves (shell init script, systemd unit, etc.). Binary path is a
separate CVar so installed builds where the daemon lives outside CWD
can still opt in without relocating the working directory.

## Auto-spawn implementation notes

Launch uses `Spark::Process::Builder(binary).Detached()` — the child
is fire-and-forget, no pipes, parent doesn't track the PID. If the
spawn returns an error, we log a warning and give up; the engine
continues with in-process caches.

After a successful `Launch`, we poll the socket path with
`std::filesystem::exists` every 20 ms up to 2 s. In practice the
daemon binds in well under 100 ms; 2 s covers very loaded CI runners
without making failure slow.

We check `filesystem::exists(binary)` before calling `Launch` to fail
fast when the daemon binary isn't installed at all. Keeps log noise
down on machines that will never have a daemon.

## StatsRequest design

Message numbers `0x0007` / `0x0008` — parallel to the per-service
`GetCacheStatsRequest` which uses the same numbers in its own
namespace. Payload is `DaemonStats`:

```cpp
struct DaemonStats
{
    uint64_t uptimeSeconds = 0;           // since Run() began its accept loop
    std::string protocolVersion;          // matches kProtocolVersion
    std::vector<uint16_t> registeredIds;  // sorted ascending
};
```

Uptime is integer seconds to keep the codec simple — sub-second
precision isn't useful at the operational level this RPC is meant to
serve (is-the-daemon-alive, what-services-exist monitoring).

`DaemonServer::SnapshotStats` walks `m_services` once and returns the
struct. The provider is injected into `ControlService` as a
`std::function<DaemonStats()>` at server construction so the
Control-service class doesn't need a back-pointer to the server.

## Concurrent-clients coverage

Previous tests used a single client per server fixture. The new
`DaemonConcurrent_TwoClientsShareCacheAndDontInterleaveResponses`
test runs two threads concurrently:

- **Thread A:** connects, calls `PutCacheEntry` 50 times with distinct
  hashes + payloads containing the loop index.
- **Thread B:** connects, calls `Ping` 50 times.

After both threads join, a third client issues 50 `GetCacheEntry`
calls and verifies each returns the index-tagged blob the writer put
— if the per-connection dispatch threads in `DaemonServer` were
interleaving responses, the first byte of returned blobs would be
mis-aligned across the 50 lookups.

Runtime: ~500 ms per test (dominated by `DaemonServer::Stop`'s 500 ms
poll tick). Cheap enough to run on every CI run.

## Pattern for future StatsRequest additions

When a future service needs to expose daemon-wide metrics (not just
its own per-service counters), extend `DaemonStats` in
`DaemonProtocol.h` rather than adding a new RPC. Bump
`kProtocolVersion` so clients that decode with an older layout fail
the decode safely (see `DecodeDaemonStats`'s truncation check).

Currently only uptime + version + service IDs are exposed. Good
future additions: total connections ever accepted, currently active
connection count, per-service request counts.

## What's left on the roadmap

From `daemon-services-architecture-2026-04-16.md`:

- **Phase 4 Collab broker** — stateful sessions, lock arbitration,
  operation history. Different shape from the cache services; needs
  streaming messages (not request/response) and presence tracking.
- **Phase 5 Build monitor** — watches CMake + source files, streams
  build events.
- **File watching** — inotify / FSEvents / kqueue so the daemon can
  push `ShaderReloaded` / `AssetChanged` events.
- **Windows named-pipe transport** — replace the "not implemented"
  stub with real `CreateNamedPipe` code.
- **AssetPipeline wiring** — bridge the engine's typed
  `shared_ptr<Asset>` to the daemon's raw-blob Asset service. Harder
  than the ShaderDiskCache wiring because asset objects carry GPU
  handles and subtype-specific state.
- **LRU eviction** — bound disk cache size.
- **Actual shader compilation inside the daemon** — warm DXC /
  glslang workers, parallel variant compile. The big latency win the
  daemon was originally motivated by.

Everything shipped so far (Phases 1 – 3c + this one) is the
transport + cache infrastructure; the "daemon does compilation for
you" layer is still a future phase.
