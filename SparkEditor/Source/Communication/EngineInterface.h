/**
 * @file EngineInterface.h
 * @brief Communication interface between Editor and Engine
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file defines the communication system that allows the editor to
 * interact with a running instance of the Spark Engine for live editing,
 * debugging, and real-time parameter adjustment.
 */

#pragma once

#include <string>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
using namespace DirectX;


namespace SparkEditor
{

    /**
 * @brief Command types that can be sent to the engine
 */
    enum class EngineCommandType : uint32_t
    {
        // Object manipulation
        CREATE_OBJECT = 1,
        DELETE_OBJECT = 2,
        MODIFY_OBJECT = 3,
        SET_TRANSFORM = 4,

        // Component operations
        ADD_COMPONENT = 10,
        REMOVE_COMPONENT = 11,
        MODIFY_COMPONENT = 12,

        // Scene operations
        LOAD_SCENE = 20,
        SAVE_SCENE = 21,
        NEW_SCENE = 22,
        CLEAR_SCENE = 23,

        // Asset operations
        LOAD_ASSET = 30,
        UNLOAD_ASSET = 31,
        RELOAD_ASSET = 32,

        // Camera operations
        SET_CAMERA_TRANSFORM = 40,
        SET_CAMERA_PROPERTIES = 41,

        // Rendering operations
        SET_RENDER_MODE = 50,
        TOGGLE_WIREFRAME = 51,
        SET_CLEAR_COLOR = 52,
        TAKE_SCREENSHOT = 53,

        // Debug operations
        SET_DEBUG_MODE = 60,
        TOGGLE_GIZMOS = 61,
        SET_DEBUG_DRAW = 62,

        // Play mode operations
        ENTER_PLAY_MODE = 70,
        EXIT_PLAY_MODE = 71,
        PAUSE_PLAY_MODE = 72,
        STEP_FRAME = 73,

        // System operations
        GET_SYSTEM_INFO = 80,
        SET_TIME_SCALE = 81,
        RELOAD_SHADERS = 82,

        // Custom commands
        CUSTOM_COMMAND = 1000
    };

    /**
 * @brief Event types that can be received from the engine
 */
    enum class EngineEventType : uint32_t
    {
        // Object events
        OBJECT_CREATED = 1,
        OBJECT_DELETED = 2,
        OBJECT_MODIFIED = 3,
        OBJECT_SELECTED = 4,

        // Component events
        COMPONENT_ADDED = 10,
        COMPONENT_REMOVED = 11,
        COMPONENT_MODIFIED = 12,

        // Scene events
        SCENE_LOADED = 20,
        SCENE_SAVED = 21,
        SCENE_CHANGED = 22,

        // Asset events
        ASSET_LOADED = 30,
        ASSET_UNLOADED = 31,
        ASSET_MODIFIED = 32,
        ASSET_MISSING = 33,

        // System events
        ENGINE_READY = 40,
        ENGINE_SHUTDOWN = 41,
        PLAY_MODE_STARTED = 42,
        PLAY_MODE_STOPPED = 43,
        FRAME_RENDERED = 44,

        // Error events
        ERROR_OCCURRED = 50,
        WARNING_OCCURRED = 51,

        // Performance events
        PERFORMANCE_UPDATE = 60,
        MEMORY_UPDATE = 61,

        // Custom events
        CUSTOM_EVENT = 1000
    };

    /**
 * @brief Command data structure
 */
    struct EngineCommand
    {
        EngineCommandType type = EngineCommandType::CUSTOM_COMMAND; ///< Command type
        uint64_t commandID = 0;                                     ///< Unique command identifier
        std::string targetObjectID;                                 ///< Target object (if applicable)
        std::string componentType;                                  ///< Component type (if applicable)
        std::unordered_map<std::string, std::string> parameters;    ///< Command parameters
        std::vector<uint8_t> binaryData;                            ///< Binary data payload
        uint64_t timestamp = 0;                                     ///< Command timestamp
    };

    /**
 * @brief Event data structure
 */
    struct EngineEvent
    {
        EngineEventType type = EngineEventType::CUSTOM_EVENT; ///< Event type
        uint64_t eventID = 0;                                 ///< Unique event identifier
        std::string sourceObjectID;                           ///< Source object (if applicable)
        std::string message;                                  ///< Event message
        std::unordered_map<std::string, std::string> data;    ///< Event data
        std::vector<uint8_t> binaryData;                      ///< Binary data payload
        uint64_t timestamp = 0;                               ///< Event timestamp
        int severity = 0;                                     ///< Event severity (0=info, 1=warning, 2=error)
    };

    /**
 * @brief Engine performance metrics
 */
    struct EngineMetrics
    {
        float fps = 0.0f;          ///< Frames per second
        float frameTime = 0.0f;    ///< Frame time in milliseconds
        float cpuTime = 0.0f;      ///< CPU time in milliseconds
        float gpuTime = 0.0f;      ///< GPU time in milliseconds
        size_t memoryUsage = 0;    ///< Memory usage in bytes
        size_t gpuMemoryUsage = 0; ///< GPU memory usage in bytes
        int drawCalls = 0;         ///< Number of draw calls
        int triangles = 0;         ///< Number of triangles rendered
        int activeObjects = 0;     ///< Number of active game objects
        bool isPlayMode = false;   ///< Whether engine is in play mode
        float timeScale = 1.0f;    ///< Current time scale
    };

    /**
 * @brief Engine system information
 */
    struct EngineSystemInfo
    {
        std::string version;                       ///< Engine version
        std::string platform;                      ///< Target platform
        std::string graphicsAPI;                   ///< Graphics API in use
        std::string audioAPI;                      ///< Audio API in use
        std::vector<std::string> supportedFormats; ///< Supported asset formats
        std::vector<std::string> capabilities;     ///< Engine capabilities
        uint64_t startTime = 0;                    ///< Engine start timestamp
        bool debugMode = false;                    ///< Whether debug mode is enabled
    };

    /**
 * @brief Communication interface between Editor and Engine
 * 
 * Provides a bidirectional communication channel for live editing,
 * real-time debugging, and seamless integration between the editor
 * and a running engine instance.
 * 
 * Features:
 * - Named pipe communication for low latency
 * - Asynchronous command/event processing
 * - Automatic reconnection handling
 * - Command queuing and prioritization
 * - Event filtering and subscription
 * - Performance monitoring
 * - Error handling and recovery
 */
    class EngineInterface
    {
      public:
        /**
     * @brief Constructor
     */
        EngineInterface();

        /**
     * @brief Destructor
     */
        ~EngineInterface();

        /**
     * @brief Initialize the engine interface
     * @param pipeName Named pipe name for communication
     * @return true if initialization succeeded, false otherwise
     */
        bool Initialize(const std::string& pipeName = "SparkEngineEditorPipe");

        /**
     * @brief Shutdown the interface
     */
        void Shutdown();

        /**
     * @brief Update the interface (call each frame)
     * @param deltaTime Time elapsed since last frame
     */
        void Update(float deltaTime);

        /**
     * @brief Check if connected to engine
     * @return true if connected, false otherwise
     */
        bool IsConnected() const { return m_isConnected.load(); }

        /**
     * @brief Attempt to connect to engine
     * @param timeoutSeconds Timeout for connection attempt
     * @return true if connection succeeded, false otherwise
     */
        bool Connect(float timeoutSeconds = 5.0f);

        /**
     * @brief Disconnect from engine
     */
        void Disconnect();

        /**
     * @brief Send command to engine
     * @param command Command to send
     * @return Command ID for tracking, or 0 if failed
     */
        uint64_t SendCommand(const EngineCommand& command);

        /**
     * @brief Send command to engine (simplified interface)
     * @param type Command type
     * @param parameters Command parameters
     * @return Command ID for tracking, or 0 if failed
     */
        uint64_t SendCommand(EngineCommandType type,
                             const std::unordered_map<std::string, std::string>& parameters = {});

        /**
     * @brief Register event callback
     * @param eventType Type of event to listen for
     * @param callback Function to call when event occurs
     */
        void RegisterEventCallback(EngineEventType eventType, std::function<void(const EngineEvent&)> callback);

        /**
     * @brief Unregister event callback
     * @param eventType Type of event to stop listening for
     */
        void UnregisterEventCallback(EngineEventType eventType);

        /**
     * @brief Get latest engine metrics
     * @return Current engine performance metrics
     */
        EngineMetrics GetEngineMetrics() const;

        /**
     * @brief Get engine system information
     * @return Engine system information
     */
        EngineSystemInfo GetEngineSystemInfo() const;

        /**
     * @brief Set command timeout
     * @param timeoutSeconds Timeout for commands in seconds
     */
        void SetCommandTimeout(float timeoutSeconds) { m_commandTimeout = timeoutSeconds; }

        /**
     * @brief Get command timeout
     * @return Current command timeout in seconds
     */
        float GetCommandTimeout() const { return m_commandTimeout; }

        /**
     * @brief Enable/disable automatic reconnection
     * @param enabled Whether to automatically reconnect on disconnect
     */
        void SetAutoReconnect(bool enabled) { m_autoReconnect = enabled; }

        /**
     * @brief Check if auto-reconnect is enabled
     * @return true if auto-reconnect is enabled
     */
        bool IsAutoReconnectEnabled() const { return m_autoReconnect; }

        /**
     * @brief Get connection statistics
     */
        struct ConnectionStats
        {
            uint64_t commandsSent = 0;
            uint64_t eventsReceived = 0;
            uint64_t bytesTransmitted = 0;
            uint64_t bytesReceived = 0;
            float averageLatency = 0.0f;
            uint32_t connectionAttempts = 0;
            uint32_t disconnections = 0;
        };
        ConnectionStats GetConnectionStats() const;

        /**
     * @brief Clear connection statistics
     */
        void ClearConnectionStats();

        /// Maximum accepted serialized engine event size.
        static constexpr size_t kMaxEventMessageSize = 16u * 1024u * 1024u;

        /**
         * @brief Decode one complete engine event without partially mutating output on failure.
         * @return true only when every declared field fits in @p buffer.
         */
        static bool DeserializeEvent(const std::vector<uint8_t>& buffer, EngineEvent& event);

      private:
        /**
     * @brief Thread function for handling communication
     */
        void CommunicationThread();

        /**
     * @brief Process incoming events
     */
        void ProcessIncomingEvents();

        /**
     * @brief Process outgoing commands
     */
        void ProcessOutgoingCommands();

        /**
     * @brief Handle connection events
     */
        void HandleConnectionEvents();

        /**
     * @brief Serialize command to binary format
     * @param command Command to serialize
     * @param buffer Output buffer
     * @return true if serialization succeeded
     */
        bool SerializeCommand(const EngineCommand& command, std::vector<uint8_t>& buffer);

        /**
     * @brief Create named pipe for communication
     * @return true if pipe creation succeeded
     */
        bool CreateCommPipe();

        /**
     * @brief Connect to existing named pipe
     * @return true if connection succeeded
     */
        bool ConnectToNamedPipe();

        /**
     * @brief Generate unique command ID
     * @return Unique command identifier
     */
        uint64_t GenerateCommandID();

        /**
     * @brief Generate unique event ID
     * @return Unique event identifier
     */
        uint64_t GenerateEventID();

      private:
        // Connection state
        std::atomic<bool> m_isConnected{false};    ///< Connection status
        std::atomic<bool> m_isShuttingDown{false}; ///< Shutdown flag
        std::string m_pipeName;                    ///< Named pipe name
        void* m_pipeHandle = nullptr;              ///< Pipe handle (platform-specific)

        // Threading
        std::unique_ptr<std::thread> m_commThread; ///< Communication thread
        mutable std::mutex m_commandMutex;         ///< Command queue mutex
        mutable std::mutex m_eventMutex;           ///< Event queue mutex
        mutable std::mutex m_metricsMutex;         ///< Metrics mutex

        // Command/Event queues
        std::queue<EngineCommand> m_outgoingCommands; ///< Commands to send
        std::queue<EngineEvent> m_incomingEvents;     ///< Events received

        // Event callbacks
        std::unordered_map<EngineEventType, std::function<void(const EngineEvent&)>> m_eventCallbacks;

        // Engine state
        EngineMetrics m_currentMetrics; ///< Latest engine metrics
        EngineSystemInfo m_systemInfo;  ///< Engine system information

        // Configuration
        float m_commandTimeout = 5.0f;    ///< Command timeout in seconds
        bool m_autoReconnect = true;      ///< Auto-reconnect on disconnect
        float m_reconnectInterval = 2.0f; ///< Reconnect attempt interval

        // Statistics
        mutable ConnectionStats m_connectionStats; ///< Connection statistics

        // ID generation
        std::atomic<uint64_t> m_nextCommandID{1}; ///< Next command ID
        std::atomic<uint64_t> m_nextEventID{1};   ///< Next event ID

        // Timing
        std::chrono::steady_clock::time_point m_lastReconnectAttempt; ///< Last reconnect attempt time
        std::chrono::steady_clock::time_point m_connectionStartTime;  ///< Connection start time
    };

} // namespace SparkEditor
