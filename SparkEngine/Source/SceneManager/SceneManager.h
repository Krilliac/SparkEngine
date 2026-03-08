/**
 * @file SceneManager.h
 * @brief Scene management with serialization, hierarchy, and prefab support
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * This file declares the `SceneManager` — Spark Engine's central authority for
 * loading, saving, and manipulating scenes at runtime. A *scene* is a flat list
 * of `SceneNode` objects that form a logical parent-child hierarchy (encoded via
 * integer indices) plus a `SceneMetadata` block with global scene settings.
 *
 * ## Scene file format
 * Scenes are stored as JSON (`.scene` or `.json`) with the following top-level keys:
 * ```json
 * {
 *   "metadata": { "sceneName": "Level01", "author": "...", "gravity": [0,-9.81,0] },
 *   "nodes": [
 *     { "name": "Ground", "type": "plane", "position": [0,0,0], "parentIndex": -1 },
 *     { "name": "Box",    "type": "cube",  "position": [0,1,0], "parentIndex":  0 }
 *   ]
 * }
 * ```
 * A legacy binary format is also supported via `LoadCustom()` for older scene files.
 *
 * ## Hierarchy design
 * Nodes reference their parent by index rather than by pointer so that the node
 * vector can be freely resized. `parentIndex == -1` denotes a root node. Each node
 * also maintains `childIndices` so that tree traversal is O(children) rather than
 * O(total nodes).
 *
 * ## Editor integration
 * The dirty-state flag (`m_dirty`) is toggled by `MarkDirty()` whenever the hierarchy
 * changes. The editor polls `IsDirty()` to decide whether to show an unsaved-changes
 * prompt before closing or loading a new scene.
 *
 * ## Async loading
 * `LoadSceneAsync()` loads the scene on a background thread and invokes a
 * `SceneLoadCallback` on completion. This prevents hitches during transitions.
 *
 * ## Usage example
 * @code
 *   SceneManager sceneMgr(&graphicsEngine, &inputManager);
 *
 *   // Load a scene synchronously:
 *   if (!sceneMgr.LoadScene(L"Assets/Scenes/Level01.scene"))
 *       LOG_ERROR("Failed to load scene");
 *
 *   // Query the hierarchy:
 *   for (int root : sceneMgr.GetRootNodes()) {
 *       const SceneNode* node = sceneMgr.GetNode(root);
 *       LOG("Root: " + node->name);
 *   }
 *
 *   // Async transition to Level02:
 *   sceneMgr.LoadSceneAsync(L"Assets/Scenes/Level02.scene",
 *       [](bool ok, const std::string& name) {
 *           if (ok) LOG("Loaded: " + name);
 *       });
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <filesystem>
#include <unordered_map>
#include <thread>


class GraphicsEngine;
class InputManager;
class GameObject;

// =============================================================================
// SceneNode
// =============================================================================

/**
 * @brief A single node in the scene object hierarchy.
 *
 * Every visible or logical object in a scene is represented as a `SceneNode`.
 * The node stores enough data to reconstruct the object at load time and to
 * serialize it back to disk.
 *
 * ### Node types
 * The `type` field determines how the node is instantiated by `InstantiateNodes()`:
 *
 * | `type` value | Instantiated as                              |
 * |--------------|----------------------------------------------|
 * | `"cube"`     | Unit cube mesh with optional material        |
 * | `"sphere"`   | Unit sphere mesh with optional material      |
 * | `"plane"`    | Flat plane mesh (XZ-aligned)                 |
 * | `"model"`    | External mesh loaded from `modelPath`        |
 * | `"light"`    | Dynamic light (properties carry light data)  |
 * | `"trigger"`  | Invisible trigger volume (no mesh)           |
 *
 * ### Custom properties
 * The `properties` map carries arbitrary key-value pairs that game code and the
 * editor may attach to nodes. For example:
 * @code
 *   node.properties["health"] = "100";
 *   node.properties["faction"] = "enemy";
 * @endcode
 * These are round-tripped through JSON unchanged.
 *
 * ### Hierarchy encoding
 * Parent-child relationships are encoded by integer index rather than pointers
 * so that the flat `m_sceneNodes` vector can be resized safely. The invariant
 * `parentIndex == -1` marks a root node. `childIndices` is always kept in sync
 * by `AddNode()`, `RemoveNode()`, and `SetParent()`.
 */
struct SceneNode
{
    /**
     * @brief Human-readable name of this node (must be unique within the scene).
     *
     * Used by `FindNode()` and displayed in the editor hierarchy panel.
     */
    std::string name;

    /**
     * @brief Object type string that controls instantiation (e.g. "cube", "model").
     *
     * See the node-type table above for recognized values. Unrecognized types are
     * silently skipped during `InstantiateNodes()` to allow forward compatibility.
     */
    std::string type;

    /**
     * @brief World-space position of this node's origin.
     *
     * Applied to the object's `Transform` component during instantiation.
     */
    DirectX::XMFLOAT3 position = {0, 0, 0};

    /**
     * @brief World-space Euler rotation (pitch, yaw, roll) in degrees.
     *
     * Converted to a quaternion during instantiation. Applied after position.
     */
    DirectX::XMFLOAT3 rotation = {0, 0, 0};

    /**
     * @brief World-space scale multiplier (X, Y, Z).
     *
     * Default (1, 1, 1) means no scale applied. Non-uniform scaling is supported
     * but may cause visual artifacts on normal-mapped surfaces.
     */
    DirectX::XMFLOAT3 scale = {1, 1, 1};

    /**
     * @brief File-system path to the mesh asset (used when `type == "model"`).
     *
     * Relative to the project asset root (e.g. `"Assets/Meshes/Tree.fbx"`).
     * Ignored for primitive node types.
     */
    std::string modelPath;

    /**
     * @brief File-system path to the material asset, overriding the mesh default.
     *
     * If empty, the default material embedded in the mesh file (or the engine
     * default material) is used.
     */
    std::string materialPath;

    /**
     * @brief Index of the parent node, or -1 if this node is a root.
     *
     * Do not modify directly; use `SceneManager::SetParent()` to keep
     * `childIndices` consistent.
     */
    int parentIndex = -1;

    /**
     * @brief Indices of all direct children of this node.
     *
     * Maintained automatically by `SceneManager::AddNode()`, `RemoveNode()`,
     * and `SetParent()`. Do not modify directly.
     */
    std::vector<int> childIndices;

    /**
     * @brief Arbitrary key-value properties attached to this node.
     *
     * Game code may read and write these freely. The editor exposes them in the
     * property inspector. Both keys and values are UTF-8 strings; numeric values
     * must be converted by the caller.
     */
    std::unordered_map<std::string, std::string> properties;
};

// =============================================================================
// SceneMetadata
// =============================================================================

/**
 * @brief Global metadata and environment settings for a scene.
 *
 * Stored in the `metadata` block of the scene JSON file and loaded alongside
 * the node list. These fields control engine-wide settings that apply to the
 * whole scene rather than individual objects.
 *
 * The `ambientLight*` fields set the RGB contribution of the global ambient
 * pass in the renderer (typically kept low, e.g. 0.05–0.15 per channel).
 *
 * The `gravity*` fields are forwarded to the Bullet physics world on scene load.
 * Standard Earth gravity is (0, -9.81, 0).
 */
struct SceneMetadata
{
    /** @brief Display name of the scene (shown in editor title bar and save dialogs). */
    std::string sceneName;

    /** @brief Author of the scene (informational only, not used by the engine). */
    std::string author;

    /**
     * @brief Scene format version string (e.g. "1.0").
     *
     * Used by the loader to select the appropriate deserialization path when the
     * format changes across engine versions.
     */
    std::string version;

    /** @brief Human-readable description of the scene (not used by the engine). */
    std::string description;

    /** @brief Red channel of the global ambient light colour. Range [0, 1]. Default 0.1. */
    float ambientLightR = 0.1f;

    /** @brief Green channel of the global ambient light colour. Range [0, 1]. Default 0.1. */
    float ambientLightG = 0.1f;

    /** @brief Blue channel of the global ambient light colour. Range [0, 1]. Default 0.1. */
    float ambientLightB = 0.1f;

    /** @brief X component of the gravity vector (m/s²). Typically 0. */
    float gravityX = 0.0f;

    /** @brief Y component of the gravity vector (m/s²). Default -9.81 (Earth). */
    float gravityY = -9.81f;

    /** @brief Z component of the gravity vector (m/s²). Typically 0. */
    float gravityZ = 0.0f;
};

// =============================================================================
// SceneLoadCallback
// =============================================================================

/**
 * @brief Completion callback for asynchronous scene loads.
 *
 * Invoked by `LoadSceneAsync()` from the background loading thread once the
 * scene is fully loaded (or has failed). The callback should be thread-safe or
 * post work to the main thread via a queue.
 *
 * @param success   `true` if the scene loaded successfully, `false` on any error.
 * @param sceneName The `SceneMetadata::sceneName` of the loaded scene, or the
 *                  file path string on failure.
 *
 * @code
 *   SceneLoadCallback cb = [](bool ok, const std::string& name) {
 *       if (ok)
 *           GameMode::EnterScene(name);
 *       else
 *           UI::ShowErrorDialog("Failed to load: " + name);
 *   };
 *   sceneMgr.LoadSceneAsync(L"Assets/Scenes/Boss.scene", cb);
 * @endcode
 */
using SceneLoadCallback = std::function<void(bool success, const std::string& sceneName)>;

// =============================================================================
// SceneManager
// =============================================================================

/**
 * @class SceneManager
 * @brief Complete scene management system for loading, saving, and editing scenes.
 *
 * `SceneManager` is the single point of truth for the live scene graph. It owns:
 * - The flat list of `SceneNode` objects (`m_sceneNodes`) that define the hierarchy.
 * - The corresponding instantiated `GameObject` objects (`m_objects`) used at runtime.
 * - Global scene settings via `SceneMetadata`.
 *
 * ### Responsibilities
 * - **Serialization**: JSON and legacy binary round-trip via `LoadJSON`/`SaveJSON`/`LoadCustom`.
 * - **Hierarchy management**: Add, remove, reparent nodes; maintain index invariants.
 * - **Prefab system**: Save/load subtrees as reusable prefab assets.
 * - **Async loading**: Background scene transitions via `LoadSceneAsync`.
 * - **Console integration**: Runtime inspection and manipulation from the debug console.
 *
 * ### Dirty state tracking
 * Every method that modifies the scene calls `MarkDirty()` internally, setting
 * `m_dirty = true`. The editor polls `IsDirty()` before discarding the scene.
 * `LoadScene()`, `SaveScene()`, and `NewScene()` reset the dirty flag.
 *
 * @code
 *   SceneManager mgr(&gfx, &input);
 *   mgr.LoadScene(L"Assets/Scenes/MainMenu.scene");
 *
 *   // Modify the hierarchy:
 *   SceneNode btn;
 *   btn.name = "StartButton";
 *   btn.type = "plane";
 *   int idx = mgr.AddNode(btn);
 *   mgr.SetParent(idx, mgr.FindNode("UI_Root"));
 *
 *   if (mgr.IsDirty())
 *       mgr.SaveScene(L"Assets/Scenes/MainMenu.scene");
 * @endcode
 */
class SceneManager
{
  public:
    /**
     * @brief Construct the SceneManager with required engine subsystems.
     *
     * Does not load any scene; call `NewScene()` or `LoadScene()` to populate the
     * hierarchy. Both pointers must remain valid for the lifetime of this object.
     *
     * @param graphics  Pointer to the active GraphicsEngine (used for mesh instantiation).
     * @param input     Pointer to the InputManager (forwarded to spawned GameObjects).
     */
    SceneManager(GraphicsEngine* graphics, InputManager* input);

    /**
     * @brief Destructor. Joins any in-flight async load thread, then destroys GameObjects.
     */
    ~SceneManager();

    // =========================================================================
    // Scene Loading / Saving
    // =========================================================================

    /**
     * @brief Load a scene from a JSON or legacy binary file, replacing the current scene.
     *
     * Clears the existing hierarchy, parses the file, populates `m_sceneNodes` and
     * `m_metadata`, and calls `InstantiateNodes()` to create `GameObject` instances.
     * The dirty flag is cleared on success.
     *
     * File format is determined by extension:
     * - `.scene` → legacy binary (`LoadCustom`)
     * - `.json` or anything else → JSON (`LoadJSON`)
     *
     * @param filepath  Absolute or asset-relative path to the scene file.
     * @return          `true` on success; `false` if the file is missing or malformed.
     *
     * @note This call is synchronous and may stall the main thread for large scenes.
     *       Use `LoadSceneAsync()` for seamless transitions.
     */
    bool LoadScene(const std::wstring& filepath);

    /**
     * @brief Serialize the current scene to a JSON file.
     *
     * Writes `m_metadata` and all nodes in `m_sceneNodes` to the specified path.
     * The dirty flag is cleared on success.
     *
     * @param filepath  Destination file path. Parent directories must already exist.
     * @return          `true` on success; `false` on I/O error.
     */
    bool SaveScene(const std::wstring& filepath) const;

    /**
     * @brief Begin loading a scene on a background thread.
     *
     * Returns immediately. The scene file is parsed off the main thread; when
     * complete, `callback` is invoked with the result. The transition to the new
     * scene (clearing the old one and instantiating nodes) happens on the main thread
     * at the start of the next frame after loading is done.
     *
     * @param filepath  Path to the scene file to load asynchronously.
     * @param callback  Completion callback; see `SceneLoadCallback` for semantics.
     *
     * @warning Do not call other `SceneManager` methods while an async load is in
     *          flight, as node data may be partially overwritten.
     */
    void LoadSceneAsync(const std::wstring& filepath, SceneLoadCallback callback);

    // =========================================================================
    // Scene Hierarchy
    // =========================================================================

    /**
     * @brief Append a node to the scene hierarchy and return its index.
     *
     * The node is pushed to the back of `m_sceneNodes`. If `node.parentIndex >= 0`,
     * the new node's index is added to the parent's `childIndices`. The dirty flag
     * is set.
     *
     * @param node  Node data to copy into the scene. `childIndices` is ignored and
     *              rebuilt automatically.
     * @return      Integer index of the newly added node (stable until `RemoveNode`).
     */
    int AddNode(const SceneNode& node);

    /**
     * @brief Remove a node and all of its descendants from the scene.
     *
     * Recursively collects the subtree rooted at `index`, unlinks it from the parent,
     * and removes all collected nodes. Indices of remaining nodes are updated to
     * preserve consistency. The dirty flag is set.
     *
     * @param index  Index of the root node of the subtree to remove. Must be in
     *               [0, GetNodeCount()). Out-of-range indices are ignored.
     *
     * @warning After removal, any previously captured node indices may be stale.
     *          Re-query with `FindNode()` or `GetRootNodes()` after calling this.
     */
    void RemoveNode(int index);

    /**
     * @brief Set or change the parent-child relationship between two nodes.
     *
     * Updates `m_sceneNodes[childIndex].parentIndex` and adjusts `childIndices`
     * on both the old and new parent. Pass `parentIndex == -1` to detach the child
     * and make it a root node. The dirty flag is set.
     *
     * @param childIndex   Index of the node to reparent. Must be valid.
     * @param parentIndex  Index of the new parent node, or -1 to make `childIndex` a root.
     *
     * @note Circular hierarchies (a node being its own ancestor) are not detected
     *       and will cause infinite loops in recursive traversal. Do not create them.
     */
    void SetParent(int childIndex, int parentIndex);

    /**
     * @brief Return the indices of all root nodes (nodes with no parent).
     *
     * Root nodes have `parentIndex == -1`. Iterating root nodes and recursing
     * into `childIndices` visits the complete hierarchy.
     *
     * @return Vector of root node indices in insertion order.
     */
    std::vector<int> GetRootNodes() const;

    /**
     * @brief Return a const pointer to the node at `index`, or `nullptr` if out of range.
     *
     * @param index  Node index in [0, GetNodeCount()).
     * @return       Pointer valid until the next `AddNode()` or `RemoveNode()` call.
     */
    const SceneNode* GetNode(int index) const;

    /**
     * @brief Return a mutable pointer to the node at `index`, or `nullptr` if out of range.
     *
     * Modifying the returned node does NOT automatically set the dirty flag.
     * Call `MarkDirty()` after making changes that should be tracked.
     *
     * @param index  Node index in [0, GetNodeCount()).
     * @return       Pointer valid until the next `AddNode()` or `RemoveNode()` call.
     */
    SceneNode* GetNode(int index);

    /**
     * @brief Find the first node with the given name and return its index.
     *
     * Performs a linear search over all nodes. If multiple nodes share the same
     * name the index of the first match (lowest index) is returned.
     *
     * @param name  Node name to search for (case-sensitive).
     * @return      Node index on success, or -1 if no match is found.
     */
    int FindNode(const std::string& name) const;

    /**
     * @brief Return the total number of nodes currently in the scene.
     *
     * Valid indices for `GetNode()`, `RemoveNode()`, etc. are [0, GetNodeCount()).
     */
    int GetNodeCount() const { return static_cast<int>(m_sceneNodes.size()); }

    // =========================================================================
    // Prefab System
    // =========================================================================

    /**
     * @brief Serialize a node and its entire subtree to a standalone prefab file.
     *
     * The prefab file uses the same JSON format as a scene but contains only the
     * specified subtree. Saved prefabs can later be loaded with `LoadPrefab()`.
     *
     * @param nodeIndex  Root node of the subtree to export as a prefab. Must be valid.
     * @param filepath   Destination path for the prefab file (e.g. `"Assets/Prefabs/Tree.prefab"`).
     * @return           `true` on success; `false` on I/O error or invalid `nodeIndex`.
     */
    bool SavePrefab(int nodeIndex, const std::wstring& filepath) const;

    /**
     * @brief Instantiate a saved prefab into the current scene at the given position.
     *
     * Reads the prefab file, offsets all node positions by `position` relative to
     * the prefab's internal origin, and appends the subtree to `m_sceneNodes`. The
     * dirty flag is set. The returned index is the root of the instantiated subtree.
     *
     * @param filepath  Path to a prefab file previously saved with `SavePrefab()`.
     * @param position  World-space offset applied to all nodes in the prefab.
     * @return          Index of the root node of the instantiated prefab, or -1 on failure.
     *
     * @code
     *   int treeIdx = mgr.LoadPrefab(L"Assets/Prefabs/Tree.prefab", {10.f, 0.f, 5.f});
     *   if (treeIdx < 0)
     *       LOG_ERROR("Failed to load tree prefab");
     * @endcode
     */
    int LoadPrefab(const std::wstring& filepath, const DirectX::XMFLOAT3& position = {0, 0, 0});

    // =========================================================================
    // Scene State
    // =========================================================================

    /**
     * @brief Clear the current scene and start a new, empty scene.
     *
     * Destroys all `GameObject` instances, empties `m_sceneNodes`, resets
     * `m_metadata` to defaults, and sets `m_currentFilePath` to empty. The
     * dirty flag is cleared.
     *
     * @param name  Display name assigned to `m_metadata.sceneName`. Defaults to "Untitled".
     */
    void NewScene(const std::string& name = "Untitled");

    /**
     * @brief Destroy all GameObjects and clear the node list without resetting metadata.
     *
     * Lower-level than `NewScene()`; used internally during `LoadScene()` to
     * discard the old scene before populating the new one. Sets the dirty flag.
     */
    void Clear();

    /**
     * @brief Return `true` if the scene has modifications that have not been saved.
     *
     * The editor checks this before discarding the scene or exiting to prompt the
     * user to save unsaved changes.
     */
    bool IsDirty() const { return m_dirty; }

    /**
     * @brief Explicitly mark the scene as having unsaved changes.
     *
     * Called automatically by hierarchy-modifying methods. Game code may also call
     * this after modifying node properties through `GetNode()` to ensure the editor
     * shows the unsaved-changes indicator.
     */
    void MarkDirty() { m_dirty = true; }

    /**
     * @brief Return a const reference to the scene's global metadata.
     *
     * Read-only access to `sceneName`, `author`, ambient light, and gravity settings.
     */
    const SceneMetadata& GetMetadata() const { return m_metadata; }

    /**
     * @brief Return a mutable reference to the scene's global metadata.
     *
     * Modifications do not automatically set the dirty flag; call `MarkDirty()`
     * after changing metadata that should be persisted.
     */
    SceneMetadata& GetMetadata() { return m_metadata; }

    /**
     * @brief Return the file path from which the current scene was last loaded or saved.
     *
     * Empty string if the scene has never been saved or if `NewScene()` was used.
     * Use this path as the default destination when the user presses Ctrl+S.
     */
    const std::wstring& GetCurrentFilePath() const { return m_currentFilePath; }

    // =========================================================================
    // Object Access (legacy compatibility)
    // =========================================================================

    /**
     * @brief Return the instantiated `GameObject` list for this scene.
     *
     * Provided for backward compatibility with code that iterates `GameObject`
     * objects directly. New code should prefer the ECS World query API.
     *
     * @return Const reference to the vector of owned `GameObject` unique pointers.
     */
    const std::vector<std::unique_ptr<GameObject>>& GetObjects() const;

    /**
     * @brief Enumerate all scene files found in a directory.
     *
     * Recursively scans `directory` for files with `.scene` or `.json` extensions
     * and returns their paths as narrow strings suitable for display in a file picker.
     *
     * @param directory  Directory to scan. Defaults to `L"Assets/Scenes"`.
     * @return           Vector of file path strings, one per found scene file.
     */
    std::vector<std::string> GetAvailableScenes(const std::wstring& directory = L"Assets/Scenes") const;

    // =========================================================================
    // Console Integration
    // =========================================================================

    /**
     * @brief Return a formatted list of all nodes in the scene (console integration).
     *
     * Produces a human-readable tree representation of the hierarchy with each
     * node's index, name, type, and depth-indent. Used by the debug console and
     * the editor's hierarchy panel.
     *
     * @return Newline-separated string with one node per line.
     */
    std::string Console_ListNodes() const;

    /**
     * @brief Return detailed information about a specific node (console integration).
     *
     * Outputs all fields of the node at `index`: name, type, transform, model/material
     * paths, parent index, child count, and the full `properties` map.
     *
     * @param index  Node index to inspect. Returns an error string if out of range.
     * @return       Multi-line formatted string with all node fields.
     */
    std::string Console_GetNodeInfo(int index) const;

    /**
     * @brief Move a node to a new world-space position via the console.
     *
     * Updates `SceneNode::position` and the corresponding `GameObject`'s Transform.
     * Sets the dirty flag.
     *
     * @param index  Node index to move. Must be in [0, GetNodeCount()).
     * @param x      New X position in world space.
     * @param y      New Y position in world space.
     * @param z      New Z position in world space.
     * @return       `true` on success; `false` if `index` is out of range.
     */
    bool Console_MoveNode(int index, float x, float y, float z);

    /**
     * @brief Rename a node via the console.
     *
     * Updates `SceneNode::name`. If a node with `newName` already exists the
     * rename is rejected and `false` is returned to avoid ambiguous `FindNode()` results.
     * Sets the dirty flag on success.
     *
     * @param index    Node index to rename. Must be in [0, GetNodeCount()).
     * @param newName  New unique name for the node.
     * @return         `true` on success; `false` if `index` is out of range or
     *                 `newName` is already in use.
     */
    bool Console_RenameNode(int index, const std::string& newName);

  private:
    /**
     * @brief Load a scene from the legacy binary `.scene` format.
     *
     * Invoked by `LoadScene()` when the file extension is `.scene`. The binary
     * format is engine-version-specific; see the internal documentation for the
     * exact byte layout.
     *
     * @param path  Path to the binary scene file.
     * @return      `true` on success; `false` on I/O error or format mismatch.
     */
    bool LoadCustom(const std::wstring& path);

    /**
     * @brief Deserialize a scene from a JSON file into `m_metadata` and `m_sceneNodes`.
     *
     * Parses the top-level `"metadata"` and `"nodes"` arrays. Unknown fields are
     * silently ignored for forward compatibility.
     *
     * @param path  Path to the JSON scene file.
     * @return      `true` on success; `false` on I/O error or JSON parse failure.
     */
    bool LoadJSON(const std::wstring& path);

    /**
     * @brief Serialize the current scene to a JSON file.
     *
     * Writes `m_metadata` followed by the full `m_sceneNodes` array. Existing
     * file content is overwritten atomically.
     *
     * @param path  Destination file path. Must be writable.
     * @return      `true` on success; `false` on I/O error.
     */
    bool SaveJSON(const std::wstring& path) const;

    /**
     * @brief Create `GameObject` instances for all nodes in `m_sceneNodes`.
     *
     * Called after `LoadJSON()` or `LoadCustom()` has populated the node list.
     * For each node, looks up the type, creates an appropriate `GameObject`, and
     * applies the node's transform and properties. Results are stored in `m_objects`.
     */
    void InstantiateNodes();

    // -------------------------------------------------------------------------
    // Engine subsystem references
    // -------------------------------------------------------------------------

    /** @brief Non-owning pointer to the engine GraphicsEngine (used during node instantiation). */
    GraphicsEngine* m_graphics;

    /** @brief Non-owning pointer to the InputManager (forwarded to spawned GameObjects). */
    InputManager* m_input;

    // -------------------------------------------------------------------------
    // Scene data
    // -------------------------------------------------------------------------

    /**
     * @brief Flat list of all scene nodes forming the hierarchy.
     *
     * Nodes reference each other by integer index. The vector may be resized by
     * `AddNode()` and `RemoveNode()`; cached pointers become invalid after resizing.
     */
    std::vector<SceneNode> m_sceneNodes;

    /**
     * @brief Instantiated GameObjects corresponding to entries in `m_sceneNodes`.
     *
     * Index `i` in `m_objects` corresponds to index `i` in `m_sceneNodes`.
     * Entries may be `nullptr` for nodes that failed to instantiate (e.g. missing
     * mesh asset).
     */
    std::vector<std::unique_ptr<GameObject>> m_objects;

    // -------------------------------------------------------------------------
    // Scene state
    // -------------------------------------------------------------------------

    /** @brief Global metadata (name, author, ambient light, gravity) for the loaded scene. */
    SceneMetadata m_metadata;

    /**
     * @brief File path from which the current scene was last loaded or saved.
     *
     * Empty if the scene was created with `NewScene()` and never saved.
     */
    std::wstring m_currentFilePath;

    /**
     * @brief Dirty flag indicating unsaved changes.
     *
     * Set to `true` by any method that modifies the hierarchy or metadata.
     * Cleared by `LoadScene()`, `SaveScene()`, and `NewScene()`.
     */
    bool m_dirty = false;

    /** @brief Background thread for async scene loading (joined in destructor). */
    std::thread m_asyncLoadThread;
};
