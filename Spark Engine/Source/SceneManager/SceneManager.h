/**
 * @file SceneManager.h
 * @brief Scene management with serialization, hierarchy, and prefab support
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides scene loading/saving, object hierarchy management, prefab system,
 * and async scene transitions.
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <filesystem>
#include <unordered_map>

class GraphicsEngine;
class InputManager;
class GameObject;

/**
 * @brief Node in the scene hierarchy
 */
struct SceneNode
{
    std::string name;
    std::string type;                           ///< "cube", "sphere", "plane", "model", etc.
    DirectX::XMFLOAT3 position = {0,0,0};
    DirectX::XMFLOAT3 rotation = {0,0,0};
    DirectX::XMFLOAT3 scale = {1,1,1};
    std::string modelPath;                      ///< For model-type objects
    std::string materialPath;                   ///< Material override path

    int parentIndex = -1;                       ///< Index of parent node (-1 = root)
    std::vector<int> childIndices;              ///< Indices of child nodes

    // Custom properties (key-value)
    std::unordered_map<std::string, std::string> properties;
};

/**
 * @brief Scene file metadata
 */
struct SceneMetadata
{
    std::string sceneName;
    std::string author;
    std::string version;
    std::string description;
    float ambientLightR = 0.1f, ambientLightG = 0.1f, ambientLightB = 0.1f;
    float gravityX = 0.0f, gravityY = -9.81f, gravityZ = 0.0f;
};

/**
 * @brief Scene transition callback type
 */
using SceneLoadCallback = std::function<void(bool success, const std::string& sceneName)>;

/**
 * @brief Complete scene management system
 */
class SceneManager
{
public:
    SceneManager(GraphicsEngine* graphics, InputManager* input);
    ~SceneManager() = default;

    // ============================================================================
    // Scene Loading / Saving
    // ============================================================================

    /**
     * @brief Load a scene from file (JSON format)
     * @param filepath Path to .scene or .json file
     * @return true on success
     */
    bool LoadScene(const std::wstring& filepath);

    /**
     * @brief Save the current scene to a file
     * @param filepath Path to save the scene
     * @return true on success
     */
    bool SaveScene(const std::wstring& filepath) const;

    /**
     * @brief Load scene asynchronously with progress callback
     * @param filepath Path to scene file
     * @param callback Completion callback
     */
    void LoadSceneAsync(const std::wstring& filepath, SceneLoadCallback callback);

    // ============================================================================
    // Scene Hierarchy
    // ============================================================================

    /**
     * @brief Add a scene node
     * @return Index of the added node
     */
    int AddNode(const SceneNode& node);

    /**
     * @brief Remove a node and its children
     */
    void RemoveNode(int index);

    /**
     * @brief Set parent-child relationship
     */
    void SetParent(int childIndex, int parentIndex);

    /**
     * @brief Get all root nodes (no parent)
     */
    std::vector<int> GetRootNodes() const;

    /**
     * @brief Get node by index
     */
    const SceneNode* GetNode(int index) const;
    SceneNode* GetNode(int index);

    /**
     * @brief Find node by name
     * @return Index or -1 if not found
     */
    int FindNode(const std::string& name) const;

    /**
     * @brief Get the total number of nodes
     */
    int GetNodeCount() const { return static_cast<int>(m_sceneNodes.size()); }

    // ============================================================================
    // Prefab System
    // ============================================================================

    /**
     * @brief Save a node subtree as a prefab
     * @param nodeIndex Root node of the prefab
     * @param filepath Path to save the prefab file
     * @return true on success
     */
    bool SavePrefab(int nodeIndex, const std::wstring& filepath) const;

    /**
     * @brief Load and instantiate a prefab
     * @param filepath Path to the prefab file
     * @param position Spawn position
     * @return Index of the root node of the instantiated prefab, or -1 on failure
     */
    int LoadPrefab(const std::wstring& filepath, const DirectX::XMFLOAT3& position = {0,0,0});

    // ============================================================================
    // Scene State
    // ============================================================================

    /**
     * @brief Create new empty scene
     */
    void NewScene(const std::string& name = "Untitled");

    /**
     * @brief Clear all objects and nodes
     */
    void Clear();

    /**
     * @brief Check if scene has unsaved changes
     */
    bool IsDirty() const { return m_dirty; }

    /**
     * @brief Mark scene as dirty (modified)
     */
    void MarkDirty() { m_dirty = true; }

    /**
     * @brief Get current scene metadata
     */
    const SceneMetadata& GetMetadata() const { return m_metadata; }
    SceneMetadata& GetMetadata() { return m_metadata; }

    /**
     * @brief Get current scene filepath
     */
    const std::wstring& GetCurrentFilePath() const { return m_currentFilePath; }

    // ============================================================================
    // Object Access (legacy compatibility)
    // ============================================================================

    const std::vector<std::unique_ptr<GameObject>>& GetObjects() const;

    /**
     * @brief Get list of available scene files in a directory
     */
    std::vector<std::string> GetAvailableScenes(const std::wstring& directory = L"Assets/Scenes") const;

    // ============================================================================
    // Console Integration
    // ============================================================================

    std::string Console_ListNodes() const;
    std::string Console_GetNodeInfo(int index) const;
    bool Console_MoveNode(int index, float x, float y, float z);
    bool Console_RenameNode(int index, const std::string& newName);

private:
    bool LoadCustom(const std::wstring& path);
    bool LoadJSON(const std::wstring& path);
    bool SaveJSON(const std::wstring& path) const;
    void InstantiateNodes();

    GraphicsEngine* m_graphics;
    InputManager* m_input;

    // Scene hierarchy data
    std::vector<SceneNode> m_sceneNodes;

    // Instantiated game objects
    std::vector<std::unique_ptr<GameObject>> m_objects;

    // Scene state
    SceneMetadata m_metadata;
    std::wstring m_currentFilePath;
    bool m_dirty = false;
};
