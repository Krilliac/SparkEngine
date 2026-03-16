# Editor Functionality Status

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Mostly Resolved
**Severity:** Medium

## Description

Comprehensive audit of 30 editor features. 24 panels/features are now working. All 10 previously built-not-shown panels have been resolved (4 restored, 6 deleted). All 4 duplicate panel pairs resolved. 5 systems remain built-not-wired, 4 features missing.

---

## Working Features (21)

| Feature | Panel | Lines | Engine Refs | Notes |
|---------|-------|-------|-------------|-------|
| Viewport/scene view | SceneViewPanel | 404 | D3D11 setup | Render target, camera controls, render modes. Gap: no entity picking |
| Game view | GameViewPanel | — | Direct | Renders game camera with input handling and cursor capture |
| Entity inspector | InspectorPanel | 1,303 | 18 | Full component editing with undo/redo via CommandHistory |
| Scene hierarchy | HierarchyPanel | 1,045 | 1 | Full tree with drag-drop, undo, multi-select (replaced SimpleHierarchyPanel) |
| Asset browser | AssetBrowserPanel | 457 | 2 | File/folder browser, thumbnails, drag-and-drop |
| Physics debug | DebugVisualizerPanel + Physics2DPanel | 245+308 | 1 each | Wireframe physics, collision shapes |
| Console | ConsolePanel | 821 | 9 | Advanced logging, filtering, export, command execution (replaced SimpleConsolePanel) |
| Profiler | PerformanceProfiler | 1,210 | 1-2 | Frame time graphs, CPU/GPU timing, memory |
| Animation editor | SpriteAnimationEditorPanel | 534 | 1 | Frame-by-frame sprite animation only (no skeletal) |
| Play mode (PIE) | PlayModeManager | — | Direct | Play/Pause/Stop, F5 hotkey, scene snapshot/restore |
| Undo/redo | UndoRedoManager + UndoHistoryPanel | 161 | Deep | Command history tracking, Inspector integration |
| Build/export | BuildCookPanel | 591 | 3 | Build config, platform targeting, cook/package |
| Scene statistics | SceneStatisticsPanel | 391 | 2 | Object/triangle/vertex counts, memory, draw calls |
| Toolbar/menu | RenderMainMenuBar() + RenderToolbar() | — | Direct | File/Edit/View/GameObject/Tools/Help menus |
| Search | SearchPanel | — | — | Ctrl+F or Window menu, entity/asset search |
| Prefab system | PrefabManager + PrefabEditorPanel | — | — | Prefab creation, editing, instantiation |
| Project management | ProjectManager + ProjectBrowserPanel | — | — | Project creation and loading |
| 2D editors | SpriteEditor, TilemapEditor, Physics2DPanel | — | — | Sprite, tilemap, 2D physics editing |
| FPS tools | FPSToolsPanel + WeaponEditorPanel | — | — | FPS gameplay and weapon editing tools |
| Object placement | ObjectPlacementPanel | — | — | Object placement tools (hidden by default) |
| Gizmos (UI only) | SceneViewPanel buttons | — | — | Mode buttons exist but GizmoSystem NOT connected |

---

## Built-Not-Shown — All Resolved

All 10 previously unused panels have been either **restored and wired in** or **deleted**:

| Panel | Action | Session |
|-------|--------|---------|
| MaterialEditorPanel | **RESTORED** — wired into EditorUI | 2026-03-16 |
| HierarchyPanel | **RESTORED** — replaced SimpleHierarchyPanel | 2026-03-16 |
| PlayModeToolbarPanel | **RESTORED** — wired into EditorUI | 2026-03-16 |
| ConsolePanel | **RESTORED** — replaced SimpleConsolePanel | 2026-03-16 |
| DialogueEditorPanel | **DELETED** — never wired in | Prior session |
| AssetDependencyPanel | **DELETED** — never wired in | Prior session |
| AudioMixerPanel | **DELETED** — never wired in | Prior session |
| PerformanceProfilerPanel | **DELETED** — never wired in | Prior session |
| ParticleEditorPanel | **DELETED** — never wired in | Prior session |
| RuntimeInspectorPanel | **DELETED** — never wired in | Prior session |

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

## Duplicate Panel Pairs — All Resolved

All 4 duplicate pairs have been resolved:

| Winner | Deleted Duplicate | Resolution |
|--------|-------------------|------------|
| HierarchyPanel | SimpleHierarchyPanel | Superior version now active |
| ConsolePanel | SimpleConsolePanel | Superior version now active |
| PerformanceProfiler | PerformanceProfilerPanel | Duplicate deleted |
| SceneStatisticsPanel | SceneStatsPanel | Duplicate deleted |

---

## Key Findings

1. **All 10 built-not-shown panels resolved** — 4 restored, 6 deleted
2. **All 4 duplicate panel pairs resolved** — superior version now active in each case
3. **GizmoSystem is disconnected** — buttons exist in SceneViewPanel but no transform manipulation
4. **Version control and collaborative editing** are built but invisible to users
5. **Animation editor** only supports 2D sprites — no skeletal/bone animation editing
6. **Viewport** has no entity picking or selection — renders scene but can't interact with objects

---

## Remaining Action Items

- **GizmoSystem**: Connect to SceneViewPanel or delete
- **Built-not-wired systems** (TerrainEditor, LightingTools, VersionControlSystem, CollaborativeEditSession, EditorPluginManager): Either connect or delete
