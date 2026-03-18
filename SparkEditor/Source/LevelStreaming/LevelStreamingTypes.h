/**
 * @file LevelStreamingTypes.h
 * @brief Type definitions for the level streaming and world composition system
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains all enums, structs, and configuration types used by LevelStreamingSystem.
 */

#pragma once

#include "../Enums/LevelStreamingEnums.h"
#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
using namespace DirectX;
#include <vector>
#include <string>
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

} // namespace SparkEditor
