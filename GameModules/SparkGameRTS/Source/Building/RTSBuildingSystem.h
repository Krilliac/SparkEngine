/**
 * @file RTSBuildingSystem.h
 * @brief Base building: tech trees, production queues, and structure management
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages RTS structures: placement, construction timers, production queues
 * for units, tech requirements, and per-faction building templates.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RTSEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RTS
{

    class RTSResourceSystem;
    class RTSUnitSystem;

    /// @brief Cost to construct a building
    struct BuildingCost
    {
        int minerals = 0;
        int gas = 0;
    };

    /// @brief An entry in a building's production queue
    struct ProductionEntry
    {
        RTSUnitType unitType = RTSUnitType::Worker;
        float timeRemaining = 0.0f;
        float totalTime = 0.0f;
    };

    /// @brief Static template defining a building type's properties
    struct BuildingTemplate
    {
        RTSBuildingType type = RTSBuildingType::CommandCenter;
        RTSFaction faction = RTSFaction::Human;
        std::string name;
        float maxHealth = 500.0f;
        float buildTime = 30.0f;
        BuildingCost cost;
        RTSBuildingType techRequirement = RTSBuildingType::Count; ///< Count = no requirement
        std::vector<RTSUnitType> producibleUnits;
        int supplyProvided = 0; ///< Supply added when completed (e.g., SupplyDepot)
    };

    /// @brief Runtime data for a placed building instance
    struct BuildingData
    {
        uint32_t buildingId = 0;
        RTSBuildingType type = RTSBuildingType::CommandCenter;
        RTSFaction faction = RTSFaction::Human;
        float health = 500.0f;
        float maxHealth = 500.0f;
        float posX = 0.0f;
        float posY = 0.0f;
        bool constructionComplete = false;
        float constructionProgress = 0.0f; ///< 0.0 to 1.0
        float constructionTime = 30.0f;
        std::vector<ProductionEntry> productionQueue;
    };

    /**
     * @brief Manages building templates, placement, construction, and production
     */
    class RTSBuildingSystem
    {
      public:
        RTSBuildingSystem() = default;
        ~RTSBuildingSystem() = default;

        bool Initialize(Spark::IEngineContext* context, RTSUnitSystem* unitSystem = nullptr,
                        RTSResourceSystem* resourceSystem = nullptr);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // === Building lifecycle ===
        uint32_t PlaceBuilding(RTSBuildingType type, RTSFaction faction, float x, float y);
        void DestroyBuilding(uint32_t buildingId);

        // === Production ===
        bool StartProduction(uint32_t buildingId, RTSUnitType unitType);

        // === Queries ===
        const BuildingData* GetBuilding(uint32_t buildingId) const;
        std::vector<uint32_t> GetBuildingsByFaction(RTSFaction faction) const;
        size_t GetBuildingCount() const;
        size_t GetBuildingCountByFaction(RTSFaction faction) const;
        const BuildingTemplate* GetTemplate(RTSBuildingType type, RTSFaction faction) const;
        int GetSupplyProvided(RTSFaction faction) const;
        std::string GetBuildingListString() const;

        /** Replace all runtime buildings from a validated persistence snapshot. */
        bool RestoreState(const std::vector<BuildingData>& buildings);

      private:
        void RegisterFactionTemplates(RTSFaction faction);
        void ReleaseQueuedSupply(const BuildingData& building);
        void UpdateConstruction(float deltaTime);
        void UpdateProduction(float deltaTime);

        Spark::IEngineContext* m_context{nullptr};
        RTSUnitSystem* m_unitSystem{nullptr};
        RTSResourceSystem* m_resourceSystem{nullptr};

        std::unordered_map<uint32_t, BuildingData> m_buildings;
        std::vector<BuildingTemplate> m_templates;
        uint32_t m_nextBuildingId = 1;
    };

} // namespace RTS
