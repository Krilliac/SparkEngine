# SparkEditor

The SparkEditor is an ImGui-based visual editor for creating and editing game content. It provides a dockable workspace with multiple panels for [scene editing](Scene-Management), material authoring, [animation](Animation), and debugging.

**Source:** `SparkEditor/Source/`

> **Note:** SparkEditor is Windows-only (requires Win32 + DirectX 11 ImGui backends). It is automatically disabled on non-Windows platforms.

`ENABLE_EDITOR=ON` (Windows only)

## Architecture

```
EditorApplication
  |
  +-- EditorUI
  |     |
  |     +-- EditorLayoutManager   (dock positions, panel visibility, layout save/load)
  |     +-- EditorTheme           (color palette, rounding, spacing, font config)
  |     +-- EditorPanel* panels[] (32+ registered panels)
  |     +-- GizmoSystem           (translate/rotate/scale gizmos via DX11)
  |     +-- UndoRedoManager       (command history stack)
  |     +-- CommandPalette        (Ctrl+P quick-command search)
  |
  +-- EditorPluginManager         (built-in + DLL plugin lifecycle)
  +-- SceneManager                (scene load/save/serialize)
  +-- AssetDatabase               (file-system asset indexing)
  +-- PrefabManager               (prefab asset CRUD)
  +-- VersionControlSystem        (optional VCS integration)
  +-- BuildCookPanel               (cook, package, deploy)
  +-- PerformanceProfiler         (CPU/GPU timing, memory)
```

### EditorApplication

The entry point is `EditorApplication` (`Core/EditorApplication.h`). It owns the Win32 window, DirectX 11 device/context/swap chain, and the ImGui context.

```cpp
struct EditorConfig
{
    std::string projectPath = ".";
    std::string layoutDirectory = "Layouts";
    std::string logDirectory = "Logs";
    bool enableLogging = true;
    bool startMaximized = true;
    float autoSaveInterval = 30.0f;
    int windowWidth = 1600;
    int windowHeight = 900;
};

class EditorApplication
{
public:
    bool Initialize(const EditorConfig& config);
    int  Run();
    void Shutdown();
    bool IsRunning() const;
    void RequestExit();
    PerformanceMetrics GetPerformanceMetrics() const;
    void OnWindowResize(int width, int height);
    void SetWindowTitle(const std::string& title);
};
```

### Frame Loop

Each frame executes the following steps:

1. `ProcessMessages()` -- Win32 message pump (or SDL on Linux stubs)
2. `Update(deltaTime)` -- tick all panels, gizmo system, profiler
3. `Render()` -- ImGui new frame, render all visible panels, ImGui render, DX11 present

## EditorPanel Base Class

Every panel inherits from `EditorPanel` (`Core/EditorPanel.h`), which provides a consistent interface:

```cpp
class EditorPanel
{
public:
    EditorPanel(const std::string& name, const std::string& id);
    virtual ~EditorPanel() = default;

    virtual bool Initialize() = 0;
    virtual void Update(float deltaTime) = 0;
    virtual void Render() = 0;
    virtual void Shutdown() {}
    virtual bool HandleEvent(const std::string& eventType, void* eventData);

    // Visibility and state
    bool IsVisible() const;
    void SetVisible(bool visible);
    bool IsFocused() const;
    void SetFocused(bool focused);
    bool IsClosable() const;
    bool IsModified() const;

    // Persistence
    virtual std::string SaveState() const;
    virtual bool LoadState(const std::string& state);

    // Geometry
    void GetSize(float& width, float& height) const;
    void SetSize(float width, float height);
    void GetPosition(float& x, float& y) const;
    void SetPosition(float x, float y);

protected:
    bool BeginPanel();   // Sets up ImGui window
    void EndPanel();     // Finishes ImGui window
    void NotifyStateChange();
};
```

## Editor Panels

The editor includes 32+ subsystem panels:

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
- Component reflection via `ComponentReflection.h`

### Game Viewport

- Real-time 3D preview of the scene
- Play/pause/stop game simulation via `PlayModeToolbarPanel`
- Camera controls (orbit, pan, fly-through)
- Grid and axis overlays

### Gizmos (GizmoSystem)

The `GizmoSystem` (`Gizmos/GizmoSystem.h`) provides industry-standard 3D object manipulation:

```cpp
enum class GizmoMode
{
    TRANSLATE = 0,   // Move objects
    ROTATE    = 1,   // Rotate objects
    SCALE     = 2,   // Scale objects
    UNIVERSAL = 3    // All operations
};

enum class GizmoSpace
{
    WORLD = 0,       // World coordinate space
    LOCAL = 1        // Local coordinate space
};

enum class GizmoAxis
{
    NONE = 0,
    X = 1,    Y = 2,    Z = 4,
    XY = 3,   XZ = 5,   YZ = 6,   XYZ = 7,
    SCREEN = 8
};
```

Features:
- Axis-constrained and plane-constrained manipulation
- Grid snapping (`SetSnapToGrid(bool)`, `SetSnapSize(float)`)
- Rotation snap (`SetRotationSnapAngle(float)` -- default 15 degrees)
- Adaptive gizmo size based on camera distance
- Multi-object editing support
- Undo/redo integration

### Asset Browser (see [Asset Pipeline](Asset-Pipeline))

- File-system view of project assets
- Drag-and-drop assets into the scene
- Thumbnail previews for textures and models
- Dependency tracking via `AssetDependencyPanel`

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
- Object placement brush via `ObjectPlacementPanel`
- Terrain configuration (size, resolution, LOD)

### Weapon Editor (see [Gameplay Systems](Gameplay-Systems))

- Weapon property configuration
- Projectile type selection
- Fire rate, damage, spread tuning

### Node Graph (imnodes)

- Visual scripting nodes (see [Scripting with AngelScript](Scripting-with-AngelScript))
- Material graph editor
- Connection-based data flow

### Profiler Panel

- CPU and GPU timing breakdown via `PerformanceProfiler`
- Per-system performance graphs
- Memory usage tracking
- Frame time history

### Additional Panels

| Panel | Description |
|-------|-------------|
| `ConsolePanel` | Debug console output window |
| `SimpleConsolePanel` | Lightweight console with command input |
| `SearchPanel` / `CommandPalette` | Ctrl+P quick-search for commands, entities, files |
| `BuildCookPanel` | Build and deployment settings |
| `DedicatedServerPanel` | Server configuration and launch |
| `DialogueEditorPanel` | Dialogue tree authoring |
| `DebugVisualizerPanel` | Debug visualization overlays |
| `SceneStatisticsPanel` / `SceneStatsPanel` | Entity counts, draw calls, triangle budgets |
| `PrefabEditorPanel` | Prefab creation and editing |
| `ParticleEditorPanel` | Particle system authoring |
| `Physics2DPanel` | 2D physics debugging tools |
| `SpriteEditorPanel` | Sprite sheet and atlas editing |
| `SpriteAnimationEditorPanel` | Sprite animation clip authoring |
| `TilemapEditorPanel` | 2D tilemap editing |
| `UndoHistoryPanel` | Visual undo/redo history browser |
| `RuntimeInspectorPanel` | Live property inspection during play mode |
| `FPSToolsPanel` | FPS-specific game design tools |
| `ProjectBrowserPanel` | Project-level file management |
| `AudioMixerPanel` | Audio bus mixing and routing |
| `MaterialEditorPanel` | Material property editor panel |

## Collaborative Editing

SparkEditor supports multi-user collaborative editing sessions where multiple editor instances can work on the same scene simultaneously. See [Collaborative Editing](Collaborative-Editing) for full documentation.

Key features:
- **Peer presence** — see other editors' selections and viewport cameras
- **Node locking** — pessimistic locks prevent conflicting edits
- **Edit broadcasting** — changes are visible across all editors in real-time
- **Auto-expiry** — locks expire after 5 minutes to prevent blocking

Source: `SparkEditor/Source/Communication/CollaborativeEditSession.h`

## Undo/Redo System

The `UndoRedoManager` (`UndoRedo/UndoRedoManager.h`) maintains command stacks for full undo/redo support:

```cpp
class UndoRedoManager
{
public:
    void ExecuteCommand(std::unique_ptr<EditorCommand> command);
    bool Undo();
    bool Redo();
    void UndoToIndex(size_t targetIndex);

    bool CanUndo() const;
    bool CanRedo() const;
    std::string GetUndoDescription() const;
    std::string GetRedoDescription() const;

    void Clear();
    void MarkSaved();
    bool HasUnsavedChanges() const;

    size_t GetMaxStackDepth() const;           // Default: 100
    void SetMaxStackDepth(size_t depth);
    void SetOnStackChanged(std::function<void()> callback);
};
```

All editor operations that modify scene state go through `ExecuteCommand()`. Each command implements `EditorCommand::Execute()` and `EditorCommand::Undo()`. Executing a new command clears the redo stack. The `UndoHistoryPanel` provides a visual timeline of the command history.

## Theme System

The `EditorTheme` class (`Core/EditorTheme.h`) provides professional theming with 8 built-in themes:

| Theme | Description |
|-------|-------------|
| Unity Pro | Unity-inspired dark theme |
| Unreal Pro | Unreal Engine-inspired dark theme |
| VS Pro | Visual Studio-inspired dark theme |
| JetBrains | JetBrains IDE-inspired theme |
| Professional Light | Light theme for well-lit environments |
| High Contrast | Accessibility-focused high-contrast theme |
| Blue Accent | Custom blue accent dark theme |
| Orange Accent | Custom orange accent dark theme |

### Theme Data Structure

The `EditorThemeData` struct contains 60+ color properties organized into groups:

- **Background**: `background`, `backgroundDark`, `backgroundLight`, `backgroundAccent`, `backgroundHeader`, `backgroundActive`, `backgroundHover`, `backgroundSelected`
- **Text**: `text`, `textDisabled`, `textSecondary`, `textAccent`, `textWarning`, `textError`, `textSuccess`
- **Buttons**: `button`, `buttonHovered`, `buttonActive`, `buttonDisabled`
- **Frames**: `frame`, `frameHovered`, `frameActive`
- **Borders**: `border`, `borderLight`, `borderAccent`, `borderSeparator`
- **Tabs/Scrollbars/Menus**: full color sets for each
- **Style values**: rounding, border sizes, padding, spacing, shadows, font size

### Theme Customization

```cpp
// Apply a built-in theme
EditorTheme::ApplyTheme("UnityPro");

// Create a blended theme
EditorTheme::CreateBlendedTheme("UnityPro", "UnrealPro", 0.5f, "HybridTheme");

// Export/import custom themes
ThemeCustomizer::ExportTheme(theme, "MyTheme.json");
ThemeCustomizer::ImportTheme("MyTheme.json", theme);

// Live editing
ThemeCustomizer::ShowThemeEditor();
```

## Layout Management

The `EditorLayoutManager` (`Core/EditorLayoutManager.h`) handles docking, saving, and loading panel layouts:

```cpp
enum class DockPosition { Left, Right, Top, Bottom, Center, Float };

struct PanelConfig
{
    std::string name;
    std::string displayName;
    DockPosition dockPosition;
    ImVec2 size;
    ImVec2 position;
    bool isVisible;
    bool isFloating;
    bool canClose;
    bool canDock;
    float dockRatio;    // Ratio of parent space to occupy
    int tabOrder;
    std::string parentDock;
};
```

The editor uses ImGui's docking branch for a fully customizable workspace:
- Drag panels to dock at any edge
- Create floating windows
- Save and load layout configurations
- Reset to default layout

## Plugin System

The `EditorPluginManager` (`Core/EditorPluginManager.h`) supports both built-in and DLL-loaded editor plugins:

```cpp
class EditorPluginManager
{
public:
    // Registration
    template<typename T> bool RegisterPlugin();
    bool LoadPlugin(const std::string& path);
    bool UnloadPlugin(const std::string& name);

    // Lookup
    IEditorPlugin* GetPlugin(const std::string& name) const;
    size_t GetPluginCount() const;
    std::vector<std::string> GetPluginNames() const;

    // Lifecycle
    bool InitializeAll(EditorApplication* app);
    void ShutdownAll();
    void UpdateAll(float deltaTime);
    void RenderAll();

    // Events
    void NotifySceneLoad(const std::string& scenePath);
    void NotifySceneSave(const std::string& scenePath);
    void NotifyEntitySelected(uint32_t entityID);
    void RenderMenuBarItems();

    // Panel registration from plugins
    void RegisterPanel(std::unique_ptr<EditorPanel> panel);
};
```

Plugins implement the `IEditorPlugin` interface and are loaded at startup or dynamically at runtime. Each plugin can register custom panels, menu items, and respond to editor events.

## Building with the Editor

```powershell
# Windows PowerShell
.\build.ps1 -config Release -editor

# Or via CMake
cmake -B build -DENABLE_EDITOR=ON
cmake --build build --config Release
```

## Performance Considerations

- All panel rendering uses immediate-mode ImGui; no retained scene graph overhead.
- The `PerformanceProfiler` tracks per-panel render time to identify bottlenecks.
- Gizmo rendering uses dedicated DX11 vertex/pixel shaders with minimal draw calls.
- The `EditorApplication` frame loop targets 60 FPS; the editor can optionally cap to lower rates when idle.

## Thread Safety

- All editor UI code runs on the main thread.
- The `EditorApplication` holds the DX11 device and context; rendering is single-threaded.
- Background asset imports (if any) communicate results back to the main thread via queues.
- The `SimpleConsolePanel` wraps the engine's thread-safe `SimpleConsole`.

## Troubleshooting

### Editor window does not appear

1. Verify `ENABLE_EDITOR=ON` is set in CMake.
2. Confirm you are building on Windows with MSVC.
3. Check that DirectX 11 runtime is available (Windows 10+ includes it).

### Panel is missing from the View menu

1. Check that the panel's `SetVisibleInMenu(true)` is called during registration.
2. If the panel was loaded from a plugin, verify the plugin initialized successfully.

### Gizmo does not respond to clicks

1. Ensure the `GizmoSystem::HandleMouseInput()` is receiving mouse events before ImGui consumes them.
2. Check that `GizmoSystem::IsVisible()` returns true.
3. Verify at least one object is selected in the hierarchy.

### Theme colors look wrong

1. Call `EditorTheme::ApplyProfessionalEnhancements()` after applying the theme.
2. Verify custom fonts loaded successfully via `EditorTheme::ApplyCustomFonts()`.

---

## See Also

- [Scene Management](Scene-Management) -- Scene hierarchy and serialization
- [Rendering and Graphics](Rendering-and-Graphics) -- Material system and render pipelines
- [Animation](Animation) -- Animation state machines and timeline
- [Cinematic Sequencer](Cinematic-Sequencer) -- Timeline editor
- [Entity Component System](Entity-Component-System) -- Component architecture
- [Shader Pipeline](Shader-Pipeline) -- Shader authoring and compilation
- [Asset Pipeline](Asset-Pipeline) -- Asset importing and management
- [Physics](Physics) -- Physics simulation
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation) -- Terrain editing and procedural tools
- [Gameplay Systems](Gameplay-Systems) -- Weapons, inventory, and game mechanics
- [Scripting with AngelScript](Scripting-with-AngelScript) -- Visual scripting system

## Editor Panels

<!-- AUTO:panel_list -->
| Panel | Header |
|-------|--------|
| `AIEditorPanel` | `SparkEditor/Source/Panels/AIEditorPanel.h` |
| `AssetBrowserPanel` | `SparkEditor/Source/Panels/AssetBrowserPanel.h` |
| `BuildCookPanel` | `SparkEditor/Source/Panels/BuildCookPanel.h` |
| `ConsolePanel` | `SparkEditor/Source/Panels/ConsolePanel.h` |
| `DebugVisualizerPanel` | `SparkEditor/Source/Panels/DebugVisualizerPanel.h` |
| `DedicatedServerPanel` | `SparkEditor/Source/Panels/DedicatedServerPanel.h` |
| `DialogueEditorPanel` | `SparkEditor/Source/Panels/DialogueEditorPanel.h` |
| `EventMonitorPanel` | `SparkEditor/Source/Panels/EventMonitorPanel.h` |
| `FPSToolsPanel` | `SparkEditor/Source/Panels/FPSToolsPanel.h` |
| `GameViewPanel` | `SparkEditor/Source/Panels/GameViewPanel.h` |
| `HierarchyPanel` | `SparkEditor/Source/Panels/HierarchyPanel.h` |
| `InspectorPanel` | `SparkEditor/Source/Panels/InspectorPanel.h` |
| `LocalizationPanel` | `SparkEditor/Source/Panels/LocalizationPanel.h` |
| `MaterialEditorPanel` | `SparkEditor/Source/Panels/MaterialEditorPanel.h` |
| `ObjectPlacementPanel` | `SparkEditor/Source/Panels/ObjectPlacementPanel.h` |
| `ParticleEditorPanel` | `SparkEditor/Source/Panels/ParticleEditorPanel.h` |
| `Physics2DPanel` | `SparkEditor/Source/Panels/Physics2DPanel.h` |
| `PlayModeToolbarPanel` | `SparkEditor/Source/Panels/PlayModeToolbarPanel.h` |
| `PostProcessingPanel` | `SparkEditor/Source/Panels/PostProcessingPanel.h` |
| `PrefabEditorPanel` | `SparkEditor/Source/Panels/PrefabEditorPanel.h` |
| `ProjectBrowserPanel` | `SparkEditor/Source/Panels/ProjectBrowserPanel.h` |
| `SaveSystemPanel` | `SparkEditor/Source/Panels/SaveSystemPanel.h` |
| `SceneStatisticsPanel` | `SparkEditor/Source/Panels/SceneStatisticsPanel.h` |
| `SceneViewPanel` | `SparkEditor/Source/Panels/SceneViewPanel.h` |
| `SearchPanel` | `SparkEditor/Source/Panels/SearchPanel.h` |
| `SplineEditorPanel` | `SparkEditor/Source/Panels/SplineEditorPanel.h` |
| `SpriteAnimationEditorPanel` | `SparkEditor/Source/Panels/SpriteAnimationEditorPanel.h` |
| `SpriteEditorPanel` | `SparkEditor/Source/Panels/SpriteEditorPanel.h` |
| `TilemapEditorPanel` | `SparkEditor/Source/Panels/TilemapEditorPanel.h` |
| `UndoHistoryPanel` | `SparkEditor/Source/Panels/UndoHistoryPanel.h` |
| `WeaponEditorPanel` | `SparkEditor/Source/Panels/WeaponEditorPanel.h` |
| `WeatherFogPanel` | `SparkEditor/Source/Panels/WeatherFogPanel.h` |
<!-- /AUTO:panel_list -->
