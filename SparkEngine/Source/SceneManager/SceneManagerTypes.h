/**
 * @file SceneManagerTypes.h
 * @brief Type definitions, enums, and structs used by SceneManager
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * This header contains the data types that form the scene hierarchy:
 * - `SceneNode` — a single node in the scene object hierarchy
 * - `SceneMetadata` — global metadata and environment settings for a scene
 * - `SceneLoadCallback` — completion callback for asynchronous scene loads
 *
 * These types are separated from `SceneManager.h` so that code which only
 * needs the data definitions (serializers, importers, tests) can include
 * this lightweight header without pulling in the full SceneManager class.
 */

#pragma once

#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>


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
