/**
 * @file SeamlessAreaManager.h
 * @brief Predictive asset streaming for seamless area transitions
 * @author Spark Engine Team
 * @date 2026
 *
 * Tracks player position, velocity, and camera direction to predict which
 * world areas will be needed soon. Areas are preloaded before the player
 * reaches them, enabling seamless transitions without loading screens.
 *
 * Integrates with the AreaServer architecture for area definitions and
 * WorldOriginSystem for large-world coordinate support.
 *
 * ## Usage
 * @code
 *   auto& mgr = Spark::Streaming::SeamlessAreaManager::GetInstance();
 *   mgr.Initialize();
 *
 *   // Define areas
 *   AreaDefinition town;
 *   town.areaId = 1;
 *   town.name = "TownSquare";
 *   town.boundsMin = {-500, -100, -500};
 *   town.boundsMax = {500, 200, 500};
 *   mgr.RegisterArea(town);
 *
 *   // Per-frame update
 *   mgr.Update(dt);
 *
 *   mgr.Shutdown();
 * @endcode
 *
 * @see AreaServer.h, WorldOriginSystem.h
 */

#pragma once

#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace DirectX;

namespace Spark::Streaming
{

    /// @brief Unique area identifier (matches AreaServer AreaID convention)
    using AreaID = uint32_t;
    constexpr AreaID INVALID_AREA_ID = 0;

    // ========================================================================
    // Area State
    // ========================================================================

    /**
     * @brief Current loading state of a world area
     */
    enum class AreaState : uint8_t
    {
        Unloaded, ///< Not in memory
        Loading,  ///< Async load in progress
        Loaded,   ///< Fully loaded and active
        Unloading ///< Async unload in progress
    };

    // ========================================================================
    // Area Definition
    // ========================================================================

    /**
     * @brief Static definition of a world area (bounds, scene path, priority)
     */
    struct AreaDefinition
    {
        AreaID areaId = INVALID_AREA_ID;
        std::string name;
        std::string scenePath; ///< Path to the area's scene/asset bundle

        XMFLOAT3 boundsMin{0, 0, 0}; ///< AABB minimum corner (world space)
        XMFLOAT3 boundsMax{0, 0, 0}; ///< AABB maximum corner (world space)

        int priority = 0; ///< Higher = more important (loaded first on tie)
    };

    /**
     * @brief Runtime state for a managed area
     */
    struct ManagedArea
    {
        AreaDefinition definition;
        AreaState state = AreaState::Unloaded;
        float distanceToPlayer = 0.0f;     ///< Current distance from player
        float predictedArrivalTime = 0.0f; ///< Estimated time until player enters
    };

    // ========================================================================
    // Streaming Configuration
    // ========================================================================

    /**
     * @brief Tuning parameters for the predictive streaming system
     */
    struct StreamingConfig
    {
        float loadRadius = 500.0f;       ///< Distance at which areas begin loading
        float unloadRadius = 800.0f;     ///< Distance at which loaded areas are unloaded
        float lookaheadTime = 3.0f;      ///< Seconds to predict ahead using velocity
        float updateInterval = 0.25f;    ///< Seconds between prediction recalculations
        uint32_t maxConcurrentLoads = 2; ///< Maximum areas loading simultaneously
    };

    // ========================================================================
    // Callback Types
    // ========================================================================

    /**
     * @brief Called when an area finishes loading or begins unloading
     * @param areaId The area that changed state
     * @param newState The new state of the area
     */
    using AreaStateChangedCallback = std::function<void(AreaID areaId, AreaState newState)>;

    // ========================================================================
    // SeamlessAreaManager
    // ========================================================================

    /**
     * @brief Predictive area streaming manager
     *
     * Tracks the player's position and velocity to predict which areas
     * will be needed. Queues load/unload requests to keep a moving window
     * of loaded areas around the player, enabling seamless world traversal.
     *
     * Thread safety: SetPlayerState() may be called from any thread.
     * All other methods are main-thread only.
     */
    class SeamlessAreaManager
    {
      public:
        static SeamlessAreaManager& GetInstance()
        {
            static SeamlessAreaManager instance;
            return instance;
        }

        // -- Lifecycle --

        void Initialize();
        void Update(float dt);
        void Shutdown();

        // -- Area registration --

        /**
         * @brief Register a new area definition
         * @param def The area definition (must have a valid areaId)
         */
        void RegisterArea(const AreaDefinition& def);

        /**
         * @brief Remove an area from the streaming system
         * @param areaId Area to remove
         */
        void UnregisterArea(AreaID areaId);

        // -- Player tracking --

        /**
         * @brief Update the player's movement state for prediction
         *
         * Thread-safe. May be called from physics or gameplay threads.
         *
         * @param position    Current world-space position
         * @param velocity    Current velocity vector
         * @param cameraDir   Camera forward direction (normalized)
         */
        void SetPlayerState(const XMFLOAT3& position, const XMFLOAT3& velocity, const XMFLOAT3& cameraDir);

        // -- Configuration --

        void SetConfig(const StreamingConfig& config) { m_config = config; }
        const StreamingConfig& GetConfig() const { return m_config; }

        // -- Queries --

        AreaState GetAreaState(AreaID areaId) const;
        const std::vector<AreaID>& GetLoadedAreas() const { return m_loadedAreaIds; }
        AreaID GetCurrentArea() const { return m_currentAreaId; }

        // -- Callbacks --

        void RegisterStateCallback(AreaStateChangedCallback callback);

        // -- Diagnostics --

        std::string Console_GetStatus() const;

      private:
        SeamlessAreaManager() = default;

        // Prediction and streaming logic
        XMFLOAT3 PredictFuturePosition() const;
        float DistanceToArea(const XMFLOAT3& point, const AreaDefinition& area) const;
        void UpdateAreaDistances();
        void ProcessLoadQueue();
        void ProcessUnloadQueue();
        AreaID FindContainingArea(const XMFLOAT3& point) const;
        void TransitionAreaState(ManagedArea& area, AreaState newState);

        // Area database
        std::unordered_map<AreaID, ManagedArea> m_areas;
        std::vector<AreaID> m_loadedAreaIds;
        AreaID m_currentAreaId = INVALID_AREA_ID;

        // Player state (protected by mutex for thread safety)
        mutable std::mutex m_playerMutex;
        XMFLOAT3 m_playerPosition{0, 0, 0};
        XMFLOAT3 m_playerVelocity{0, 0, 0};
        XMFLOAT3 m_cameraDirection{0, 0, 1};

        // Streaming configuration
        StreamingConfig m_config;

        // Timing
        float m_timeSinceLastUpdate = 0.0f;
        bool m_initialized = false;

        // State change callbacks
        std::vector<AreaStateChangedCallback> m_stateCallbacks;

        // Load queue (areas sorted by priority/distance)
        std::vector<AreaID> m_loadQueue;
        uint32_t m_activeLoadCount = 0;
    };

} // namespace Spark::Streaming
