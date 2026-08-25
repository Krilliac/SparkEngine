/**
 * @file OWWorldSetup.h
 * @brief Open world map with 8 biome regions, seamless streaming, and origin rebasing
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines a large contiguous open world divided into 8 biome regions, each with
 * distinct terrain, climate, wildlife, and resources. Integrates with
 * SeamlessAreaManager for predictive streaming and WorldOriginSystem for
 * floating-point precision at large distances.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Engine/World/WorldOriginSystem.h"
#include "Enums/OpenWorldEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenWorld
{

    /// @brief Defines a single biome region in the open world
    struct BiomeRegion
    {
        uint32_t regionId = 0;
        std::string name;
        std::string description;
        Biome biome = Biome::Grasslands;
        float boundsMinX = 0.0f;
        float boundsMinY = 0.0f;
        float boundsMinZ = 0.0f;
        float boundsMaxX = 0.0f;
        float boundsMaxY = 0.0f;
        float boundsMaxZ = 0.0f;
        float baseTemperature = 20.0f; ///< Celsius, base temperature for this biome
        float elevationMin = 0.0f;
        float elevationMax = 100.0f;
        int dangerLevel = 1; ///< 1-10 difficulty rating
        std::vector<ResourceType> abundantResources;
        std::vector<AnimalType> nativeWildlife;
        std::vector<uint32_t> connectedRegions; ///< Adjacent biome IDs
    };

    /// @brief Road or path connecting two regions
    struct WorldRoad
    {
        uint32_t roadId = 0;
        std::string name;
        uint32_t fromRegion = 0;
        uint32_t toRegion = 0;
        float safetyRating = 0.5f; ///< 0 = dangerous, 1 = fully patrolled
        bool isMainRoad = false;
    };

    /**
     * @brief World region registry, terrain configuration, and streaming setup
     *
     * Manages the open world's 8 biome regions, road network between them,
     * seamless area streaming via SeamlessAreaManager, and coordinate-space
     * management via WorldOriginSystem.
     */
    class OWWorldSetup
    {
      public:
        OWWorldSetup() = default;
        ~OWWorldSetup() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        size_t GetRegionCount() const { return m_regions.size(); }
        size_t GetRoadCount() const { return m_roads.size(); }
        const std::vector<BiomeRegion>& GetRegions() const { return m_regions; }
        const BiomeRegion* GetRegion(uint32_t regionId) const;
        const BiomeRegion* GetRegionAtPosition(float x, float z) const;
        const BiomeRegion* GetRegionAtPosition(float x, float y, float z) const;
        std::string GetRegionListString() const;
        std::string GetWorldStatusString() const;

      private:
        void DefineBiomeRegions();
        void DefineRoadNetwork();
        void RegisterAreasWithStreaming();
        void ConfigureOriginRebasing();

        Spark::IEngineContext* m_context{nullptr};
        std::vector<BiomeRegion> m_regions;
        std::vector<WorldRoad> m_roads;
        Spark::World::WorldOriginSystem m_originSystem;
        float m_worldTime{0.0f};
        bool m_initialized{false};
    };

} // namespace OpenWorld
