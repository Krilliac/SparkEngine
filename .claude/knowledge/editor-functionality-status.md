# Editor Functionality Status

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Active
**Severity:** High

## Description

Comprehensive audit of 30 editor features. 14 panels/features are working, 8 panels are built-not-shown (never instantiated, ~10K dead lines), 5 systems are built-not-wired, and 4 features are completely missing.

---

## Working Features (14)

| Feature | Panel | Lines | Engine Refs | Notes |
|---------|-------|-------|-------------|-------|
| Viewport/scene view | SceneViewPanel | 404 | D3D11 setup | Render target, camera controls, render modes. Gap: no entity picking |
| Entity inspector | InspectorPanel | 1,303 | 18 | Full component editing with undo/redo via CommandHistory |
| Scene hierarchy | SimpleHierarchyPanel | 283 | 1 | Tree view, selection. Limited vs unused HierarchyPanel (1,045 lines) |
| Asset browser | AssetBrowserPanel | 457 | 2 | File/folder browser, thumbnails, drag-and-drop |
| Physics debug | DebugVisualizerPanel + Physics2DPanel | 245+308 | 1 each | Wireframe physics, collision shapes |
| Console | SimpleConsolePanel | 880 | 9 | Command input, log display, SparkConsole bridge |
| Profiler | PerformanceProfiler | 1,210 | 1-2 | Frame time graphs, CPU/GPU timing, memory |
| Animation editor | SpriteAnimationEditorPanel | 534 | 1 | Frame-by-frame sprite animation only (no skeletal) |
| Play mode (PIE) | PlayModeManager | — | Direct | Play/Pause/Stop, F5 hotkey, scene snapshot/restore |
| Undo/redo | UndoRedoManager + UndoHistoryPanel | 161 | Deep | Command history tracking, Inspector integration |
| Build/export | BuildCookPanel | 591 | 3 | Build config, platform targeting, cook/package |
| Scene statistics | SceneStatisticsPanel | 391 | 2 | Object/triangle/vertex counts, memory, draw calls |
| Toolbar/menu | RenderMainMenuBar() + RenderToolbar() | — | Direct | File/Edit/View/GameObject/Tools/Help menus |
| Gizmos (UI only) | SceneViewPanel buttons | — | — | Mode buttons exist but GizmoSystem NOT connected |

---

## Built-Not-Shown (8 panels, ~10K dead lines)

Fully implemented panels that are NEVER instantiated in EditorUI::CreatePanels():

| Panel | Lines | Key Features | Why It Matters |
|-------|-------|-------------|----------------|
| MaterialEditorPanel | 1,832 | Shader params, texture slots, render states, live preview | Largest panel, fully implemented |
| DialogueEditorPanel | 1,781 | Node-graph editor, branching, conditions, choices | Complete dialogue tree editor |
| AssetDependencyPanel | 1,551 | Dependency graph, reverse lookup, circular detection, unused assets | Powerful debugging tool |
| AudioMixerPanel | 1,467 | Channel faders, bus routing, effects chains, level metering | Full mixing console |
| ParticleEditorPanel | 1,235 | Multi-emitter, presets, live preview, physics, birth/death events | Complete particle authoring |
| RuntimeInspectorPanel | 1,003 | Live object inspection, property modification during gameplay | Valuable for debugging |
| PerformanceProfilerPanel | 1,296 | Alternative to PerformanceProfiler (duplicate) | Backup implementation |
| ConsolePanel | 821 | Alternative to SimpleConsolePanel (duplicate) | Backup implementation |
| HierarchyPanel | 1,045 | Alternative to SimpleHierarchyPanel (15 engine refs vs 1) | More capable but unused |
| SceneStatsPanel | 273 | Alternative to SceneStatisticsPanel (duplicate) | Backup implementation |
| PlayModeToolbarPanel | ~500 | Play mode controls | Duplicate of inline toolbar |

**Total dead panel code: ~11,804 lines**

---

## Built-Not-Wired Systems (5)

Systems with implementations but NO reference in EditorUI.cpp:

| System | Files | Status |
|--------|-------|--------|
| TerrainEditor | SparkEditor/Source/Terrain/TerrainEditor.h/cpp | Built, no panel, no EditorUI reference |
| LightingTools | SparkEditor/Source/Lighting/LightingTools.h/cpp | Built, no panel, no EditorUI reference |
| GizmoSystem | SparkEditor/Source/Gizmos/GizmoSystem.h/cpp | Built, SceneViewPanel has buttons but NO backend |
| VersionControlSystem | SparkEditor/Source/VersionControl/ | Git/SVN integration, no UI panel |
| EditorPluginManager | SparkEditor/Source/Core/EditorPluginManager.cpp | Plugin loading system, not integrated |
| CollaborativeEditSession | SparkEditor/Source/Communication/ | Multi-user editing, not wired in |

---

## Missing Features (4)

| Feature | Status | Impact |
|---------|--------|--------|
| Shader graph editor | No node-based shader composition exists | Must edit HLSL externally |
| AI debug visualization | No navmesh, behavior tree, or path visualizer | Cannot debug AI in editor |
| Cinematic sequencer editor | Engine has Sequencer.cpp but no editor panel | Cannot author cinematics |
| Project settings panel | No dedicated UI to edit/save project config | Must edit config files manually |

---

## Duplicate Panel Pairs (4)

| Active Panel | Unused Duplicate | Active Lines | Duplicate Lines |
|-------------|-----------------|-------------|-----------------|
| SimpleHierarchyPanel | HierarchyPanel | 283 | 1,045 |
| SimpleConsolePanel | ConsolePanel | 880 | 821 |
| PerformanceProfiler | PerformanceProfilerPanel | 1,210 | 1,296 |
| SceneStatisticsPanel | SceneStatsPanel | 391 | 273 |

In each case, the "Simple" or shorter version is the one actually instantiated.

---

## Key Findings

1. **6 substantial panels (8,869 lines)** are fully implemented but never shown — MaterialEditor, Dialogue, AssetDependency, AudioMixer, Particle, RuntimeInspector
2. **4 duplicate panel pairs** exist — the more capable version is always the unused one
3. **GizmoSystem is disconnected** — buttons exist in SceneViewPanel but no transform manipulation
4. **Version control and collaborative editing** are built but invisible to users
5. **Animation editor** only supports 2D sprites — no skeletal/bone animation editing
6. **Viewport** has no entity picking or selection — renders scene but can't interact with objects

---

## Action Required

**Immediate decision needed** for each built-not-shown panel:
- **Instantiate**: Add to CreatePanels() if the feature is wanted
- **Delete**: Remove if the feature is not needed (per CLAUDE.md: dead code is actively harmful)

**Built-not-wired systems** must be either connected to the editor UI or deleted.
