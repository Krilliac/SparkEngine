# Scene Management

The `SceneManager` handles loading, saving, and manipulating scenes at runtime. Scenes are stored as JSON files with a hierarchical node structure. The SceneManager is the single point of truth for the live scene graph.

> **stable-v1 support boundary:** [`stable-v1`](../../docs/site/readiness.json) is blocked and uncertified; its exact host is Windows 11 x64. This page describes source-level scene behavior, including paths that are outside that profile, and does not certify a release or platform.

**Source:** `SparkEngine/Source/SceneManager/SceneManager.h`

## Overview

The SceneManager owns:
- A flat list of `SceneNode` objects (`m_sceneNodes`) that define the hierarchy
- The corresponding instantiated `GameObject` objects used at runtime
- Global scene settings via `SceneMetadata`

### Responsibilities

| Responsibility | Description |
|----------------|-------------|
| **Serialization** | JSON and legacy binary round-trip via `LoadJSON`/`SaveJSON`/`LoadCustom` |
| **Hierarchy management** | Add, remove, reparent nodes; maintain index invariants |
| **Prefab system** | Save/load subtrees as reusable prefab assets |
| **Async loading** | Background scene transitions via `LoadSceneAsync` |
| **Console integration** | Runtime inspection and manipulation from the debug console |
| **Dirty tracking** | Tracks unsaved changes for editor "Save changes?" prompts |

## Scene File Format

Scenes use a JSON format (`.scene` or `.json`):

```json
{
    "metadata": {
        "sceneName": "Level01",
        "author": "Developer",
        "version": "1.0",
        "description": "First level of the game",
        "ambientLightR": 0.1,
        "ambientLightG": 0.1,
        "ambientLightB": 0.15,
        "gravityX": 0.0,
        "gravityY": -9.81,
        "gravityZ": 0.0
    },
    "nodes": [
        {
            "name": "Ground",
            "type": "plane",
            "position": [0, 0, 0],
            "rotation": [0, 0, 0],
            "scale": [10, 1, 10],
            "parentIndex": -1
        },
        {
            "name": "Box",
            "type": "cube",
            "position": [0, 1, 0],
            "rotation": [0, 45, 0],
            "scale": [1, 1, 1],
            "parentIndex": 0
        }
    ]
}
```

## SceneMetadata

The `SceneMetadata` struct holds global scene settings:

```cpp
struct SceneMetadata
{
    std::string sceneName;      // Display name ("Level01")
    std::string author;         // Author (informational)
    std::string version;        // Format version ("1.0")
    std::string description;    // Human-readable description

    float ambientLightR = 0.1f; // Ambient light red [0,1]
    float ambientLightG = 0.1f; // Ambient light green [0,1]
    float ambientLightB = 0.1f; // Ambient light blue [0,1]

    float gravityX = 0.0f;     // Gravity X (m/s^2)
    float gravityY = -9.81f;   // Gravity Y (m/s^2), default Earth
    float gravityZ = 0.0f;     // Gravity Z (m/s^2)
};
```

The ambient light fields control the global ambient pass in the renderer. The gravity fields are forwarded to the Jolt physics world on scene load.

## SceneNode

Each object in a scene is represented as a `SceneNode`:

```cpp
struct SceneNode
{
    std::string name;           // Unique name within the scene
    std::string type;           // Node type (see table below)
    XMFLOAT3 position;         // World-space position
    XMFLOAT3 rotation;         // Euler rotation (degrees)
    XMFLOAT3 scale;            // Scale multiplier (default 1,1,1)
    std::string modelPath;     // Mesh asset path (for "model" type)
    std::string materialPath;  // Material override path
    int parentIndex;           // Parent index (-1 = root)
    std::vector<int> childIndices;  // Direct children
    std::unordered_map<std::string, std::string> properties;  // Custom key-value pairs
};
```

## Node Types

| Type | Description | Instantiation |
|------|-------------|---------------|
| `"cube"` | Unit cube mesh with optional material | `Primitives::CreateCube()` |
| `"sphere"` | Unit sphere mesh with optional material | `Primitives::CreateSphere()` |
| `"plane"` | Flat plane mesh (XZ-aligned) | `Primitives::CreatePlane()` |
| `"model"` | Mesh requested from `modelPath` | `.obj` through `Mesh`/tinyobjloader; unsupported, missing, or failed paths use a placeholder mesh |
| `"light"` | Light source (point, directional, spot) | Creates `LightComponent` |
| `"trigger"` | Invisible trigger volume | Creates `ColliderComponent` (trigger) |

Unrecognized types are silently skipped during `InstantiateNodes()` for forward compatibility.

### Custom Properties

Nodes can carry arbitrary key-value properties:

```cpp
node.properties["health"] = "100";
node.properties["faction"] = "enemy";
node.properties["lightType"] = "directional";
node.properties["intensity"] = "1.5";
node.properties["spawnDelay"] = "3.0";
```

Properties are round-tripped through JSON unchanged. Game code reads them to configure gameplay behavior.

## Loading Scenes

### Synchronous Loading

```cpp
SceneManager sceneMgr(&graphicsEngine, &inputManager);

if (!sceneMgr.LoadScene(L"Assets/Scenes/Level01.scene")) {
    LOG_ERROR("Failed to load scene");
}
```

File format is determined by extension:
- `.scene` -- legacy binary format (`LoadCustom`)
- `.json` or anything else -- JSON format (`LoadJSON`)

### Asynchronous Loading

Load scenes on a background thread to avoid hitches during transitions:

```cpp
sceneMgr.LoadSceneAsync(L"Assets/Scenes/Level02.scene",
    [](bool success, const std::string& sceneName) {
        if (success) {
            LOG("Loaded: " + sceneName);
        } else {
            LOG_ERROR("Failed to load: " + sceneName);
        }
    });
```

**Warning:** Do not call other `SceneManager` methods while an async load is in flight, as node data may be partially overwritten. The transition to the new scene (clearing the old one and instantiating nodes) happens on the main thread at the start of the next frame after loading is done.

### Loading Flow Diagram

```
LoadScene(filepath)
    │
    ├── Clear existing scene (destroy GameObjects, clear nodes)
    │
    ├── Determine format from extension
    │   ├── .scene  →  LoadCustom(filepath)   [legacy binary]
    │   └── .json   →  LoadJSON(filepath)     [JSON]
    │
    ├── Parse file into m_metadata + m_sceneNodes
    │
    ├── InstantiateNodes()
    │   └── For each node: create GameObject based on type
    │       ├── cube/sphere/plane → Primitives factory
    │       ├── model → LoadOrPlaceholderMesh(modelPath): OBJ or placeholder
    │       ├── light → Create LightComponent
    │       └── trigger → Create ColliderComponent (isTrigger=true)
    │
    ├── Clear dirty flag
    │
    └── Publish SceneLoadedEvent via EventBus
```

## Hierarchy

Nodes form a parent-child hierarchy using integer indices:
- `parentIndex == -1` denotes a root node
- Each node maintains `childIndices` for efficient tree traversal
- Parent-child relationships are encoded by index rather than pointers so the node vector can be freely resized

```cpp
// Get all root nodes
for (int root : sceneMgr.GetRootNodes()) {
    const SceneNode* node = sceneMgr.GetNode(root);
    LOG("Root: " + node->name);
}

// Navigate children recursively
void PrintHierarchy(const SceneManager& mgr, int index, int depth = 0)
{
    const SceneNode* node = mgr.GetNode(index);
    std::string indent(depth * 2, ' ');
    LOG(indent + node->name + " [" + node->type + "]");
    for (int childIdx : node->childIndices)
    {
        PrintHierarchy(mgr, childIdx, depth + 1);
    }
}

for (int root : sceneMgr.GetRootNodes())
    PrintHierarchy(sceneMgr, root);
```

## Scene Operations

### Adding Nodes

```cpp
SceneNode node;
node.name = "NewCube";
node.type = "cube";
node.position = {5, 0, 0};
int index = sceneMgr.AddNode(node);
```

The node is pushed to the back of the node list. If `node.parentIndex >= 0`, the new node is added to the parent's `childIndices`. The dirty flag is set automatically.

### Removing Nodes

```cpp
sceneMgr.RemoveNode(index);
```

Recursively removes the node and all descendants. Indices of remaining nodes are updated to preserve consistency. **Warning:** Previously captured node indices may become stale after removal.

### Reparenting Nodes

```cpp
// Make node a child of another node
sceneMgr.SetParent(childIndex, parentIndex);

// Detach node (make it a root)
sceneMgr.SetParent(childIndex, -1);
```

### Saving Scenes

```cpp
sceneMgr.SaveScene(L"Assets/Scenes/Modified.scene");
```

## SceneManager API Reference

### Construction and Lifecycle

| Method | Description |
|--------|-------------|
| `SceneManager(GraphicsEngine*, InputManager*)` | Construct with engine subsystems |
| `~SceneManager()` | Join async thread, destroy GameObjects |
| `void NewScene(const string& name = "Untitled")` | Clear and start a new empty scene |
| `void Clear()` | Destroy all objects without resetting metadata |

### Loading and Saving

| Method | Description |
|--------|-------------|
| `bool LoadScene(const wstring& filepath)` | Load scene synchronously (returns false on error) |
| `bool SaveScene(const wstring& filepath) const` | Save scene to JSON (returns false on I/O error) |
| `void LoadSceneAsync(const wstring& filepath, SceneLoadCallback)` | Load on background thread |

### Hierarchy Operations

| Method | Description |
|--------|-------------|
| `int AddNode(const SceneNode& node)` | Append a node, return its index |
| `void RemoveNode(int index)` | Remove node and all descendants |
| `void SetParent(int childIndex, int parentIndex)` | Reparent a node (-1 = root) |
| `vector<int> GetRootNodes() const` | Get indices of all root nodes |
| `const SceneNode* GetNode(int index) const` | Get node by index (const) |
| `SceneNode* GetNode(int index)` | Get node by index (mutable) |
| `int FindNode(const string& name) const` | Find node index by name (-1 if not found) |
| `int GetNodeCount() const` | Total number of nodes |

### Prefab System

| Method | Description |
|--------|-------------|
| `bool SavePrefab(int nodeIndex, const wstring& filepath) const` | Save subtree as prefab |
| `int LoadPrefab(const wstring& filepath, const XMFLOAT3& pos)` | Instantiate prefab at position |

### State and Metadata

| Method | Description |
|--------|-------------|
| `bool IsDirty() const` | Check for unsaved changes |
| `void MarkDirty()` | Explicitly mark as modified |
| `const SceneMetadata& GetMetadata() const` | Read-only metadata access |
| `SceneMetadata& GetMetadata()` | Mutable metadata access |
| `const wstring& GetCurrentFilePath() const` | Last loaded/saved file path |

### Console Integration

| Method | Description |
|--------|-------------|
| `string Console_ListNodes() const` | Formatted hierarchy tree |
| `string Console_GetNodeInfo(int index) const` | Detailed node information |
| `bool Console_MoveNode(int index, float x, y, z)` | Move node to position |
| `bool Console_RenameNode(int index, const string& newName)` | Rename a node |

## Creating a Complete Scene Programmatically

```cpp
SceneManager sceneMgr(&graphicsEngine, &inputManager);
sceneMgr.NewScene();

// Set scene metadata
SceneMetadata& meta = sceneMgr.GetMetadata();
meta.sceneName    = "TestArena";
meta.author       = "Developer";
meta.gravityY     = -9.81f;
meta.ambientLightR = 0.2f;
meta.ambientLightG = 0.2f;
meta.ambientLightB = 0.3f;

// Add a ground plane
SceneNode ground;
ground.name     = "Ground";
ground.type     = "plane";
ground.position = {0.0f, 0.0f, 0.0f};
ground.scale    = {50.0f, 1.0f, 50.0f};
int groundIdx   = sceneMgr.AddNode(ground);

// Add a directional light
SceneNode light;
light.name     = "SunLight";
light.type     = "light";
light.position = {0.0f, 20.0f, 0.0f};
light.properties["lightType"] = "directional";
light.properties["intensity"] = "1.5";
light.properties["color"] = "1.0,0.95,0.9";
sceneMgr.AddNode(light);

// Add a model as a child of the ground
SceneNode enemy;
enemy.name      = "EnemySpawn";
enemy.type      = "model";
enemy.modelPath = "Assets/Models/Enemy.obj";
enemy.position  = {10.0f, 0.0f, 5.0f};
int enemyIdx    = sceneMgr.AddNode(enemy);
sceneMgr.SetParent(enemyIdx, groundIdx);  // Parent to ground

// Add a trigger volume for area detection
SceneNode trigger;
trigger.name     = "ExitTrigger";
trigger.type     = "trigger";
trigger.position = {0.0f, 1.0f, 25.0f};
trigger.scale    = {5.0f, 3.0f, 1.0f};
trigger.properties["onEnter"] = "ExitLevel";
sceneMgr.AddNode(trigger);

// Save the scene
sceneMgr.SaveScene(L"Assets/Scenes/TestArena.scene");
```

## Finding Nodes

```cpp
// Find a node by name (linear search, returns index or -1)
int sunIdx = sceneMgr.FindNode("SunLight");
if (sunIdx >= 0) {
    const SceneNode* sun = sceneMgr.GetNode(sunIdx);
    LOG("Sun position: " + std::to_string(sun->position.y));
}

// Get total node count
int count = sceneMgr.GetNodeCount();

// List available scene files in a directory
auto scenes = sceneMgr.GetAvailableScenes(L"Assets/Scenes");
for (const auto& path : scenes) {
    LOG("Scene: " + path);
}
```

## Prefab System

Save and load reusable prefab templates:

```cpp
// Save a node subtree as a prefab
sceneMgr.SavePrefab(nodeIndex, L"Assets/Prefabs/Enemy.prefab");

// Instantiate a prefab into the scene at a specific position
int newNodeIndex = sceneMgr.LoadPrefab(
    L"Assets/Prefabs/Enemy.prefab",
    {10.0f, 0.0f, 5.0f}  // World-space offset
);

if (newNodeIndex < 0) {
    LOG_ERROR("Failed to load prefab");
}
```

Prefab files use the same JSON format as scene files but contain only the exported subtree. Node positions are stored relative to the prefab's internal origin and offset by the `position` parameter during instantiation.

## Dirty State Tracking

The scene manager tracks unsaved changes:

```cpp
sceneMgr.MarkDirty();               // Mark scene as modified
bool unsaved = sceneMgr.IsDirty();  // Check for unsaved changes
```

Methods that automatically set the dirty flag:
- `AddNode()`, `RemoveNode()`, `SetParent()`
- `Console_MoveNode()`, `Console_RenameNode()`

Methods that clear the dirty flag:
- `LoadScene()`, `SaveScene()`, `NewScene()`

The [editor](../gameplay-tools/SparkEditor.md) uses `IsDirty()` to prompt "Save changes?" before closing or loading a new scene.

## Undo/Redo

Scene history supports undo/redo for editor operations. The undo stack captures snapshots of the node list before each modifying operation.

## Legacy Format

A legacy binary format is supported via `LoadCustom()` for older `.scene` files. The binary format is engine-version-specific. New scenes should always use the JSON format.

## Console Commands

```
scene_info          # Show current scene metadata (name, author, node count)
scene_list          # List all nodes in hierarchy with tree indentation
scene_load <path>   # Load a scene file (synchronous)
scene_save <path>   # Save current scene to file
scene_clear         # Clear the scene (destroy all nodes)
scene_node <index>  # Show detailed info for a specific node
scene_move <index> <x> <y> <z>    # Move a node to a new position
scene_rename <index> <name>       # Rename a node
scene_parent <child> <parent>     # Reparent a node (-1 = root)
```

## Integration with Other Systems

### ECS Integration

Scene nodes are converted to ECS entities during `InstantiateNodes()`. Each node creates an entity with:
- `NameComponent` (from `node.name`)
- `Transform` (from `node.position`, `rotation`, `scale`)
- Type-specific components (`MeshRenderer`, `LightComponent`, `ColliderComponent`)

### Physics Integration

- Gravity settings from `SceneMetadata` are applied to the Jolt physics world
- Trigger nodes create `ColliderComponent` with `isTrigger = true`
- Nodes with physics properties create `RigidBodyComponent`

### Event Integration

The scene manager publishes events through the [Event System](Event-System.md):
- `SceneLoadedEvent` when a scene finishes loading
- `SceneUnloadedEvent` when a scene is cleared

---

## See Also

- [Entity Component System](Entity-Component-System.md) -- ECS entities created from scene nodes
- [SparkEditor](../gameplay-tools/SparkEditor.md) -- Visual scene hierarchy editor
- [Asset Pipeline](../gameplay-tools/Asset-Pipeline.md) -- Model and asset loading
- [Rendering and Graphics](Rendering-and-Graphics.md) -- Scene rendering pipeline
- [Physics](Physics.md) -- Physics bodies from scene nodes
- [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md) -- Gravity and interactive objects
- [Save System](../gameplay-tools/Save-System.md) -- Saving and restoring scene state
- [Event System](Event-System.md) -- Scene lifecycle events
- [Networking](Networking.md) -- Networked scene synchronization
