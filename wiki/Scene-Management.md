# Scene Management

The `SceneManager` handles loading, saving, and manipulating scenes at runtime. Scenes are stored as JSON files with a hierarchical node structure.

**Source:** `SparkEngine/Source/SceneManager/SceneManager.h`

## Scene File Format

Scenes use a JSON format (`.scene` or `.json`):

```json
{
    "metadata": {
        "sceneName": "Level01",
        "author": "Developer",
        "gravity": [0, -9.81, 0]
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
            "parentIndex": 0
        }
    ]
}
```

## Node Types

| Type | Description |
|------|-------------|
| `"cube"` | Unit cube mesh with optional material |
| `"sphere"` | Unit sphere mesh with optional material |
| `"plane"` | Flat plane mesh (XZ-aligned) |
| `"model"` | External mesh loaded from `modelPath` |
| `"light"` | Light source (point, directional, spot) |
| `"trigger"` | Invisible trigger volume |

## Loading Scenes

### Synchronous Loading

```cpp
SceneManager sceneMgr(&graphicsEngine, &inputManager);

if (!sceneMgr.LoadScene(L"Assets/Scenes/Level01.scene")) {
    LOG_ERROR("Failed to load scene");
}
```

### Asynchronous Loading

Load scenes on a background thread to avoid hitches during transitions:

```cpp
sceneMgr.LoadSceneAsync(L"Assets/Scenes/Level02.scene",
    [](bool success, const std::string& sceneName) {
        if (success) {
            LOG("Loaded: " + sceneName);
        }
    });
```

## Hierarchy

Nodes form a parent-child hierarchy using integer indices:
- `parentIndex == -1` denotes a root node
- Each node maintains `childIndices` for efficient tree traversal

```cpp
// Get all root nodes
for (int root : sceneMgr.GetRootNodes()) {
    const SceneNode* node = sceneMgr.GetNode(root);
    LOG("Root: " + node->name);
}

// Navigate children
const SceneNode* parent = sceneMgr.GetNode(0);
for (int childIdx : parent->childIndices) {
    const SceneNode* child = sceneMgr.GetNode(childIdx);
}
```

## Scene Operations

```cpp
// Add a new node
SceneNode node;
node.name = "NewCube";
node.type = "cube";
node.position = {5, 0, 0};
int index = sceneMgr.AddNode(node);

// Remove a node
sceneMgr.RemoveNode(index);

// Save the current scene
sceneMgr.SaveScene(L"Assets/Scenes/Modified.scene");
```

## Prefab System

Save and load reusable prefab templates:

```cpp
// Save a node subtree as a prefab
sceneMgr.SavePrefab(nodeIndex, L"Assets/Prefabs/Enemy.prefab");

// Instantiate a prefab into the scene
int newNodeIndex = sceneMgr.LoadPrefab(L"Assets/Prefabs/Enemy.prefab");
```

## Dirty State Tracking

The scene manager tracks unsaved changes:

```cpp
sceneMgr.MarkDirty();          // Mark scene as modified
bool unsaved = sceneMgr.IsDirty();  // Check for unsaved changes
```

The [editor](SparkEditor) uses this to prompt "Save changes?" before closing or loading a new scene.

## Undo/Redo

Scene history supports undo/redo for editor operations.

## Legacy Format

A legacy binary format is supported via `LoadCustom()` for older scene files.

## Console Commands

```
scene_info          # Show current scene info
scene_list          # List all nodes in hierarchy
scene_load <path>   # Load a scene file
scene_save <path>   # Save current scene
scene_clear         # Clear the scene
```

---

## See Also

- [Entity Component System](Entity-Component-System) — ECS entities created from scene nodes
- [SparkEditor](SparkEditor) — Visual scene hierarchy editor
- [Asset Pipeline](Asset-Pipeline) — Model and asset loading
- [Rendering and Graphics](Rendering-and-Graphics) — Scene rendering pipeline
- [Physics](Physics) — Physics bodies from scene nodes
- [Gameplay Systems](Gameplay-Systems) — Gravity and interactive objects
- [Save System](Save-System) — Saving and restoring scene state
- [Event System](Event-System) — Scene lifecycle events
- [Networking](Networking) — Networked scene synchronization
