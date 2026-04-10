# Engine Next Steps — Phase A (2026-04-10)

**Type:** Observation
**Status:** Active
**Related:** [stub-and-abandoned-features-2026-04-10.md](stub-and-abandoned-features-2026-04-10.md), [codebase-bloat-audit-2026-03-15.md](codebase-bloat-audit-2026-03-15.md)

---

## Context

Continuation of the wire-or-delete campaign from the April 10 audit. This
branch (`claude/engine-next-steps-3M0Sk`) tackled the "highest value / lowest
risk" items in Phase A. Phase B items (AnimationSystem, SelectionManager
uint32→uint64 unification, SteamTransport, DXRSupport, Foliage GPU baking,
VRSystem) are deferred to their own branches.

Two fresh Explore passes at session start re-verified the state of the
remaining pending items. Several audit claims did NOT survive re-verification
— documented below so future audits do not re-flag them.

---

## New false positives (do NOT re-flag in future audits)

Items that the April 10 audit listed as orphaned but are actually
wired/already-documented:

| System | Reality | Evidence |
|---|---|---|
| `Sequencer` (engine-side) | **Wired** as `SequencerManager` | `GameplayLifecycleShared.cpp:828` (Update), `:868` (Update) |
| `DecalSystem` (engine-side) | **Wired** | `GameplayLifecycleShared.cpp:326` (Initialize), `:1112` (Update), `:1119` (Shutdown) |
| `ClusteredLightGPU.h` | **Already documented** with `@note` header | `Graphics/ClusteredLightGPU.h:17-24` |
| `DirtyRectTracker.h` | **Already documented** with `@note` header | `Graphics/DirtyRectTracker.h:8-14` |

The audit's "Tier 3 unwired but tested" and "unresolved March singletons"
sections should be updated to drop these four.

---

## Phase A resolutions

### A1. `NavMeshManager` + `NavMeshObstacleManager` — documented as demand-driven registries

Investigation showed both classes are **intentional passive singletons**.
`NavMeshManager` is consumed on demand by `AISystem.cpp:139`,
`NavMeshLinkSystem`, and `GameModules/SparkGameRTS`. `NavMeshObstacleManager`
has no engine-lifecycle consumer (no dynamic-obstacle gameplay feature ships
with the engine yet) but it is fully implemented (~435 lines) and tested
(`Tests/TestNavMeshObstacles.cpp`) so it stays as a library for future game
modules.

`GameplayLifecycleShared.cpp:328-329` already had comments identifying both as
"passive registries, no lifecycle needed." The April 10 audit subagent missed
those comments — this entry records that the comments are authoritative.

**Action taken:** Added a `@note` block to both headers explaining the
intentional-utility pattern, matching the style used for the 25 Graphics
utility headers already documented. No `.cpp` changes.

- `SparkEngine/Source/Engine/AI/NavMesh.h` — `@note` added near `NavMeshManager`
- `SparkEngine/Source/Engine/AI/NavMeshObstacles.h` — `@note` added in file header

### A2. `LODManager` — documented as passive cache

Same situation as A1 but for the graphics `LODManager`:
`GameplayLifecycleShared.cpp:327` already reads "LODManager is a passive cache
(no init/update needed; queries only)". The class has full implementations on
both Windows and Linux (`Graphics/MeshLOD.cpp:198-335` for Windows,
`:513-635` for Linux stubs — the apparent duplicate definitions are platform
stubs inside `#ifdef SPARK_PLATFORM_WINDOWS` / `#else` blocks, NOT an ODR
violation).

**Action taken:** Added a `@note` block to the `LODManager` class declaration
in `SparkEngine/Source/Graphics/MeshLOD.h`. No `.cpp` changes.

### A3. `NetworkDebugPanel` producer wiring — pull model

The audit flagged that `NetworkManager.cpp` has zero references to
`NetworkDebugPanel`, so the panel's bandwidth / latency graphs were never fed
from live network data. The obvious fix was to push from `NetworkManager`,
but that reverses the dependency direction (engine → editor) and would
require either a forward-declare dance or a static registration hook.

**Action taken:** Implemented the reverse direction instead — let the panel
**pull** from `NetworkManager::GetStats()` each frame, which is cleaner
because:

1. `NetworkManager` stays free of editor includes.
2. `NetworkManager` already tracks everything needed (`NetworkStats` has
   `bytesSent`, `bytesReceived`, `packetsDropped`, `ping`).
3. `SparkEditor` already links `SparkEngineLib`, so the include direction is
   natural.
4. The existing push API (`RecordBytesSent`, `RecordBytesRecv`, `LogPacket`,
   `SetCurrentLatency`, `RecordPacketDrop`) is kept intact so tests and
   future explicit producers still work; `PollEngineNetwork()` simply funnels
   its computed deltas through the same API so `TakeSnapshot()` remains the
   single source of truth.

Implementation details:

- Added `#include "Engine/Networking/NetworkManager.h"` to the panel header.
- Added `PollEngineNetwork()` private method that:
  - Computes deltas `(stats.bytesSent - m_lastPolledBytesSent)` etc.
  - Clamps to zero if counters went backwards (reconnect reset).
  - Calls `RecordBytesSent` / `RecordBytesRecv` / `RecordPacketDrop` / `SetCurrentLatency`.
  - Saves the new baseline.
- Called `PollEngineNetwork()` at the top of `Update(float deltaTime)`.
- Seeded the baseline in `Initialize()` so the first frame shows a zero
  delta instead of the full historical total.
- Added three tracking members: `m_lastPolledBytesSent`, `m_lastPolledBytesRecv`,
  `m_lastPolledPacketsDropped`.

Tests added to `Tests/TestNetworkDebugPanel.cpp` (gated on
`__has_include(<imgui.h>)` because the real `NetworkDebugPanel` derives from
`EditorPanel` whose `.cpp` is only linked into `SparkTests` when ImGui is
present, per `Tests/CMakeLists.txt:475-496`):

1. `NetworkDebugPanelReal_PollEngineNetworkDelta` — pushes bytes/ping into
   NetworkManager stats, ticks the panel, asserts the snapshot picked up the
   delta (~4096 B/s outbound for a 2 KB delta over 0.5s, 42 ms latency).
2. `NetworkDebugPanelReal_PollEngineNetworkHandlesReset` — seeds high
   baseline, resets NetworkManager stats to zero, verifies the panel clamps
   the negative delta instead of wrapping a uint64 subtraction.

The 11 existing standalone-collector tests in the file are untouched and
continue to verify the core `TakeSnapshot` / bandwidth / latency logic.

### A4. Skipped — already done

`ClusteredLightGPU.h` and `DirtyRectTracker.h` already carry `@note`
headers. No-op.

### A5. `.claude/index.md` refreshed

- Added a new knowledge-table row for this entry.
- Removed the false claim on line 46 ("All 12 former stubs now have .cpp
  implementations") — this contradicted the April 10 audit.
- Added a "Passive registries" bullet explicitly listing
  `NavMeshManager` / `NavMeshObstacleManager` / `LODManager` so future
  sessions don't re-flag them.
- Listed the remaining Tier 1 stubs (`VRSystem`, `SteamTransport`,
  `SteamPlatform`, `EpicPlatform`, `ConsolePlatform`) in the Rendering line
  instead of the misleading "all stubs resolved" claim.
- Noted the new `NetworkDebugPanel` pull model in the Editor line.

---

## Phase B — still deferred

Unchanged from the approved plan. These need their own branches:

1. `AnimationSystem` (engine-side) — investigate overlap with
   `AnimNotifyManager`, `Engine/Animation/StateMachine`, ECS
   `AnimationSystem` before wiring or renaming.
2. `SelectionManager` ID unification — `EntityId` is `uint32_t`,
   `ObjectID` is `uint64_t`. Migrate `HierarchyPanel`, `InspectorPanel`,
   `SceneView` after unification.
3. `SteamTransport` — delete or mark `[[deprecated]]`.
4. `DXRSupport` — 887-line `.cpp`, no tests, no call sites. Wire into D3D12
   RT path or delete.
5. Foliage GPU impostor baking + render-graph submission.
6. `VRSystem` — wait until OpenXR is linked.

---

## Files touched in Phase A

- `SparkEngine/Source/Engine/AI/NavMesh.h` — `@note` on `NavMeshManager`
- `SparkEngine/Source/Engine/AI/NavMeshObstacles.h` — `@note` on file header
- `SparkEngine/Source/Graphics/MeshLOD.h` — `@note` on `LODManager`
- `SparkEditor/Source/Panels/NetworkDebugPanel.h` — new include,
  `PollEngineNetwork()`, three tracking members, Update + Initialize hook
- `Tests/TestNetworkDebugPanel.cpp` — two new gated real-class tests
- `.claude/index.md` — new row, state-block refresh
- `.claude/knowledge/engine-next-steps-2026-04-10.md` — this file (new)

No `.cpp` files in `SparkEngine/` were modified — all header-only changes on
the engine side. This keeps the blast radius small: any TU that includes
these headers recompiles, but no binary interface changes.
