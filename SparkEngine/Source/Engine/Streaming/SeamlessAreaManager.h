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
#include "AreaAssetLoader.h"
#include "SceneManifest.h"
#include "SeamlessAreaManagerTypes.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace DirectX;

namespace Spark::Streaming
{

    // Area/state/config types (AreaID, AreaState, AreaDefinition, ManagedArea,
    // StreamingConfig, AreaStateChangedCallback) live in SeamlessAreaManagerTypes.h.

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
         * @brief Register an area with an associated asset manifest
         * @param def The area definition
         * @param manifest The scene manifest listing assets to load for this area
         */
        void RegisterArea(const AreaDefinition& def, SceneManifest manifest);

        /**
         * @brief Set or replace the scene manifest for an area
         * @param areaId Area to set the manifest for
         * @param manifest The scene manifest listing assets to load
         */
        void SetManifest(AreaID areaId, SceneManifest manifest);

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

        // -- Asset loader --

        /**
         * @brief Get the area asset loader (for diagnostics or direct access)
         */
        AreaAssetLoader& GetAssetLoader() { return m_assetLoader; }
        const AreaAssetLoader& GetAssetLoader() const { return m_assetLoader; }

        // -- Diagnostics --

        std::string Console_GetStatus() const;

      private:
        SeamlessAreaManager() = default;

        // Prediction and streaming logic
        XMFLOAT3 PredictFuturePosition() const;
        float DistanceToArea(const XMFLOAT3& point, const AreaDefinition& area) const;

        /**
         * @brief Snapshot of the player state used by directional bias calculations.
         *
         * Captured once per Update tick so the sort comparator and radius tests
         * see a consistent value (SetPlayerState() may run on other threads,
         * which would otherwise break std::sort's strict-weak-ordering).
         */
        struct DirectionalSnapshot
        {
            XMFLOAT3 position{0, 0, 0};
            XMFLOAT3 direction{0, 0, 1};
            bool valid = false;
        };

        /// @brief Build a snapshot under a single mutex lock.
        DirectionalSnapshot SnapshotPlayerDirection() const;

        /**
         * @brief Compute a distance biased by alignment with the player's movement direction.
         *
         * @param rawDistance  The raw distance to the area
         * @param area         The area definition
         * @param snap         A pre-captured player direction snapshot
         */
        float DirectionalEffectiveDistance(float rawDistance, const AreaDefinition& area,
                                           const DirectionalSnapshot& snap) const;
        void UpdateAreaDistances();
        void ProcessLoadQueue();
        void ProcessUnloadQueue();
        AreaID FindContainingArea(const XMFLOAT3& point) const;
        void TransitionAreaState(ManagedArea& area, AreaState newState);

        // Asset loader bridge
        AreaAssetLoader m_assetLoader;

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

        /// @brief History of all area IDs that have ever been registered (for diagnostics).
        std::unordered_set<AreaID> m_registeredHistory;
    };

} // namespace Spark::Streaming
