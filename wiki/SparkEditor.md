# SparkEditor

The SparkEditor is an ImGui-based visual editor for creating and editing game content. It provides a dockable workspace with multiple panels for [scene editing](Scene-Management), material authoring, [animation](Animation), and debugging.

**Source:** `SparkEditor/Source/`

> **Note:** SparkEditor is Windows-only (requires Win32 + DirectX 11 ImGui backends). It is automatically disabled on non-Windows platforms.

`ENABLE_EDITOR=ON` (Windows only)

## Editor Panels

The editor includes 22+ subsystem panels:

### Scene Hierarchy

- Tree view of all scene nodes with parent-child relationships
- Drag-and-drop to reparent nodes
- Right-click context menu for add/delete/duplicate
- Search and filter nodes by name or type

### Inspector

- Property editor for the selected node/entity
- Transform editing (position, rotation, scale)
- [Component](Entity-Component-System) editing with type-appropriate widgets
- Material assignment and configuration

### Game Viewport

- Real-time 3D preview of the scene
- Play/pause/stop game simulation
- Camera controls (orbit, pan, fly-through)
- Grid and axis overlays

### Gizmos (ImGuizmo)

- Translate, rotate, and scale gizmos
- World-space and local-space modes
- Snap to grid functionality

### Asset Browser (see [Asset Pipeline](Asset-Pipeline))

- File-system view of project assets
- Drag-and-drop assets into the scene
- Thumbnail previews for textures and models

### Material Editor (see [Shader Pipeline](Shader-Pipeline))

- PBR material authoring
- Texture slot assignment
- Real-time preview sphere
- Blend mode and [shader](Shader-Pipeline) selection

### Animation Timeline

- Keyframe editing on a timeline
- State machine visualization
- Blend tree configuration
- Animation clip preview

### Terrain Editor (see [Terrain and Procedural Generation](Terrain-and-Procedural-Generation))

- Heightmap painting (raise/lower/smooth/flatten)
- Texture splatting brush
- Object placement brush
- Terrain configuration (size, resolution, LOD)

### Weapon Editor (see [Gameplay Systems](Gameplay-Systems))

- Weapon property configuration
- Projectile type selection
- Fire rate, damage, spread tuning

### Node Graph (imnodes)

- Visual scripting nodes
- Material graph editor
- Connection-based data flow

### Profiler Panel

- CPU and GPU timing breakdown
- Per-system performance graphs
- Memory usage tracking
- Frame time history

### Additional Panels

- Console output window
- Log viewer
- Build and deployment settings
- Version control integration
- Level streaming configuration

## Docking and Layout

The editor uses ImGui's docking branch for a fully customizable workspace:
- Drag panels to dock at any edge
- Create floating windows
- Save and load layout configurations
- Multiple theme options

## Building with the Editor

```powershell
# Windows PowerShell
.\build.ps1 -config Release -editor

# Or via CMake
cmake -B build -DENABLE_EDITOR=ON
cmake --build build --config Release
```

---

## See Also

- [Scene Management](Scene-Management) — Scene hierarchy and serialization
- [Rendering and Graphics](Rendering-and-Graphics) — Material system and render pipelines
- [Animation](Animation) — Animation state machines and timeline
- [Cinematic Sequencer](Cinematic-Sequencer) — Timeline editor
- [Entity Component System](Entity-Component-System) — Component architecture
- [Shader Pipeline](Shader-Pipeline) — Shader authoring and compilation
- [Asset Pipeline](Asset-Pipeline) — Asset importing and management
- [Physics](Physics) — Physics simulation
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation) — Terrain editing and procedural tools
- [Gameplay Systems](Gameplay-Systems) — Weapons, inventory, and game mechanics

## Editor Panels

<!-- AUTO:panel_list -->
| Panel | Header |
|-------|--------|
| `AssetBrowserPanel` | `SparkEditor/Source/Panels/AssetBrowserPanel.h` |
| `BuildCookPanel` | `SparkEditor/Source/Panels/BuildCookPanel.h` |
| `ConsolePanel` | `SparkEditor/Source/Panels/ConsolePanel.h` |
| `DebugVisualizerPanel` | `SparkEditor/Source/Panels/DebugVisualizerPanel.h` |
| `FPSToolsPanel` | `SparkEditor/Source/Panels/FPSToolsPanel.h` |
| `GameViewPanel` | `SparkEditor/Source/Panels/GameViewPanel.h` |
| `HierarchyPanel` | `SparkEditor/Source/Panels/HierarchyPanel.h` |
| `InspectorPanel` | `SparkEditor/Source/Panels/InspectorPanel.h` |
| `ObjectPlacementPanel` | `SparkEditor/Source/Panels/ObjectPlacementPanel.h` |
| `Physics2DPanel` | `SparkEditor/Source/Panels/Physics2DPanel.h` |
| `PrefabEditorPanel` | `SparkEditor/Source/Panels/PrefabEditorPanel.h` |
| `ProjectBrowserPanel` | `SparkEditor/Source/Panels/ProjectBrowserPanel.h` |
| `SceneStatisticsPanel` | `SparkEditor/Source/Panels/SceneStatisticsPanel.h` |
| `SceneStatsPanel` | `SparkEditor/Source/Panels/SceneStatsPanel.h` |
| `SceneViewPanel` | `SparkEditor/Source/Panels/SceneViewPanel.h` |
| `SearchPanel` | `SparkEditor/Source/Panels/SearchPanel.h` |
| `SimpleConsolePanel` | `SparkEditor/Source/Panels/SimpleConsolePanel.h` |
| `SimpleHierarchyPanel` | `SparkEditor/Source/Panels/SimpleHierarchyPanel.h` |
| `SpriteAnimationEditorPanel` | `SparkEditor/Source/Panels/SpriteAnimationEditorPanel.h` |
| `SpriteEditorPanel` | `SparkEditor/Source/Panels/SpriteEditorPanel.h` |
| `TilemapEditorPanel` | `SparkEditor/Source/Panels/TilemapEditorPanel.h` |
| `UndoHistoryPanel` | `SparkEditor/Source/Panels/UndoHistoryPanel.h` |
| `WeaponEditorPanel` | `SparkEditor/Source/Panels/WeaponEditorPanel.h` |
<!-- /AUTO:panel_list -->
