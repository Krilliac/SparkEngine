/**
 * @file IntegrationTypes.h
 * @brief Type definitions for editor/engine integration
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
using namespace DirectX;

namespace SparkEditor
{

    /**
 * @brief Engine connection status
 */
    enum class EngineConnectionStatus
    {
        Disconnected,   ///< Not connected to engine
        Connecting,     ///< Attempting to connect
        Connected,      ///< Successfully connected
        ConnectionLost, ///< Connection was lost
        ConnectionError ///< Connection error occurred
    };

    /**
 * @brief Engine state information
 */
    struct EngineState
    {
        bool isRunning = false;              ///< Engine is running
        bool isPaused = false;               ///< Engine is paused
        float frameRate = 0.0f;              ///< Current frame rate
        float frameTime = 0.0f;              ///< Frame time in milliseconds
        size_t memoryUsage = 0;              ///< Memory usage in bytes
        int drawCalls = 0;                   ///< Number of draw calls
        int triangles = 0;                   ///< Number of triangles rendered
        int activeObjects = 0;               ///< Number of active game objects
        XMFLOAT3 cameraPosition = {0, 0, 0}; ///< Current camera position
        XMFLOAT3 cameraRotation = {0, 0, 0}; ///< Current camera rotation
    };

    /**
 * @brief Entity component data for editor integration
 */
    struct EditorEntityData
    {
        uint32_t entityId = 0;               ///< Entity ID
        std::string name;                    ///< Entity name
        XMFLOAT3 position = {0, 0, 0};       ///< World position
        XMFLOAT3 rotation = {0, 0, 0};       ///< World rotation (Euler angles)
        XMFLOAT3 scale = {1, 1, 1};          ///< World scale
        std::vector<std::string> components; ///< Component type names
        bool isActive = true;                ///< Entity active state
        bool isVisible = true;               ///< Entity visibility
        uint32_t parentId = 0;               ///< Parent entity ID (0 = no parent)
        std::vector<uint32_t> childIds;      ///< Child entity IDs
    };

    /**
 * @brief Asset data for real-time engine integration
 */
    struct EditorAssetData
    {
        std::string path;                      ///< Asset file path
        std::string type;                      ///< Asset type
        std::string guid;                      ///< Unique identifier
        bool isLoaded = false;                 ///< Loaded in engine
        size_t memoryUsage = 0;                ///< Memory usage in bytes
        float loadTime = 0.0f;                 ///< Load time in milliseconds
        std::vector<std::string> dependencies; ///< Asset dependencies
    };

    /**
 * @brief Scene data for editor/engine synchronization
 */
    struct EditorSceneData
    {
        std::string name;                       ///< Scene name
        std::string path;                       ///< Scene file path
        std::vector<EditorEntityData> entities; ///< All entities in scene
        bool isDirty = false;                   ///< Scene has unsaved changes
        size_t memoryUsage = 0;                 ///< Scene memory usage
    };

    /**
 * @brief Profiling data from engine
 */
    struct EngineProfilingData
    {
        // CPU timing
        float cpuFrameTime = 0.0f; ///< Total CPU frame time
        float updateTime = 0.0f;   ///< Update phase time
        float renderTime = 0.0f;   ///< Render phase time
        float physicsTime = 0.0f;  ///< Physics simulation time
        float audioTime = 0.0f;    ///< Audio processing time

        // GPU timing
        float gpuFrameTime = 0.0f;     ///< Total GPU frame time
        float shadowRenderTime = 0.0f; ///< Shadow map rendering time
        float lightingTime = 0.0f;     ///< Lighting calculation time
        float postProcessTime = 0.0f;  ///< Post-processing time

        // Memory usage
        size_t totalMemory = 0;   ///< Total allocated memory
        size_t meshMemory = 0;    ///< Mesh data memory
        size_t textureMemory = 0; ///< Texture memory
        size_t shaderMemory = 0;  ///< Shader memory
        size_t audioMemory = 0;   ///< Audio memory

        // Resource counts
        int loadedMeshes = 0;     ///< Number of loaded meshes
        int loadedTextures = 0;   ///< Number of loaded textures
        int loadedShaders = 0;    ///< Number of loaded shaders
        int loadedAudioClips = 0; ///< Number of loaded audio clips
    };

    /**
 * @brief Live variable editing support
 */
    struct LiveVariable
    {
        std::string name;        ///< Variable name
        std::string type;        ///< Variable type (float, int, bool, etc.)
        std::string category;    ///< Variable category
        std::string value;       ///< Current value as string
        std::string minValue;    ///< Minimum value (for numeric types)
        std::string maxValue;    ///< Maximum value (for numeric types)
        bool isReadOnly = false; ///< Variable is read-only
        std::string description; ///< Variable description
    };

    /**
 * @brief Command execution result
 */
    struct CommandResult
    {
        bool success = false;       ///< Command executed successfully
        std::string result;         ///< Command result/output
        std::string error;          ///< Error message if failed
        float executionTime = 0.0f; ///< Execution time in milliseconds
    };

    /**
 * @brief Callback function types
 */
    using EngineStateCallback = std::function<void(const EngineState&)>;
    using EntityChangedCallback = std::function<void(const EditorEntityData&)>;
    using AssetChangedCallback = std::function<void(const EditorAssetData&)>;
    using SceneChangedCallback = std::function<void(const EditorSceneData&)>;
    using ProfilingDataCallback = std::function<void(const EngineProfilingData&)>;
    using ConsoleMessageCallback = std::function<void(const std::string& message, const std::string& type)>;

} // namespace SparkEditor
