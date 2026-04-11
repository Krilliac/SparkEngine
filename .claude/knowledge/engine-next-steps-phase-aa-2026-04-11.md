# Engine next-steps — Phase AA (2026-04-11)

**Type:** Observation
**Status:** Active — landed
**Scope:** Register the two `EditorPanel` subclasses that live outside
the `SparkEditor/Source/Panels/` directory and were never wired into
`EditorUI::CreatePanels`. First phase of Theme 3C (editor panel
activation) from the Phase U+ plan.

---

## Panel survey

The Phase U+ plan for Theme 3C said:

> Grep `SparkEditor/Source/Panels/` for classes that never appear in
> `EditorPanelFactory::Register...` call sites.

The initial glob against `SparkEditor/Source/Panels/*Panel.h` returned
59 files, all 59 of which are registered in
`EditorPanelFactory::CreateCorePanels` or `CreateToolAndContentPanels`.
`ProjectBrowserPanel` is registered in a different way — it lives as a
dedicated `m_projectBrowserPanel` modal overlay member on `EditorUI`,
initialised in `EditorUI::Initialize` and rendered conditionally when
`IsModalActive()` — so it's not in the `m_panels` dictionary but it IS
wired.

A broader grep across `SparkEditor/Source/**/*.h` for `class.*public
EditorPanel\b` found seven classes **outside** the `Panels/` folder:

| File | Status before Phase AA |
|---|---|
| `Animation/AnimationTimeline.h` | **Orphan** — no registration, no external call sites |
| `AssetPipeline/AdvancedAssetPipeline.h` | **Orphan** — no registration, no external call sites |
| `LevelStreaming/LevelStreamingSystem.h` | **Orphan** — ~1700 lines, zero external references |
| `MaterialEditor/MaterialEditor.h` | **Orphan** — no registration (note: `Panels/MaterialEditorPanel.h` is a *different* class that IS registered) |
| `Profiler/PerformanceProfiler.h` | **Registered** as `"Profiler"` |
| `Terrain/TerrainEditor.h` | **Registered** as `"TerrainEditor"` |
| `VersionControl/VersionControlSystem.h` | **Orphan** — ~2300 lines across 5 .cpp files, zero external references |

Phase AA wires **two** of the four orphans — `LevelStreamingSystem`
and `VersionControlSystem` — because they are the two with the most
substantial implementations and the most predictable public APIs.
The other two (`AnimationTimeline`, `AdvancedAssetPipeline`,
`MaterialEditor`) are noted as follow-up candidates; registering them
carries higher risk because I cannot CI-verify them on the Linux
build (see below).

## CI verification gap

`SparkEditor` is only built when ImGui is available. On the current
`linux-gcc-release` CI job the dependency audit reports:

```
[MISSING] Dear ImGui
  feature  : SPARK_HAS_IMGUI (path-gated in CMake)
```

As a consequence:

1. `EditorPanelFactory.cpp` is **not compiled on Linux CI**. The panel
   registration changes land in source control but are verified only
   by Windows CI jobs (`build-windows-vs2022`, `build-windows-vs2026`).
2. The Phase AA test files
   (`TestLevelStreamingSystemPhaseAA.cpp` /
   `TestVersionControlSystemPhaseAA.cpp`) are added to the
   ImGui-gated block in `Tests/CMakeLists.txt` and therefore
   **skipped** when ImGui is missing. They run on any CI job that
   enables ImGui (Windows, and any Linux job that installs imgui
   sources).
3. Full SparkTests regression on this Linux CI: **4703 passed, 0
   failed, 1 warned, 4704 total** — same as pre-Phase-AA baseline.

This matches the Phase Z precedent: mechanical code changes to
platform-gated subsystems that cannot be verified in the primary CI
image. Windows CI is the verification of record.

## Files touched

### Code

- `SparkEditor/Source/Core/EditorPanelFactory.cpp`
  - Removed the dead `#include "../Panels/ProjectBrowserPanel.h"`
    (ProjectBrowserPanel is instantiated separately in
    `EditorUI::Initialize`, not in this factory).
  - Added `#include "../LevelStreaming/LevelStreamingSystem.h"` and
    `#include "../VersionControl/VersionControlSystem.h"`.
  - Added two `registerPanel` calls in `CreateToolAndContentPanels`:
    `"LevelStreaming"` and `"VersionControl"`.
  - Added two entries to `InitializePanelIcons`' `panelIcons` table:
    `ICON_FA_MAP` and `ICON_FA_CODE_BRANCH`.

### Tests

- `Tests/TestLevelStreamingSystemPhaseAA.cpp` (new) — 11 tests against
  the real `SparkEditor::LevelStreamingSystem` class covering:
  Initialize/Shutdown lifecycle, empty-world on construct, add/remove
  tile, automatic streaming and pause toggles, world settings
  round-trip, multiple tiles + remove-specific, remove-unknown-tile
  safety, validate-world on empty, idempotent Shutdown, Update
  without viewer safety.

- `Tests/TestVersionControlSystemPhaseAA.cpp` (new) — 11 tests against
  the real `SparkEditor::VersionControlSystem` class covering:
  Initialize/Shutdown lifecycle, enabled toggle, no-repository
  default, `UserInfo` round-trip (`name`, `email`, `signCommits`),
  `CollaborationSettings` round-trip (`enableRealtimeSync`,
  `enableFileLocking`, `enableAutoMerge`, `autoSyncInterval`),
  ignore-pattern management, file-tracked / file-locked queries
  without a repo, `GetActiveUsers` without a repo, `CloseRepository`
  on no-op safety, idempotent Shutdown, `Update` without a repo
  safety.

Both tests are correctly structured to match the real struct field
names (`WorldTile::name` not `tileName`, `UserInfo::name` not
`userName`, `CollaborationSettings::enableRealtimeSync` not
`enableRealTimeUpdates`).

- `Tests/CMakeLists.txt` — added both new test files to the
  **ImGui-gated** `target_sources(SparkTests PRIVATE ...)` block so
  they only compile when the editor .cpp files they depend on are
  available.

## Theme 3C scoreboard

| Panel class | Status |
|---|---|
| 59 `*Panel.h` files in `Panels/` | **All registered** (58 in `m_panels`, 1 as `m_projectBrowserPanel` modal overlay) |
| `LevelStreamingSystem` (outside Panels/) | **Phase AA — registered** |
| `VersionControlSystem` (outside Panels/) | **Phase AA — registered** |
| `AnimationTimeline` (outside Panels/) | **Deferred** — default ctor exists, follow-up |
| `AdvancedAssetPipeline` (outside Panels/) | **Deferred** — default ctor exists, follow-up |
| `MaterialEditor` (outside Panels/) | **Deferred** — note: name collides with `Panels/MaterialEditorPanel.h`, investigate before registering |
| `PerformanceProfiler` (outside Panels/) | **Already registered** as `"Profiler"` |
| `TerrainEditor` (outside Panels/) | **Already registered** as `"TerrainEditor"` |

## Playbook notes for future phases

1. **Panel glob must be recursive.** The initial Theme 3C survey
   against `Panels/*Panel.h` missed four orphans because they live
   in subsystem-specific folders (`LevelStreaming/`, `VersionControl/`,
   `Animation/`, `AssetPipeline/`, `MaterialEditor/`). Next time use
   `grep -rln 'public EditorPanel\\b' SparkEditor/Source/` so the
   survey catches EditorPanel subclasses anywhere in the tree.

2. **Name collision trap:** `SparkEditor::MaterialEditor` (in
   `MaterialEditor/MaterialEditor.h`) and
   `SparkEditor::MaterialEditorPanel` (in
   `Panels/MaterialEditorPanel.h`) are DIFFERENT classes. The
   `Panels/` version is registered as `"MaterialEditor"`; the
   non-Panels version is orphaned. Any phase that registers the
   non-Panels version needs a different key (e.g.,
   `"MaterialEditorAdvanced"`) to avoid collision.

3. **ImGui gating matters.** Tests that depend on editor `.cpp`
   files must live in the ImGui-gated `target_sources` block, not
   the main test source list. Putting them in the main list on
   an ImGui-missing CI image causes silent link errors that are
   hard to trace. Phase AA caught this by reading the CMakeLists
   structure before placing the tests.

4. **ProjectBrowserPanel is not an orphan — it's a modal overlay.**
   The `#include` in `EditorPanelFactory.cpp` was dead code from
   a previous refactor. ProjectBrowserPanel is actually wired via
   `EditorUI::m_projectBrowserPanel` (rendered conditionally when
   `IsModalActive()`) — a legitimate alternative wiring pattern
   for modal-overlay UI that doesn't fit the dockspace panel
   model. Don't assume every `EditorPanel` subclass must live in
   `m_panels[]`.

5. **Defer ambiguous wire-ups.** The three deferred orphans
   (`AnimationTimeline`, `AdvancedAssetPipeline`, `MaterialEditor`)
   all have default constructors but unknown internal state
   assumptions. Registering them in a CI image that doesn't
   compile the editor means any hidden Initialize() bug lands
   unverified. A future session with a Linux-ImGui CI can pick
   these up safely.

## Cross-references

- Plan: [engine-next-steps-phase-u-plan-2026-04-11.md](engine-next-steps-phase-u-plan-2026-04-11.md)
- Previous phases: [engine-next-steps-phase-y-z-2026-04-11.md](engine-next-steps-phase-y-z-2026-04-11.md) (Theme 3B complete)
- Theme 3C status: 2 of 4 outside-Panels/ orphans wired; 3 deferred
  for a future CI session that builds SparkEditor on Linux
