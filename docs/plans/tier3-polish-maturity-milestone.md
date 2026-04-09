# Milestone: Tier 3 Polish & Maturity (M1)

**Created:** 2026-04-09  
**Status:** Proposed  
**Gate:** Milestone is complete only when (1) required CI jobs pass and (2) wiki pages are updated for every subsystem touched by each epic.

## Scope Source

This milestone scopes the seven Tier-3 items from `.claude/knowledge/engine-recommendations-2026-04-04.md` (items 9-15), each represented as one epic.

## Global Completion Gates

1. **CI gate (mandatory):**
   - `check-format`
   - `validate-prompts`
   - `build-linux-gcc`
   - `build-linux-clang`
   - `build-linux-asan`
   - `build-linux-tsan`
   - `build-windows-vs2022`
   - `coverage`
2. **Docs gate (mandatory):**
   - Every changed subsystem must have its corresponding `wiki/` page updated in the same PR.
   - `docs/sync-wiki.sh sync` and `docs/update-all-docs.sh check` must pass before merge.

---

## Epic T3-1: CPack Packaging / SDK Distribution

- **Owner:** Build & Release (primary), SDK Maintainer (support)
- **Estimate:** 8 engineer-days
- **Subsystems:** CMake/install, packaging, SparkSDK
- **Perf budget impact:** None at runtime. Build pipeline time budget: **+5 minutes max** for packaging jobs.

### Acceptance Tests

- CPack generates `.zip` and platform-native package artifacts from CI.
- `find_package(SparkEngine CONFIG REQUIRED)` works in a clean external sample project.
- Packaged SDK includes headers, import libs, runtime binaries, and license files.
- Installation paths validate on Windows + Linux.

### Required Docs Updates

- `docs/packaging.md` — add CPack invocation, artifact matrix, troubleshooting.
- `wiki/Build-System.md` — document packaging targets and install layout.
- `wiki/Getting-Started.md` — add SDK consumer quickstart.

---

## Epic T3-2: Game Module Code Deduplication

- **Owner:** Gameplay Architecture
- **Estimate:** 10 engineer-days
- **Subsystems:** Engine gameplay systems, SparkGameRPG, SparkGameARPG
- **Perf budget impact:** Neutral to positive. CPU frame budget target: **no regression >0.10 ms** in gameplay-system update slices.

### Acceptance Tests

- RPG/ARPG modules compile and run using shared engine Quest/Dialogue extension points.
- Existing quest/dialogue save/load compatibility tests pass.
- New tests verify module-specific behavior through extension hooks (not duplicated systems).
- Static analysis confirms duplicate class removal from module scope.

### Required Docs Updates

- `wiki/Gameplay-Systems.md` — extension/override model and ownership boundaries.
- `wiki/Game-Modules.md` — module integration contract for engine-provided gameplay systems.
- `docs/architecture/gameplay-extension-policy.md` — update policy and examples.

---

## Epic T3-3: Undo/Redo Wiring Completeness

- **Owner:** Editor Team
- **Estimate:** 7 engineer-days
- **Subsystems:** SparkEditor command stack, panel editing flows, hierarchy/gizmo operations
- **Perf budget impact:** Editor interaction budget: **<0.25 ms median** command push/pop overhead; no frame hitch >2 ms on history operations.

### Acceptance Tests

- Transform gizmo move/rotate/scale produce reversible command entries.
- Component add/remove/edit operations are fully undoable/redone across supported panels.
- Hierarchy reparent, create, delete are undoable with entity integrity preserved.
- Serialized undo stack survives editor save/load session boundaries (if persistence is enabled).

### Required Docs Updates

- `wiki/Editor.md` — undo/redo coverage matrix by operation type.
- `wiki/Editor-Panels.md` — panel compliance table for command wiring.
- `wiki/Collaborative-Editing.md` — note interaction model between collaboration and local undo.

---

## Epic T3-4: LOD Auto-Generation

- **Owner:** Rendering Geometry Pipeline
- **Estimate:** 9 engineer-days
- **Subsystems:** MeshLOD, asset import/cook pipeline, meshoptimizer integration
- **Perf budget impact:**
  - Build/cook time budget: **+15% max** per mesh asset.
  - Runtime render budget target: **-0.50 ms or better** in geometry-heavy scenes at 1080p baseline.

### Acceptance Tests

- Given a high-poly mesh, pipeline emits deterministic LOD chain (L0..Ln) from configured reduction targets.
- Visual/metric validation: geometric error thresholds stay within configured limits.
- Runtime LOD selection transitions correctly by distance with no missing index/vertex buffers.
- Cooked content hash changes only when source mesh/settings change.

### Required Docs Updates

- `wiki/Rendering-System.md` — auto-LOD generation pipeline + quality settings.
- `wiki/Asset-Pipeline.md` — import/cook options and LOD metadata schema.
- `docs/specs/asset-format.md` — extend format spec for generated LOD descriptors.

---

## Epic T3-5: Multi-Monitor / Floating Editor Windows

- **Owner:** Editor UX/Platform
- **Estimate:** 11 engineer-days
- **Subsystems:** Docking/layout persistence, window lifecycle, platform windowing abstraction
- **Perf budget impact:** Editor UI frame budget: **no regression >0.30 ms** in ImGui update/render path with two active monitors.

### Acceptance Tests

- Panels can detach to native secondary windows and re-dock reliably.
- Layout persistence restores multi-monitor placements across editor restarts.
- Fallback behavior when secondary monitor disappears (hot unplug) is safe and deterministic.
- DPI scaling and input focus operate correctly across mixed-DPI displays.

### Required Docs Updates

- `wiki/Editor.md` — multi-window workflow and known constraints.
- `wiki/Editor-Panels.md` — detachable panel support matrix.
- `wiki/Threading-Model.md` — any thread-affinity constraints for window operations.

---

## Epic T3-6: Multiplayer Tutorial Documentation

- **Owner:** DevRel + Networking
- **Estimate:** 4 engineer-days
- **Subsystems:** Documentation/tutorial content; references Networking, ECS, and game module setup
- **Perf budget impact:** None.

### Acceptance Tests

- A new user can follow the tutorial end-to-end and run a two-client local session.
- Tutorial code/config snippets compile and run against current default branch.
- Tutorial includes one authoritative-server example and one client prediction/reconciliation example.
- Link-check and docs lint pass for all added/updated pages.

### Required Docs Updates

- `wiki/Making-Your-First-Multiplayer-Game.md` — expand to full guided flow.
- `wiki/Networking.md` — tutorial cross-links and prerequisites.
- `docs/specs/networking-wire-format.md` — reference tutorial packet examples where applicable.

---

## Epic T3-7: AI/NavMesh Visual Debugger

- **Owner:** AI/Gameplay Systems
- **Estimate:** 8 engineer-days
- **Subsystems:** AI runtime debug rendering, navmesh introspection, behavior tree state overlay
- **Perf budget impact:**
  - Debug-off: **zero measurable overhead** in shipping/profile builds.
  - Debug-on: **<=1.00 ms** frame overhead budget on reference AI test scene.

### Acceptance Tests

- Toggleable navmesh polygon overlay with area type coloring.
- Toggleable perception cones/radii for selected AI entities.
- Behavior tree node-state visualization (running/success/failure) updates live.
- Debug renderer remains thread-safe with async AI updates and does not crash on scene reload.

### Required Docs Updates

- `wiki/AI-and-Navigation.md` — debugger usage, overlays, and troubleshooting.
- `wiki/Editor.md` — where the debugger is exposed in editor UI.
- `wiki/Debug-Rendering.md` (or equivalent) — draw-call/debug pass integration details.

---

## Tracking Template (per Epic)

- [ ] Scope approved
- [ ] Implementation merged
- [ ] Acceptance tests passing
- [ ] Perf budget validated
- [ ] Required docs updated (`wiki/` + any `docs/specs/` changes)
- [ ] CI gate passing

## Exit Criteria (Milestone M1)

Milestone M1 closes only when all seven epics are complete and every epic satisfies:

1. Acceptance tests pass on CI.
2. Performance impact is within the declared budget.
3. Required documentation updates are merged, including subsystem wiki pages.
4. Mandatory CI jobs are green on the integrating branch.
