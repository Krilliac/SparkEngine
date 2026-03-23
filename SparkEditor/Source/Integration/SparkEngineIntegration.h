/**
 * @file SparkEngineIntegration.h
 * @brief Deep integration system between Spark Engine Editor and Runtime
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
using namespace DirectX;


// Forward declarations
struct ID3D11Device;
struct ID3D11DeviceContext;

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

    /**
 * @brief Deep integration system between Spark Engine Editor and Runtime
 * 
 * This system provides seamless communication between the editor and engine,
 * enabling live editing, real-time debugging, performance monitoring,
 * and bidirectional data synchronization.
 */
    class SparkEngineIntegration
    {
      public:
        /**
     * @brief Constructor
     */
        SparkEngineIntegration();

        /**
     * @brief Destructor
     */
        ~SparkEngineIntegration();

        /**
     * @brief Initialize the integration system
     * @param device DirectX device
     * @param context DirectX context
     * @return true if initialization succeeded
     */
        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

        /**
     * @brief Update the integration system
     * @param deltaTime Time since last update
     */
        void Update(float deltaTime);

        /**
     * @brief Shutdown the integration system
     */
        void Shutdown();

        // === CONNECTION MANAGEMENT ===

        /**
     * @brief Connect to Spark Engine runtime
     * @param enginePath Path to engine executable
     * @param projectPath Path to project file
     * @return true if connection initiated successfully
     */
        bool ConnectToEngine(const std::string& enginePath, const std::string& projectPath);

        /**
     * @brief Disconnect from engine
     */
        void DisconnectFromEngine();

        /**
     * @brief Get connection status
     * @return Current connection status
     */
        EngineConnectionStatus GetConnectionStatus() const;

        /**
     * @brief Check if connected to engine
     * @return true if connected
     */
        bool IsConnected() const;

        // === ENGINE CONTROL ===

        /**
     * @brief Start engine playback
     * @return Command execution result
     */
        CommandResult StartEngine();

        /**
     * @brief Pause engine execution
     * @return Command execution result
     */
        CommandResult PauseEngine();

        /**
     * @brief Stop engine execution
     * @return Command execution result
     */
        CommandResult StopEngine();

        /**
     * @brief Step engine one frame (when paused)
     * @return Command execution result
     */
        CommandResult StepFrame();

        /**
     * @brief Set engine time scale
     * @param timeScale Time scale multiplier
     * @return Command execution result
     */
        CommandResult SetTimeScale(float timeScale);

        // === SCENE SYNCHRONIZATION ===

        /**
     * @brief Get current scene data from engine
     * @return Scene data
     */
        EditorSceneData GetSceneData();

        /**
     * @brief Load scene in engine
     * @param scenePath Path to scene file
     * @return Command execution result
     */
        CommandResult LoadScene(const std::string& scenePath);

        /**
     * @brief Save current scene
     * @param scenePath Path to save scene
     * @return Command execution result
     */
        CommandResult SaveScene(const std::string& scenePath);

        /**
     * @brief Create new empty scene
     * @return Command execution result
     */
        CommandResult CreateNewScene();

        // === ENTITY MANAGEMENT ===

        /**
     * @brief Get all entities in scene
     * @return Vector of entity data
     */
        std::vector<EditorEntityData> GetAllEntities();

        /**
     * @brief Get entity by ID
     * @param entityId Entity ID
     * @return Entity data
     */
        EditorEntityData GetEntity(uint32_t entityId);

        /**
     * @brief Create new entity
     * @param name Entity name
     * @param position Initial position
     * @return Entity ID
     */
        uint32_t CreateEntity(const std::string& name, const XMFLOAT3& position = {0, 0, 0});

        /**
     * @brief Delete entity
     * @param entityId Entity ID
     * @return Command execution result
     */
        CommandResult DeleteEntity(uint32_t entityId);

        /**
     * @brief Update entity transform
     * @param entityId Entity ID
     * @param position New position
     * @param rotation New rotation
     * @param scale New scale
     * @return Command execution result
     */
        CommandResult UpdateEntityTransform(uint32_t entityId, const XMFLOAT3& position, const XMFLOAT3& rotation,
                                            const XMFLOAT3& scale);

        /**
     * @brief Add component to entity
     * @param entityId Entity ID
     * @param componentType Component type name
     * @return Command execution result
     */
        CommandResult AddComponent(uint32_t entityId, const std::string& componentType);

        /**
     * @brief Remove component from entity
     * @param entityId Entity ID
     * @param componentType Component type name
     * @return Command execution result
     */
        CommandResult RemoveComponent(uint32_t entityId, const std::string& componentType);

        // === ASSET MANAGEMENT ===

        /**
     * @brief Get all loaded assets
     * @return Vector of asset data
     */
        std::vector<EditorAssetData> GetLoadedAssets();

        /**
     * @brief Load asset in engine
     * @param assetPath Path to asset file
     * @return Command execution result
     */
        CommandResult LoadAsset(const std::string& assetPath);

        /**
     * @brief Unload asset from engine
     * @param assetPath Path to asset file
     * @return Command execution result
     */
        CommandResult UnloadAsset(const std::string& assetPath);

        /**
     * @brief Reload asset (for hot-reloading)
     * @param assetPath Path to asset file
     * @return Command execution result
     */
        CommandResult ReloadAsset(const std::string& assetPath);

        /**
     * @brief Reload all shaders
     * @return Command execution result
     */
        CommandResult ReloadAllShaders();

        // === LIVE VARIABLE EDITING ===

        /**
     * @brief Get all live variables
     * @return Vector of live variables
     */
        std::vector<LiveVariable> GetLiveVariables();

        /**
     * @brief Set live variable value
     * @param variableName Variable name
     * @param value New value as string
     * @return Command execution result
     */
        CommandResult SetLiveVariable(const std::string& variableName, const std::string& value);

        /**
     * @brief Register live variable for editing
     * @param variable Variable definition
     * @return Command execution result
     */
        CommandResult RegisterLiveVariable(const LiveVariable& variable);

        // === DEBUGGING AND PROFILING ===

        /**
     * @brief Get current engine state
     * @return Engine state data
     */
        EngineState GetEngineState();

        /**
     * @brief Get profiling data
     * @return Profiling data
     */
        EngineProfilingData GetProfilingData();

        /**
     * @brief Enable/disable profiling
     * @param enabled Profiling enabled
     * @return Command execution result
     */
        CommandResult SetProfilingEnabled(bool enabled);

        /**
     * @brief Take screenshot
     * @param filePath Path to save screenshot
     * @return Command execution result
     */
        CommandResult TakeScreenshot(const std::string& filePath);

        /**
     * @brief Enable/disable wireframe rendering
     * @param enabled Wireframe enabled
     * @return Command execution result
     */
        CommandResult SetWireframeMode(bool enabled);

        // === CONSOLE INTEGRATION ===

        /**
     * @brief Execute console command in engine
     * @param command Console command
     * @return Command execution result
     */
        CommandResult ExecuteConsoleCommand(const std::string& command);

        /**
     * @brief Get console history
     * @return Vector of console messages
     */
        std::vector<std::string> GetConsoleHistory();

        /**
     * @brief Clear console history
     * @return Command execution result
     */
        CommandResult ClearConsole();

        // === CALLBACK REGISTRATION ===

        /**
     * @brief Set engine state callback
     * @param callback Callback function
     */
        void SetEngineStateCallback(EngineStateCallback callback);

        /**
     * @brief Set entity changed callback
     * @param callback Callback function
     */
        void SetEntityChangedCallback(EntityChangedCallback callback);

        /**
     * @brief Set asset changed callback
     * @param callback Callback function
     */
        void SetAssetChangedCallback(AssetChangedCallback callback);

        /**
     * @brief Set scene changed callback
     * @param callback Callback function
     */
        void SetSceneChangedCallback(SceneChangedCallback callback);

        /**
     * @brief Set profiling data callback
     * @param callback Callback function
     */
        void SetProfilingDataCallback(ProfilingDataCallback callback);

        /**
     * @brief Set console message callback
     * @param callback Callback function
     */
        void SetConsoleMessageCallback(ConsoleMessageCallback callback);

        // === CAMERA CONTROL ===

        /**
     * @brief Set editor camera position and rotation
     * @param position Camera position
     * @param rotation Camera rotation (Euler angles)
     * @return Command execution result
     */
        CommandResult SetEditorCamera(const XMFLOAT3& position, const XMFLOAT3& rotation);

        /**
     * @brief Get editor camera transform
     * @return Pair of position and rotation
     */
        std::pair<XMFLOAT3, XMFLOAT3> GetEditorCamera();

        // === GIZMO AND SELECTION ===

        /**
     * @brief Set selected entity
     * @param entityId Entity ID (0 = deselect all)
     * @return Command execution result
     */
        CommandResult SetSelectedEntity(uint32_t entityId);

        /**
     * @brief Get selected entity ID
     * @return Selected entity ID (0 = none selected)
     */
        uint32_t GetSelectedEntityId();

        /**
     * @brief Enable/disable gizmos
     * @param enabled Gizmos enabled
     * @return Command execution result
     */
        CommandResult SetGizmosEnabled(bool enabled);

      private:
        /**
     * @brief Communication thread function
     */
        void CommunicationThread();

        /**
     * @brief Process incoming messages from engine
     */
        void ProcessIncomingMessages();

        /**
     * @brief Send command to engine
     * @param command Command string
     * @return Command result
     */
        CommandResult SendCommand(const std::string& command);

        /**
     * @brief Parse engine response
     * @param response Response string
     * @return Parsed command result
     */
        CommandResult ParseResponse(const std::string& response);

      private:
        // Connection state
        std::atomic<EngineConnectionStatus> m_connectionStatus{EngineConnectionStatus::Disconnected};
        std::string m_enginePath;
        std::string m_projectPath;

        // Communication
        std::thread m_communicationThread;
        std::atomic<bool> m_shouldStop{false};
        std::mutex m_commandMutex;
        std::mutex m_responseMutex;

        // Cached data
        EngineState m_engineState;
        EditorSceneData m_sceneData;
        EngineProfilingData m_profilingData;
        std::vector<LiveVariable> m_liveVariables;
        std::vector<EditorAssetData> m_loadedAssets;
        std::vector<std::string> m_consoleHistory;
        uint32_t m_selectedEntityId = 0;

        // Callbacks
        EngineStateCallback m_engineStateCallback;
        EntityChangedCallback m_entityChangedCallback;
        AssetChangedCallback m_assetChangedCallback;
        SceneChangedCallback m_sceneChangedCallback;
        ProfilingDataCallback m_profilingDataCallback;
        ConsoleMessageCallback m_consoleMessageCallback;

        // DirectX resources
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;

        // State
        bool m_isInitialized = false;
        float m_updateTimer = 0.0f;
        const float m_updateInterval = 0.1f; // Update 10 times per second
    };

} // namespace SparkEditor