/**
 * @file RTSUnitSystem.h
 * @brief Unit management: templates, spawning, stats, and AI states
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages all RTS units across factions. Each faction has predefined unit
 * templates with balanced stats. Units track health, position, and
 * behavioral state (idle, moving, attacking, gathering).
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RTSEnums.h"

#include <cstdint>
#include <string>
#include <map>
#include <vector>

namespace RTS
{

    /// @brief Cost to produce a unit
    struct UnitCost
    {
        int minerals = 0;
        int gas = 0;
        int supply = 1;
    };

    /// @brief Static template defining a unit type's base stats
    struct UnitTemplate
    {
        RTSUnitType type = RTSUnitType::Worker;
        RTSFaction faction = RTSFaction::Human;
        std::string name;
        float maxHealth = 100.0f;
        float damage = 10.0f;
        float attackSpeed = 1.0f; ///< Attacks per second
        float moveSpeed = 3.0f;   ///< Units per second
        float visionRange = 8.0f; ///< Fog of war reveal radius
        float attackRange = 1.0f; ///< Attack reach
        float buildTime = 10.0f;  ///< Seconds to produce
        UnitCost cost;
    };

    /// @brief Runtime data for a spawned unit instance
    struct UnitData
    {
        uint32_t unitId = 0;
        RTSUnitType type = RTSUnitType::Worker;
        RTSFaction faction = RTSFaction::Human;
        RTSUnitState state = RTSUnitState::Idle;
        float health = 100.0f;
        float maxHealth = 100.0f;
        float damage = 10.0f;
        float attackSpeed = 1.0f;
        float moveSpeed = 3.0f;
        float visionRange = 8.0f;
        float posX = 0.0f;
        float posY = 0.0f;
        uint32_t targetId = 0; ///< Target unit/building for attack/gather
    };

    /**
     * @brief Manages unit templates, spawning, lifecycle, and simple AI
     */
    class RTSUnitSystem
    {
      public:
        RTSUnitSystem() = default;
        ~RTSUnitSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // === Unit lifecycle ===
        uint32_t SpawnUnit(RTSUnitType type, RTSFaction faction, float x, float y);
        void KillUnit(uint32_t unitId);

        // === Queries ===
        const UnitData* GetUnit(uint32_t unitId) const;
        UnitData* GetUnitMutable(uint32_t unitId);
        std::vector<uint32_t> GetUnitsByFaction(RTSFaction faction) const;
        size_t GetUnitCount() const;
        size_t GetUnitCountByFaction(RTSFaction faction) const;
        const UnitTemplate* GetTemplate(RTSUnitType type, RTSFaction faction) const;
        std::string GetUnitListString() const;

        /** @brief Id the next spawned unit receives (ids are never reused, so this is persistent state). */
        uint32_t GetNextUnitId() const;

        /**
         * @brief Replace all runtime units from a validated persistence snapshot.
         * @param nextUnitId  Id the next spawn receives; 0 derives it as one past the highest restored id,
         *                    otherwise it must exceed every restored id.
         * @return false (leaving state untouched) if any record or the id counter is invalid.
         */
        bool RestoreState(const std::vector<UnitData>& units, uint32_t nextUnitId = 0);

        // === Unit state ===
        void SetUnitState(uint32_t unitId, RTSUnitState state);
        void SetUnitTarget(uint32_t unitId, uint32_t targetId);

      private:
        void RegisterFactionTemplates(RTSFaction faction);
        void UpdateUnitAI(UnitData& unit, float deltaTime);

        Spark::IEngineContext* m_context{nullptr};

        // Ordered by id: every per-tick walk must visit units in the same order on every run and platform.
        std::map<uint32_t, UnitData> m_units;
        std::vector<UnitTemplate> m_templates;
        uint32_t m_nextUnitId = 1;
    };

} // namespace RTS
