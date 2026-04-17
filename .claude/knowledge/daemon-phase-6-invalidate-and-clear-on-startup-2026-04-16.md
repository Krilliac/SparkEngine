# SparkDaemon Phase 6 follow-up — `daemon.invalidate` + `spark.daemon.clear_on_startup`

**Last updated:** 2026-04-16
**Type:** Observation
**Status:** Active
**Cross-references:** `daemon-phase-6-diagnostics-command-2026-04-16.md`, `daemon-phase-6-clear-cache-2026-04-16.md`

## TL;DR

Rounded out the operator surface of Phase 6 with two small additions:

1. **`daemon.invalidate <path>` InGameConsole command** —
   forces the daemon to drop every platform variant of a given
   asset path. Fronts the existing `AssetServiceClient::InvalidateAsset`
   RPC; no new wire messages.

2. **`spark.daemon.clear_on_startup` CVar** — when `true`, the
   lifecycle helper runs `ShaderServiceClient::ClearCache()` +
   `AssetServiceClient::ClearCache()` immediately after connect,
   logging success or per-cache failure.

5 new integration tests (3 for the invalidate command, 2 for the
CVar), full suite 5588 passed / 0 failed, 123822/123822 assertions.

## `daemon.invalidate <path>`

```
daemon.invalidate meshes/hero.obj   → "daemon.invalidate: meshes/hero.obj (dropped 2 variants)"
daemon.invalidate does/not/exist    → "daemon.invalidate: does/not/exist (dropped 0 variants)"
daemon.invalidate                   → "usage: daemon.invalidate <path>"
daemon.invalidate <path>  (no d.)   → "daemon: not connected"
```

Handler in `DaemonLifecycle.cpp` (anon namespace):

```cpp
std::string RunInvalidateCommand(const std::string& path)
{
    auto& conn = DaemonConnection::Instance();
    auto* client = conn.GetClient();
    if (!conn.IsConnected() || client == nullptr)
        return "daemon: not connected";

    AssetServiceClient tmp(*client);
    auto* c = g_assetClient ? g_assetClient.get() : &tmp;
    auto r = c->InvalidateAsset(path);
    if (!r)
        return std::string("daemon.invalidate: ") + r.error();
    return "daemon.invalidate: " + path + " (dropped " + std::to_string(*r) +
           (*r == 1u ? " variant)" : " variants)");
}
```

Reuses the lifecycle-owned `g_assetClient` when present, stack-local
otherwise — same pattern as `daemon.stats` and `daemon.clear_cache`.
Singular / plural is spelled correctly ("1 variant" vs "N variants")
because operator UX matters even at this scale.

## `spark.daemon.clear_on_startup` CVar

```cpp
Spark::CVar<bool> cv_DaemonClearOnStartup("spark.daemon.clear_on_startup", false,
                                          Spark::CVarFlags::Save,
                                          "After connecting, drop all cached entries from the daemon.");
```

`InitializeDaemonLifecycle()` invokes the hook right after both
service clients are constructed and `g_active = true`. Failures are
logged at WARN, not propagated — the engine keeps running even if
the daemon refuses the RPC.

```cpp
if (cv_DaemonClearOnStartup.Get())
{
    const auto shaderResult = g_shaderClient->ClearCache();
    const auto assetResult  = g_assetClient->ClearCache();
    if (shaderResult && assetResult)
        SPARK_LOG_INFO(..., "Daemon clear-on-startup: both caches dropped");
    else
        SPARK_LOG_WARN(..., "Daemon clear-on-startup: shader=%s asset=%s", ...);
}
```

### When operators want this

The scenario that drove this: a developer edits a shader locally,
the daemon is still running from a previous session with a stale
blob. Subsequent engine launches serve the stale blob because the
content-hash key is the same. A CVar flip means "I know my source
tree has drifted, discard the daemon's memory". Set and forget per
developer machine, opt-in so shared/CI daemons are never wiped.

## Lifecycle command registration, consolidated

`InitializeDaemonLifecycle()` now unconditionally registers three
commands (`daemon.stats`, `daemon.clear_cache`, `daemon.invalidate`)
before doing any CVar / connection work. `ShutdownDaemonLifecycle()`
unregisters all three. The handlers themselves render
`daemon: not connected` when appropriate.

This is the "wire it in completely or not at all" rule from CLAUDE.md:
a command that only exists when the daemon happens to be reachable
would be the worst of both worlds for operators.

## Test coverage (5 new)

| Test | Verifies |
|---|---|
| `DaemonLifecycle_InvalidateCommandDropsCachedVariants` | Put 2 platform variants → `daemon.invalidate meshes/hero.obj` drops both; subsequent `GetAsset` returns `found=false` |
| `DaemonLifecycle_InvalidateCommandRejectsBadArgs` | No-arg form emits `usage:` hint |
| `DaemonLifecycle_InvalidateCommandWhenDisconnected` | Handler returns exactly `daemon: not connected` |
| `DaemonLifecycle_ClearOnStartupWipesDaemonCaches` | Pre-populate 2 asset entries → connect with CVar on → stats show `entryCount=0` |
| `DaemonLifecycle_ClearOnStartupDisabledLeavesCachesAlone` | Pre-populate → connect with CVar off → entry still present |

The `LifecycleDaemonFixture` now adds an `AssetService` alongside
the existing `ControlService` + `ShaderService` so the integration
tests have a real asset backend. Existing lifecycle tests are
unaffected by the additional service.

A new `BoolCVarGuard` RAII helper scopes the CVar flip to a test —
the CVars are process-wide so this prevents flake from leaking
state into subsequent tests.

## What's still open

- **Editor HUD panel** (`DaemonDebugPanel`) — a dockable ImGui panel
  that shows the same `DaemonDiagnostics` output plus Clear / Invalidate
  buttons. Deliberately deferred: the panel itself is ~200 lines of
  ImGui scaffolding with limited unit-test surface. Best done as a
  standalone slice after the next feature-adding phase lands.
- **Phase 4 Collab broker** — the real next architectural slice per
  the daemon roadmap. Roughly 1–2 weeks of work; see
  `daemon-architecture-roadmap.md` when it gets picked up.
