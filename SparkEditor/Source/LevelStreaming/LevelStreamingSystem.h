/**
 * @file LevelStreamingSystem.h
 * @brief Level streaming and world composition system for large worlds in Spark Engine
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file implements a comprehensive level streaming system similar to Unreal's
 * World Composition and Unity's Addressables, enabling seamless large world
 * management with efficient memory usage and background loading.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../SceneSystem/SceneFile.h"
#include "../Enums/LevelStreamingEnums.h"
#include <DirectXMath.h>
using namespace DirectX;
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <future>


namespace SparkEditor
{

    /**
 * @brief Level streaming state
 */
    enum class StreamingState
    {
        UNLOADED = 0,  ///< Level is not loaded
        LOADING = 1,   ///< Level is currently loading
        LOADED = 2,    ///< Level is fully loaded
        UNLOADING = 3, ///< Level is currently unloading
        FAILED = 4     ///< Loading/unloading failed
    };

    /**
 * @brief Level of detail settings
 */
    enum class LODLevel
    {
        LOD_0 = 0, ///< Highest detail (closest)
        LOD_1 = 1, ///< High detail
        LOD_2 = 2, ///< Medium detail
        LOD_3 = 3, ///< Low detail
        LOD_4 = 4, ///< Lowest detail (furthest)
        LOD_COUNT = 5
    };

    // StreamingMethod is defined in ../Enums/LevelStreamingEnums.h (included via SceneFile.h or directly)
    // Using the canonical definition from LevelStreamingEnums.h

    /**
 * @brief World tile information
 */
    struct WorldTile
    {
        std::string name;                       ///< Tile name
        std::string filePath;                   ///< Path to tile scene file
        XMFLOAT3 worldPosition = {0, 0, 0};     ///< World position of tile center
        XMFLOAT3 worldSize = {1000, 100, 1000}; ///< World size of tile
        XMFLOAT2 tileCoordinates = {0, 0};      ///< Grid coordinates (x, y)

        // Streaming settings
        StreamingMethod streamingMethod = StreamingMethod::DISTANCE_BASED;
        float streamingDistance = 2000.0f; ///< Distance at which to start streaming
        float unloadingDistance = 3000.0f; ///< Distance at which to unload
        int priority = 0;                  ///< Streaming priority (higher = more important)
        bool alwaysLoaded = false;         ///< Whether tile should always be loaded
        bool blockOnLoad = false;          ///< Whether to block on loading

        // LOD settings
        std::vector<float> lodDistances = {500, 1000, 1500, 2000, 2500}; ///< LOD transition distances
        std::vector<std::string> lodMeshPaths;                           ///< Paths to LOD meshes

        // Runtime state
        StreamingState state = StreamingState::UNLOADED; ///< Current streaming state
        LODLevel currentLOD = LODLevel::LOD_0;           ///< Current LOD level
        float lastUpdateTime = 0.0f;                     ///< Last update time
        size_t memoryUsage = 0;                          ///< Current memory usage in bytes

        // Loading data
        std::future<bool> loadingTask; ///< Async loading task
        float loadingProgress = 0.0f;  ///< Loading progress (0-1)
        std::string errorMessage;      ///< Error message if loading failed

        // Dependencies
        std::vector<std::string> dependencies; ///< Tiles this tile depends on
        std::vector<std::string> dependents;   ///< Tiles that depend on this tile

        // Visibility and culling
        bool isVisible = true;                    ///< Whether tile is visible
        bool isCulled = false;                    ///< Whether tile is currently culled
        XMFLOAT4 boundingSphere = {0, 0, 0, 500}; ///< Bounding sphere (xyz = center, w = radius)

        /**
     * @brief Check if point is within tile bounds
     * @param point World position to check
     * @return true if point is within tile
     */
        bool ContainsPoint(const XMFLOAT3& point) const;

        /**
     * @brief Get distance from point to tile center
     * @param point World position
     * @return Distance to tile center
     */
        float GetDistanceToCenter(const XMFLOAT3& point) const;

        /**
     * @brief Get distance from point to tile bounds
     * @param point World position
     * @return Distance to nearest tile boundary (negative if inside)
     */
        float GetDistanceToBounds(const XMFLOAT3& point) const;

        /**
     * @brief Calculate appropriate LOD level for distance
     * @param distance Distance from viewer
     * @return Appropriate LOD level
     */
        LODLevel CalculateLOD(float distance) const;
    };

    /**
 * @brief Streaming volume for trigger-based streaming
 */
    struct StreamingVolume
    {
        std::string name;                       ///< Volume name
        XMFLOAT3 center = {0, 0, 0};            ///< Volume center
        XMFLOAT3 size = {100, 100, 100};        ///< Volume size
        std::vector<std::string> tilesToLoad;   ///< Tiles to load when entering
        std::vector<std::string> tilesToUnload; ///< Tiles to unload when exiting
        bool isActive = true;                   ///< Whether volume is active
        bool playerInside = false;              ///< Whether player is currently inside

        /**
     * @brief Check if point is inside volume
     * @param point World position to check
     * @return true if point is inside volume
     */
        bool ContainsPoint(const XMFLOAT3& point) const;
    };

    /**
 * @brief World composition settings
 */
    struct WorldCompositionSettings
    {
        // Grid settings
        XMFLOAT2 tileSize = {1000, 1000}; ///< Default tile size
        int maxTilesX = 64;               ///< Maximum tiles in X direction
        int maxTilesY = 64;               ///< Maximum tiles in Y direction
        bool autoGenerateGrid = true;     ///< Auto-generate tile grid

        // Streaming settings
        StreamingMethod defaultStreamingMethod = StreamingMethod::DISTANCE_BASED;
        float defaultStreamingDistance = 2000.0f; ///< Default streaming distance
        float defaultUnloadingDistance = 3000.0f; ///< Default unloading distance
        bool enablePredictiveStreaming = true;    ///< Enable predictive streaming
        float predictionTime = 2.0f;              ///< Prediction time in seconds

        // Memory management
        size_t maxMemoryBudget = 2ULL * 1024 * 1024 * 1024; ///< Max memory budget (2GB)
        size_t softMemoryLimit = 1536ULL * 1024 * 1024;     ///< Soft memory limit (1.5GB)
        bool enableMemoryPressureUnloading = true;          ///< Unload when memory pressure high

        // LOD settings
        bool enableLOD = true;                  ///< Enable level of detail
        float lodBias = 1.0f;                   ///< LOD bias multiplier
        bool enableSmoothLODTransitions = true; ///< Enable smooth LOD transitions

        // Performance settings
        int maxConcurrentLoads = 4;         ///< Maximum concurrent loading operations
        int maxLoadingFrameTime = 16;       ///< Maximum time per frame for loading (ms)
        bool loadInBackground = true;       ///< Load tiles in background threads
        bool enableOcclusionCulling = true; ///< Enable occlusion culling

        // Quality settings
        bool enableHighQualityPreview = false; ///< Enable high-quality preview in editor
        bool showDebugInfo = false;            ///< Show debug information
        bool showTileBounds = false;           ///< Show tile boundaries
        bool showStreamingVolumes = false;     ///< Show streaming volumes
    };

    /**
 * @brief Streaming statistics
 */
    struct StreamingStatistics
    {
        int totalTiles = 0;             ///< Total number of tiles
        int loadedTiles = 0;            ///< Currently loaded tiles
        int loadingTiles = 0;           ///< Currently loading tiles
        int unloadingTiles = 0;         ///< Currently unloading tiles
        size_t memoryUsage = 0;         ///< Current memory usage
        size_t peakMemoryUsage = 0;     ///< Peak memory usage
        float averageLoadTime = 0.0f;   ///< Average loading time
        float averageUnloadTime = 0.0f; ///< Average unloading time
        int loadRequests = 0;           ///< Total load requests
        int unloadRequests = 0;         ///< Total unload requests
        int failedLoads = 0;            ///< Failed loading operations
        float frameTime = 0.0f;         ///< Current frame time
        float streamingOverhead = 0.0f; ///< Streaming overhead per frame
    };

    /**
 * @brief Player/camera information for streaming
 */
    struct StreamingViewer
    {
        XMFLOAT3 position = {0, 0, 0}; ///< Current position
        XMFLOAT3 velocity = {0, 0, 0}; ///< Current velocity
        XMFLOAT3 forward = {0, 0, 1};  ///< Forward direction
        float fieldOfView = 70.0f;     ///< Field of view in degrees
        bool isActive = true;          ///< Whether viewer is active

        /**
     * @brief Get predicted position
     * @param predictionTime Time to predict ahead
     * @return Predicted position
     */
        XMFLOAT3 GetPredictedPosition(float predictionTime) const;

        /**
     * @brief Check if position is within view frustum
     * @param position Position to check
     * @param radius Object radius
     * @return true if object is potentially visible
     */
        bool IsInViewFrustum(const XMFLOAT3& position, float radius) const;
    };

    /**
 * @brief Level streaming and world composition system
 * 
 * Provides comprehensive world streaming capabilities including:
 * - Automatic level streaming based on distance and triggers
 * - Level of detail (LOD) management for large worlds
 * - Background loading with memory budget management
 * - Predictive streaming based on player movement
 * - Occlusion culling and performance optimization
 * - Multi-threaded loading and unloading
 * - Real-time world composition editing
 * - Integration with asset pipeline
 * 
 * Inspired by Unreal's World Composition and Unity's Addressables system.
 */
    class LevelStreamingSystem : public EditorPanel
    {
      public:
        /**
     * @brief Constructor
     */
        LevelStreamingSystem();

        /**
     * @brief Destructor
     */
        ~LevelStreamingSystem() override;

        /**
     * @brief Initialize the level streaming system
     * @return true if initialization succeeded
     */
        bool Initialize() override;

        /**
     * @brief Update level streaming system
     * @param deltaTime Time elapsed since last update
     */
        void Update(float deltaTime) override;

        /**
     * @brief Render level streaming UI
     */
        void Render() override;

        /**
     * @brief Shutdown the level streaming system
     */
        void Shutdown() override;

        /**
     * @brief Handle panel events
     * @param eventType Event type
     * @param eventData Event data
     * @return true if event was handled
     */
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /**
     * @brief Create new world composition
     * @param name World name
     * @param settings World composition settings
     */
        void CreateNewWorld(const std::string& name, const WorldCompositionSettings& settings);

        /**
     * @brief Load world composition from file
     * @param filePath Path to world file
     * @return true if world was loaded successfully
     */
        bool LoadWorld(const std::string& filePath);

        /**
     * @brief Save current world composition to file
     * @param filePath Path to save world
     * @return true if world was saved successfully
     */
        bool SaveWorld(const std::string& filePath);

        /**
     * @brief Add tile to world
     * @param tile Tile to add
     * @return true if tile was added successfully
     */
        bool AddTile(const WorldTile& tile);

        /**
     * @brief Remove tile from world
     * @param tileName Name of tile to remove
     * @return true if tile was removed
     */
        bool RemoveTile(const std::string& tileName);

        /**
     * @brief Get tile by name
     * @param tileName Tile name
     * @return Pointer to tile, or nullptr if not found
     */
        WorldTile* GetTile(const std::string& tileName);

        /**
     * @brief Get tile at world position
     * @param worldPosition World position
     * @return Pointer to tile, or nullptr if not found
     */
        WorldTile* GetTileAtPosition(const XMFLOAT3& worldPosition);

        /**
     * @brief Get all tiles
     * @return Vector of all tiles
     */
        const std::vector<WorldTile>& GetAllTiles() const { return m_tiles; }

        /**
     * @brief Add streaming volume
     * @param volume Streaming volume to add
     */
        void AddStreamingVolume(const StreamingVolume& volume);

        /**
     * @brief Remove streaming volume
     * @param volumeName Name of volume to remove
     * @return true if volume was removed
     */
        bool RemoveStreamingVolume(const std::string& volumeName);

        /**
     * @brief Set streaming viewer (player/camera)
     * @param viewer Viewer information
     */
        void SetStreamingViewer(const StreamingViewer& viewer);

        /**
     * @brief Get current streaming viewer
     * @return Reference to current viewer
     */
        const StreamingViewer& GetStreamingViewer() const { return m_streamingViewer; }

        /**
     * @brief Request tile loading
     * @param tileName Name of tile to load
     * @param priority Loading priority
     * @param blockOnLoad Whether to block until loaded
     * @return true if load request was queued
     */
        bool RequestTileLoad(const std::string& tileName, int priority = 0, bool blockOnLoad = false);

        /**
     * @brief Request tile unloading
     * @param tileName Name of tile to unload
     * @param immediate Whether to unload immediately
     * @return true if unload request was queued
     */
        bool RequestTileUnload(const std::string& tileName, bool immediate = false);

        /**
     * @brief Force immediate tile loading
     * @param tileName Name of tile to load
     * @return true if tile was loaded successfully
     */
        bool ForceLoadTile(const std::string& tileName);

        /**
     * @brief Force immediate tile unloading
     * @param tileName Name of tile to unload
     * @return true if tile was unloaded successfully
     */
        bool ForceUnloadTile(const std::string& tileName);

        /**
     * @brief Get tiles within distance of position
     * @param position Center position
     * @param distance Search distance
     * @return Vector of tile pointers
     */
        std::vector<WorldTile*> GetTilesWithinDistance(const XMFLOAT3& position, float distance);

        /**
     * @brief Get tiles visible from position
     * @param position Viewer position
     * @param forward View direction
     * @param fieldOfView Field of view in degrees
     * @return Vector of visible tile pointers
     */
        std::vector<WorldTile*> GetVisibleTiles(const XMFLOAT3& position, const XMFLOAT3& forward, float fieldOfView);

        /**
     * @brief Update tile LOD based on distance
     * @param tileName Tile name
     * @param viewerDistance Distance from viewer
     */
        void UpdateTileLOD(const std::string& tileName, float viewerDistance);

        /**
     * @brief Get streaming statistics
     * @return Current streaming statistics
     */
        StreamingStatistics GetStreamingStatistics() const;

        /**
     * @brief Set world composition settings
     * @param settings New settings
     */
        void SetWorldSettings(const WorldCompositionSettings& settings);

        /**
     * @brief Get world composition settings
     * @return Reference to current settings
     */
        const WorldCompositionSettings& GetWorldSettings() const { return m_worldSettings; }

        /**
     * @brief Enable/disable automatic streaming
     * @param enabled Whether automatic streaming is enabled
     */
        void SetAutomaticStreaming(bool enabled) { m_automaticStreaming = enabled; }

        /**
     * @brief Check if automatic streaming is enabled
     * @return true if automatic streaming is enabled
     */
        bool IsAutomaticStreaming() const { return m_automaticStreaming; }

        /**
     * @brief Pause/resume streaming system
     * @param paused Whether streaming should be paused
     */
        void SetStreamingPaused(bool paused) { m_streamingPaused = paused; }

        /**
     * @brief Check if streaming is paused
     * @return true if streaming is paused
     */
        bool IsStreamingPaused() const { return m_streamingPaused; }

        /**
     * @brief Generate tile grid from heightmap
     * @param heightmapPath Path to heightmap texture
     * @param tileSize Size of each tile
     * @param worldSize Total world size
     * @return Number of tiles generated
     */
        int GenerateTileGridFromHeightmap(const std::string& heightmapPath, const XMFLOAT2& tileSize,
                                          const XMFLOAT2& worldSize);

        /**
     * @brief Optimize tile arrangement
     * @param targetMemoryUsage Target memory usage
     * @return Number of tiles optimized
     */
        int OptimizeTileArrangement(size_t targetMemoryUsage);

        /**
     * @brief Validate world composition
     * @param errors Output vector for validation errors
     * @return true if world is valid
     */
        bool ValidateWorld(std::vector<std::string>& errors) const;

      private:
        /**
     * @brief Render world overview
     */
        void RenderWorldOverview();

        /**
     * @brief Render tile list
     */
        void RenderTileList();

        /**
     * @brief Render streaming volumes
     */
        void RenderStreamingVolumes();

        /**
     * @brief Render world settings
     */
        void RenderWorldSettings();

        /**
     * @brief Render streaming statistics
     */
        void RenderStreamingStatistics();

        /**
     * @brief Render debugging information
     */
        void RenderDebugInfo();

        /**
     * @brief Update automatic streaming
     */
        void UpdateAutomaticStreaming();

        /**
     * @brief Update distance-based streaming
     */
        void UpdateDistanceBasedStreaming();

        /**
     * @brief Update trigger-based streaming
     */
        void UpdateTriggerBasedStreaming();

        /**
     * @brief Update predictive streaming
     */
        void UpdatePredictiveStreaming();

        /**
     * @brief Update memory management
     */
        void UpdateMemoryManagement();

        /**
     * @brief Update LOD system
     */
        void UpdateLODSystem();

        /**
     * @brief Process loading queue
     */
        void ProcessLoadingQueue();

        /**
     * @brief Process unloading queue
     */
        void ProcessUnloadingQueue();

        /**
     * @brief Background loading thread function
     */
        void BackgroundLoadingFunction();

        /**
     * @brief Load tile synchronously
     * @param tileName Tile name
     * @return true if loading succeeded
     */
        bool LoadTileSync(const std::string& tileName);

        /**
     * @brief Unload tile synchronously
     * @param tileName Tile name
     * @return true if unloading succeeded
     */
        bool UnloadTileSync(const std::string& tileName);

        /**
     * @brief Calculate tile priority
     * @param tile Tile to calculate priority for
     * @return Calculated priority
     */
        int CalculateTilePriority(const WorldTile& tile) const;

        /**
     * @brief Get memory usage of tile
     * @param tileName Tile name
     * @return Memory usage in bytes
     */
        size_t GetTileMemoryUsage(const std::string& tileName) const;

        /**
     * @brief Free memory by unloading least important tiles
     * @param targetMemory Target memory to free
     * @return Amount of memory actually freed
     */
        size_t FreeMemory(size_t targetMemory);

        /**
     * @brief Update streaming statistics
     */
        void UpdateStatistics();

      private:
        // World data
        std::string m_worldName = "New World";           ///< World name
        std::vector<WorldTile> m_tiles;                  ///< All tiles in world
        std::vector<StreamingVolume> m_streamingVolumes; ///< Streaming trigger volumes
        WorldCompositionSettings m_worldSettings;        ///< World composition settings

        // Streaming state
        StreamingViewer m_streamingViewer; ///< Current viewer for streaming
        bool m_automaticStreaming = true;  ///< Automatic streaming enabled
        bool m_streamingPaused = false;    ///< Streaming paused

        // Loading queues
        struct LoadingRequest
        {
            std::string tileName;
            int priority;
            bool blockOnLoad;
            std::chrono::steady_clock::time_point requestTime;

            bool operator<(const LoadingRequest& other) const
            {
                return priority < other.priority; // Higher priority first
            }
        };

        std::priority_queue<LoadingRequest> m_loadingQueue; ///< Loading request queue
        std::queue<std::string> m_unloadingQueue;           ///< Unloading request queue
        mutable std::mutex m_queueMutex;                    ///< Queue access mutex

        // Background processing
        std::vector<std::thread> m_loadingThreads;    ///< Background loading threads
        std::atomic<bool> m_shouldStopLoading{false}; ///< Stop loading flag
        std::condition_variable m_loadingCondition;   ///< Loading condition variable

        // Statistics and monitoring
        mutable StreamingStatistics m_statistics;                ///< Current statistics
        std::chrono::steady_clock::time_point m_lastStatsUpdate; ///< Last statistics update

        // UI state
        std::string m_selectedTile;          ///< Currently selected tile
        bool m_showWorldOverview = true;     ///< Show world overview panel
        bool m_showTileList = true;          ///< Show tile list panel
        bool m_showStreamingVolumes = false; ///< Show streaming volumes panel
        bool m_showStatistics = true;        ///< Show statistics panel
        bool m_showDebugInfo = false;        ///< Show debug information

        // Visualization
        bool m_showTileBounds = true;       ///< Show tile boundaries in viewport
        bool m_showStreamingRadii = true;   ///< Show streaming distances
        bool m_showLODColors = false;       ///< Color-code tiles by LOD
        float m_overviewZoom = 1.0f;        ///< World overview zoom level
        XMFLOAT2 m_overviewOffset = {0, 0}; ///< World overview pan offset
    };

} // namespace SparkEditor