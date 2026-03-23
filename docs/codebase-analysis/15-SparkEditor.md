# 15 — SparkEditor

**Location:** `SparkEditor/Source/`

Live visual editor with 32+ specialized panels, collaborative multi-user editing, real-time engine integration, asset management, undo/redo, and scene management. Built on Dear ImGui with D3D11 (Windows) or SDL+OpenGL (Linux).

---

## Architecture

```
SparkEditor/Source/
├── Core/                    — Application, panels, theming, fonts, crash handling
├── Panels/                  — 32+ specialized editor panels
├── SceneSystem/             — Scene load/save/serialize
├── UndoRedo/                — Command-based undo/redo
├── Communication/           — Collaborative editing, engine interface
├── AssetBrowser/            — Asset database and import
├── Profiler/                — Performance metrics
└── Integration/             — Engine integration, console integration
```

---

## EditorApplication — Main Application

**File:** `SparkEditor/Source/Core/EditorApplication.h`

```cpp
SparkEditor::EditorConfig config;
config.projectPath = "MyProject/";
config.windowWidth = 1920;
config.windowHeight = 1080;
config.startMaximized = true;
config.autoSaveInterval = 300.0f;  // 5 minutes

SparkEditor::EditorApplication app;
app.Initialize(config);
app.Run();    // Main event loop
app.Shutdown();
```

- **Windows**: HWND, D3D11 device/context/swapchain
- **Linux**: SDL window + GL context
- ImGui integration with custom theming

---

## EditorPanel — Base Panel Class

**File:** `SparkEditor/Source/Core/EditorPanel.h`

Abstract base for all 32+ editor panels:

```cpp
class EditorPanel {
public:
    virtual void Initialize() = 0;
    virtual void Update(float deltaTime) = 0;
    virtual void Render() = 0;
    virtual void Shutdown() = 0;
    virtual void HandleEvent(EventType type, const EventData& data);

    // State
    bool IsVisible() const;
    void SetVisible(bool visible);
    bool IsFocused() const;
    bool IsModified() const;
    std::string GetName() const;

    // Layout persistence
    void SaveState();
    void LoadState();

protected:
    void BeginPanel();  // ImGui window setup
    void EndPanel();    // ImGui window finalization
};
```

---

## Editor Panels (32+)

### Core Panels

| Panel | Purpose |
|-------|---------|
| **HierarchyPanel** | Scene object tree with drag-drop reordering, multi-select |
| **InspectorPanel** | Property editing for selected objects, per-component renderers |
| **SceneViewPanel** | 3D viewport with gizmos (move/rotate/scale), camera controls |
| **AssetBrowserPanel** | Asset browser with import, thumbnail previews |
| **ProjectBrowserPanel** | Project file browser |
| **ConsolePanel** | Log output and command console |
| **SearchPanel** | Global search across scene, assets, and code |
| **CommandPalette** | Cmd+K quick command search |

### Domain-Specific Panels

| Panel | Purpose |
|-------|---------|
| **AIEditorPanel** | Behavior tree visual editor |
| **AIDebugPanel** | Live AI debugging and visualization |
| **AnimationTimeline** | Animation sequencer with keyframe editing |
| **AnimationClipManager** | Clip management and preview |
| **MaterialEditorPanel** | PBR material editing with live preview |
| **TilemapEditorPanel** | 2D tilemap editing |
| **SpriteEditorPanel** | Sprite editing and atlas management |
| **SpriteAnimationEditorPanel** | Sprite animation sequencing |
| **ParticleEditorPanel** | Particle system visual editor |
| **DialogueEditorPanel** | Branching dialogue graph editor |
| **SplineEditorPanel** | Spline path editing |
| **TerrainEditor** | Terrain sculpting and painting |
| **CinematicSequencerPanel** | Timeline-based cutscene editing |

### System Panels

| Panel | Purpose |
|-------|---------|
| **PostProcessingPanel** | Post-process effect configuration |
| **Physics2DPanel** | 2D physics debugging |
| **Physics3DPanel** | 3D physics debugging and visualization |
| **AudioMixerPanel** | Audio bus mixing and effect chains |
| **LightingToolsPanel** | Light placement and baking tools |
| **WeatherFogPanel** | Weather and fog configuration |
| **PrefabEditorPanel** | Prefab creation and editing |
| **BuildCookPanel** | Build/cook pipeline management |
| **DedicatedServerPanel** | Server configuration tools |
| **DestructionEditorPanel** | Destructible object configuration |
| **ModdingPanel** | Mod management tools |
| **LocalizationPanel** | String table editing |
| **SaveSystemPanel** | Save/load debugging |
| **ReplayPanel** | Replay recording and playback tools |
| **VRConfigPanel** | VR settings configuration |
| **WeaponEditorPanel** | Weapon stats and behavior editing |

### Debug Panels

| Panel | Purpose |
|-------|---------|
| **UndoHistoryPanel** | Undo/redo history visualization |
| **EventMonitorPanel** | Live event bus monitoring |
| **CoroutineDebugPanel** | Active coroutine inspection |
| **FPSToolsPanel** | FPS-specific debugging |
| **StreamingPanel** | Level streaming visualization |
| **PlayModeToolbarPanel** | Play/pause/stop controls |
| **SceneStatisticsPanel** | Entity count, draw calls, memory |
| **GameViewPanel** | Play mode viewport |
| **GameModuleSelectorPanel** | Game module selection |

---

## Collaborative Editing

**File:** `SparkEditor/Source/Communication/CollaborativeEditSession.h`

Multi-user editing with pessimistic locking:

```cpp
struct EditorPeer {
    uint32_t id;
    std::string username;
    uint32_t selectedNode;
    XMFLOAT3 cameraPosition;
    XMFLOAT4 color;  // Display color
};

struct EditMessage {
    EditMessageType type;  // NodeAdded, NodeRemoved, NodeModified, NodeMoved
    uint32_t sourceId;
    uint32_t nodeId;
    std::string property;
    std::string oldValue, newValue;
    double timestamp;
};
```

### Workflow

```cpp
CollaborativeEditSession session;
session.Connect("192.168.1.100", 8080);

// Lock before editing
session.RequestLock(nodeId);
// ... edit properties ...
session.BroadcastEdit({EditMessageType::NodeModified, myId, nodeId, "position", old, new});
session.ReleaseLock(nodeId);

// Receive others' edits
session.SetEditCallback([](const EditMessage& msg) {
    ApplyRemoteEdit(msg);
});

session.Update(deltaTime);
auto peers = session.GetConnectedPeers();
```

Designed for 2-10 concurrent editors.

---

## Scene Management

**File:** `SparkEditor/Source/SceneSystem/SceneManager.h`

```cpp
SceneManager scenes;
scenes.Initialize();

scenes.CreateNewScene("MyLevel");
scenes.SaveScene("Data/Scenes/MyLevel.scene");
scenes.LoadScene("Data/Scenes/MyLevel.scene");

bool unsaved = scenes.HasUnsavedChanges();
```

---

## Undo/Redo

**File:** `SparkEditor/Source/UndoRedo/UndoRedoManager.h`

Command pattern for reversible operations:

```cpp
UndoRedoManager& undo = UndoRedoManager::GetInstance();

// Execute and record
undo.ExecuteCommand(std::make_unique<MoveObjectCommand>(objectId, oldPos, newPos));

// Navigate history
undo.Undo();
undo.Redo();
undo.UndoToIndex(5);  // Jump to specific point

std::string desc = undo.GetUndoDescription();  // "Move Object"
auto& stack = undo.GetUndoStack();
```

---

## Asset Database

**File:** `SparkEditor/Source/AssetBrowser/AssetDatabase.h`

```cpp
struct AssetInfo {
    std::string path, name;
    AssetType type;
    std::string guid;
    size_t size;
    time_t modTime;
    bool imported;
    std::vector<std::string> dependencies;
};
```

Features:
- Real-time file system monitoring
- Auto-import on file change
- Dependency tracking
- GUID-based lookup
- Import settings per asset type (texture, model, audio)

---

## Engine Integration

**File:** `SparkEditor/Source/Integration/SparkEngineIntegration.h`

Real-time sync between editor and running engine:

```cpp
struct EngineState {
    bool running;
    bool paused;
    float fps, frameTime;
    size_t memoryUsage;
    uint32_t drawCalls, triangles, objects;
    XMFLOAT3 cameraPosition, cameraRotation;
};

// Connection states
enum class EngineConnectionStatus {
    Disconnected, Connecting, Connected, ConnectionLost, ConnectionError
};
```

---

## Plugin System

**File:** `SparkEditor/Source/Core/EditorPluginManager.h`

Extensible editor via plugins:

```cpp
EditorPluginManager plugins;
plugins.LoadPlugin("MyEditorPlugin.dll");
plugins.InitializeAll();
plugins.UpdateAll(deltaTime);
```
