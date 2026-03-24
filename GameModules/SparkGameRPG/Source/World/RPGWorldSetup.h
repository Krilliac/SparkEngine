/**
 * @file RPGWorldSetup.h
 * @brief Defines RPG world areas, connections, and encounter tables
 * @author Spark Engine Team
 * @date 2026
 *
 * Sets up the RPG world with 5 areas (Village, Forest, Dungeon, Castle, Swamp),
 * connections between them for seamless transitions, and per-area enemy
 * encounter tables. Integrates with SeamlessAreaManager for streaming.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Engine/World/WorldOriginSystem.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RPG
{

    /// @brief An enemy type that can spawn in an area
    struct EncounterEntry
    {
        uint32_t enemyId = 0;
        std::string enemyName;
        int minLevel = 1;
        int maxLevel = 5;
        float spawnWeight = 1.0f; ///< Relative probability
    };

    /// @brief Describes a single RPG world area
    struct RPGAreaInfo
    {
        uint32_t areaId = 0;
        std::string name;
        std::string description;
        float boundsMinX = 0.0f;
        float boundsMinY = 0.0f;
        float boundsMinZ = 0.0f;
        float boundsMaxX = 0.0f;
        float boundsMaxY = 0.0f;
        float boundsMaxZ = 0.0f;
        int suggestedLevel = 1;
        bool isSafeZone = false;
        std::vector<uint32_t> connectedAreas; ///< IDs of adjacent areas
        std::vector<EncounterEntry> encounters;
    };

    /**
     * @brief World area registry, connections, and streaming configuration
     */
    class RPGWorldSetup
    {
      public:
        RPGWorldSetup() = default;
        ~RPGWorldSetup() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        size_t GetAreaCount() const { return m_areas.size(); }
        const std::vector<RPGAreaInfo>& GetAreas() const { return m_areas; }
        const RPGAreaInfo* GetArea(uint32_t areaId) const;
        std::string GetAreaListString() const;

      private:
        void DefineWorldAreas();
        void RegisterAreasWithStreaming();
        void ConfigureOriginRebasing();

        Spark::IEngineContext* m_context{nullptr};
        std::vector<RPGAreaInfo> m_areas;
        Spark::World::WorldOriginSystem m_originSystem;
        float m_worldTime{0.0f};
        bool m_initialized{false};
    };

} // namespace RPG
