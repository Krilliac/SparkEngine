# Editor Panel Bloat — Unused Panels and Duplicates

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Partially Resolved

## Description

SparkEditor had 32 panel classes but only 21 were instantiated in EditorUI.cpp. 8 complete panels (11,257+ lines) were built but never shown. 3 pairs of panels had overlapping functionality.

## Resolution (2026-03-16)

### Panels Restored and Wired In
Three panels were restored from git history and properly wired into EditorUI.cpp (created in `CreatePanels()`, added to Window menu):

| Panel | Lines | Feature | Status |
|-------|-------|---------|--------|
| **HierarchyPanel** | 1,045+338 | Full scene graph with drag-drop, undo, multi-select | **RESTORED** — replaced SimpleHierarchyPanel |
| **MaterialEditorPanel** | 1,832+270 | PBR material and shader property editor | **RESTORED** — wired into EditorUI |
| **PlayModeToolbarPanel** | 644+81 | Play/Stop/Pause transport controls | **RESTORED** — wired into EditorUI |

### Panels Deleted (confirmed dead / never needed wiring)
These were deleted in prior sessions because they had no callers and were pure stubs or duplicates:

| Panel | Lines | Reason |
|-------|-------|--------|
| SimpleHierarchyPanel | 283+84 | **DELETED** — replaced by superior HierarchyPanel |
| SceneStatsPanel | 273 | **DELETED** — duplicate of SceneStatisticsPanel |
| DialogueEditorPanel | 1,781 | Deleted — never wired in |
| AssetDependencyPanel | 1,551 | Deleted — never wired in |
| AudioMixerPanel | 1,467 | Deleted — never wired in |
| PerformanceProfilerPanel | 1,296 | Deleted — never wired in |
| ParticleEditorPanel | 1,235 | Deleted — never wired in |
| RuntimeInspectorPanel | 1,003 | Deleted — never wired in |

### Remaining Issues
- **Console Duplication**: ConsolePanel (821 lines) and SimpleConsolePanel (880 lines) both exist with different architectures
- **Oversized Files**: EditorUI.cpp (~2,400 lines), AdvancedAssetPipeline.cpp (2,324 lines) still exceed limits
- **SparkEngineIntegration**: 51 public methods in one class — needs decomposition

## Notes

- HierarchyPanel now creates an owned SceneFile on Initialize() so it works standalone
- HierarchyPanel exposes `GetSceneObjects()` and `ResetToDefault()` for EditorUI compatibility
- AllEnums.h engine-depends-on-editor violation was already resolved (file deleted in prior session)
