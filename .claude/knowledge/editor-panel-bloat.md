# Editor Panel Bloat — Unused Panels and Duplicates

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Active
**Severity:** High

## Description

SparkEditor has 32 panel classes but only 21 are instantiated in EditorUI.cpp. 8 complete panels (11,257+ lines) are built but never shown. 3 pairs of panels have overlapping functionality.

## 8 Unused Panels (Never Instantiated)

These panels exist in `SparkEditor/Source/Panels/` but are never created in `EditorUI.cpp`:

| Panel | .cpp Lines | Feature |
|-------|-----------|---------|
| MaterialEditorPanel | 1,832 | Material/shader editor |
| DialogueEditorPanel | 1,781 | Dialogue system editor |
| AssetDependencyPanel | 1,551 | Asset dependency visualization |
| AudioMixerPanel | 1,467 | Audio mixing interface |
| PerformanceProfilerPanel | 1,296 | Performance profiling UI |
| ParticleEditorPanel | 1,235 | Particle system editor |
| RuntimeInspectorPanel | 1,003 | Runtime entity inspector |
| PlayModeToolbarPanel | 644 | Play mode controls |

**Total: 11,257 lines of dead editor code.**

## 3 Duplicate Panel Pairs

### Hierarchy Duplication
- **HierarchyPanel** (1,045 lines) — Full scene graph with drag-drop, undo, callbacks
- **SimpleHierarchyPanel** (283 lines) — 4 hardcoded objects stub
- Both instantiated. SimpleHierarchyPanel is redundant.

### Console Duplication
- **ConsolePanel** (821 lines) — EditorLogger integration, filtering, history, export
- **SimpleConsolePanel** (880 lines) — External SparkConsole integration
- Different architectures solving same problem.

### Scene Statistics Duplication
- **SceneStatisticsPanel** (391 lines) — Entity/render/physics/memory stats
- **SceneStatsPanel** (273 lines) — Frame graphs, FPS history
- Only SceneStatisticsPanel instantiated. SceneStatsPanel is dead.

## Engine-Depends-on-Editor Violation

`SparkEngine/Source/AllEnums.h` includes 8 SparkEditor enum headers:
```
SparkEditor/Source/Enums/BuildSystemEnums.h
SparkEditor/Source/Enums/VersionControlEnums.h
SparkEditor/Source/Enums/CoreEditorEnums.h
... (5 more)
```
This pulls editor enums into engine code — architectural violation.

## Oversized Editor Files

| File | Lines | Limit Exceeded |
|------|-------|---------------|
| VisualScriptingSystem.cpp | 4,067 | 10x |
| EditorUI.cpp | 2,353 | 5.9x |
| AdvancedAssetPipeline.cpp | 2,324 | 5.8x |
| LightingTools.cpp | 1,963 | 4.9x |
| AnimationTimeline.cpp | 1,796 | 4.5x |
| VersionControlSystem.cpp | 1,710 | 4.3x |
| SparkEngineIntegration.cpp | 972 | 2.4x |

## SparkEngineIntegration — 51 Public Methods

`SparkEditor/Source/Integration/SparkEngineIntegration.h` (610 lines) handles engine connection, scene sync, entity management, asset management, live variables, debugging, console, camera, and gizmo control — all in one class. Needs decomposition into 3-4 focused classes.

## Action

1. Delete 8 unused panels (11,257 lines)
2. Delete SceneStatsPanel and SimpleHierarchyPanel (duplicates)
3. Remove editor enum includes from AllEnums.h
4. Split SparkEngineIntegration into focused classes
