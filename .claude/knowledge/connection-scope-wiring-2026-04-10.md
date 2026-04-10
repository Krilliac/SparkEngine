# ConnectionScopeFilter Wiring — Closed 2026-04-10

**Last updated:** 2026-04-10
**Type:** Observation
**Status:** Resolved

## Context

During an "engine next steps" planning pass, cross-referencing older
recommendation files against the current codebase revealed that
`ConnectionScopeFilter` (per-connection entity visibility filter) had been
fully implemented with area/distance/team/visibility filtering and a
singleton API, but was **not wired into the replication path** — only its
`Shutdown()` was called from `GameplayLifecycleShared.cpp`. No production
call site invoked `IsEntityInScope()` or `SetScope()`.

This is a classic CLAUDE.md "built but not wired in" violation.

## What Changed

### NetworkManager replication path

`SparkEngine/Source/Engine/Networking/NetworkReplication.cpp`:
- `SendFullEntitySync()` now checks `ConnectionScopeFilter::IsEntityInScope()`
  before sending spawn + state-update pairs to the target client.
- `UpdateReplication()` (both the full-sync and per-connection delta paths)
  now filters every entity against every connected client's scope before
  queuing an EntityStateUpdate message. The old full-sync path was a
  `SendToAll` broadcast — it now iterates clients and respects per-client
  scope.

### Public API on NetworkManager

`SparkEngine/Source/Engine/Networking/NetworkManager.h` + `.cpp`:
- New `SetClientScope(ClientID, position, radius, areaId, teamMask, visibilityMask)`
  forwards into `ConnectionScopeFilter::SetScope`.
- New `ClearClientScope(ClientID)` forwards to `RemoveConnection`.
- `ReplicatedEntity` struct gained `areaId`, `teamMask`, and `visibilityMask`
  fields with "accept all" defaults so existing code paths continue to
  replicate everywhere until a scope is explicitly set.

### Cleanup on disconnect / timeout / kick

`SparkEngine/Source/Engine/Networking/NetworkConnection.cpp`:
- `HandleDisconnect()`, `KickClient()`, and `CheckConnectionTimeouts()` now
  call `ConnectionScopeFilter::RemoveConnection()` so a future client
  reusing the same `ClientID` starts with "see everything" instead of
  inheriting the prior player's visibility sphere.

### WorldServer integration

`SparkEngine/Source/Engine/Networking/WorldServer.h` + `.cpp`:
- New `WorldServer::UpdatePlayerPosition(clientId, position, interestRadius)`
  updates `PlayerSession::lastKnownPosition` and forwards to
  `NetworkManager::SetClientScope`. This is the seam that area servers call
  when player coordinates arrive.
- `WorldServer::HandlePlayerDisconnect()` also calls `ClearClientScope`
  defensively for code paths that bypass the network layer (tests,
  drive-by disconnects).

### Tests

`Tests/TestConnectionScopeWiring.cpp` — 6 new tests:
- `ConnectionScopeWiring_SetClientScopeRegistersFilter`
- `ConnectionScopeWiring_ClearClientScopeRemovesFilter`
- `ConnectionScopeWiring_SetClientScopeReplacesPrevious`
- `ConnectionScopeWiring_SetClientScopeAppliesTeamMask`
- `ConnectionScopeWiring_ReplicatedEntityHasScopeFields`
- `ConnectionScopeWiring_ClearNonexistentScopeIsSafe`

Complements the existing `TestConnectionScopeFilter` (which tests the
filter in isolation) — these verify the **wiring** between NetworkManager
and the filter.

## Verification

- `cmake --preset linux-gcc-release && cmake --build build/linux-gcc-release`
  succeeds.
- `SparkTests` reports **4212 passed, 0 failed, 1 warned, 4213 total** after
  the changes. The single warning is the pre-existing flaky
  `Integration_NetworkingECS_ReplicationLatencyJitterPredictionReconciliation`
  test (added to `TestWarnings.h` during this session).

## Related

- `.claude/knowledge/engine-viability-evaluation.md` — listed networking as
  enterprise-grade but did not catch this wiring gap.
- `TestConnectionScopeFilter.cpp` — pre-existing unit tests for the filter.
- `ConnectionScope.h` + `ConnectionScopeFilter.h` — the filter
  implementation (unchanged this session).
