/**
 * @file OWExplorationSystem.h
 * @brief Points of interest, map discovery, landmarks, and exploration rewards
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages the player's exploration progress: POI discovery, map fog reveal,
 * viewpoint unlocks, discovery XP, and completion tracking per region.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/OpenWorldEnums.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OpenWorld
{

    /// @brief A point of interest in the world
    struct PointOfInterest
    {
        uint32_t poiId = 0;
        std::string name;
        std::string description;
        POIType type = POIType::Landmark;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float discoveryRadius = 30.0f; ///< How close player must be to discover
        uint32_t regionId = 0;
        uint32_t xpReward = 50;
        bool hasSecret = false; ///< Contains hidden collectible
    };

    /// @brief Per-POI discovery tracking
    struct POIProgress
    {
        uint32_t poiId = 0;
        DiscoveryState state = DiscoveryState::Undiscovered;
        bool secretFound = false;
    };

    /// @brief Per-region exploration summary
    struct RegionExploration
    {
        uint32_t regionId = 0;
        uint32_t totalPOIs = 0;
        uint32_t discoveredPOIs = 0;
        uint32_t completedPOIs = 0;
        float completionPercent = 0.0f;
    };

    /// @brief Mutable discovery state stored in an OpenWorld save.
    struct ExplorationSaveState
    {
        std::vector<POIProgress> progress;
        uint32_t totalXPEarned = 0;
    };

    /**
     * @brief Exploration and discovery tracking system
     */
    class OWExplorationSystem
    {
      public:
        using DiscoveryCallback = std::function<void(const PointOfInterest&)>;

        OWExplorationSystem() = default;
        ~OWExplorationSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime, float playerX, float playerY, float playerZ);
        void Shutdown();
        void RenderDebugUI();

        size_t GetPOICount() const { return m_pois.size(); }
        size_t GetDiscoveredCount() const;
        const PointOfInterest* GetPOI(uint32_t poiId) const;
        float GetOverallCompletion() const;
        RegionExploration GetRegionExploration(uint32_t regionId) const;
        std::string GetExplorationString() const;
        std::string GetPOIListString() const;

        /// @brief Manually reveal a POI (e.g., from viewpoint or NPC hint)
        void RevealPOI(uint32_t poiId);

        /// @brief Mark a POI's secret as found
        void FindSecret(uint32_t poiId);

        ExplorationSaveState CaptureSaveState() const;
        bool RestoreSaveState(const ExplorationSaveState& state, std::string* error = nullptr);

        /// @brief Install a synchronous notification for newly visited POIs.
        void SetDiscoveryCallback(DiscoveryCallback callback) { m_onDiscovered = std::move(callback); }

      private:
        void DefinePointsOfInterest();
        void CheckDiscovery(float playerX, float playerY, float playerZ);
        void DiscoverPOI(uint32_t poiId);

        Spark::IEngineContext* m_context{nullptr};
        std::vector<PointOfInterest> m_pois;
        std::unordered_map<uint32_t, POIProgress> m_progress;
        uint32_t m_totalXPEarned{0};
        DiscoveryCallback m_onDiscovered;
        bool m_initialized{false};
    };

} // namespace OpenWorld
