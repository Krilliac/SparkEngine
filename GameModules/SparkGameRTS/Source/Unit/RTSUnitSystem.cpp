/**
 * @file RTSUnitSystem.cpp
 * @brief Unit templates, spawning, lifecycle, and behavioral AI
 */

#include "RTSUnitSystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>

namespace RTS
{

    bool RTSUnitSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;

        // Register unit templates for each faction
        RegisterFactionTemplates(RTSFaction::Human);
        RegisterFactionTemplates(RTSFaction::Sentinel);
        RegisterFactionTemplates(RTSFaction::Swarm);

        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Unit system initialized (" +
                                                    std::to_string(m_templates.size()) + " templates)");
        return true;
    }

    void RTSUnitSystem::Update(float deltaTime)
    {
        // Remove dead units
        for (auto it = m_units.begin(); it != m_units.end();)
        {
            if (it->second.state == RTSUnitState::Dead)
            {
                it = m_units.erase(it);
            }
            else
            {
                UpdateUnitAI(it->second, deltaTime);
                ++it;
            }
        }
    }

    void RTSUnitSystem::Shutdown()
    {
        m_units.clear();
        m_templates.clear();
    }

    // === Unit lifecycle ===

    uint32_t RTSUnitSystem::SpawnUnit(RTSUnitType type, RTSFaction faction, float x, float y)
    {
        const UnitTemplate* tmpl = GetTemplate(type, faction);
        if (!tmpl)
            return 0;

        UnitData unit;
        unit.unitId = m_nextUnitId++;
        unit.type = type;
        unit.faction = faction;
        unit.state = RTSUnitState::Idle;
        unit.health = tmpl->maxHealth;
        unit.maxHealth = tmpl->maxHealth;
        unit.damage = tmpl->damage;
        unit.attackSpeed = tmpl->attackSpeed;
        unit.moveSpeed = tmpl->moveSpeed;
        unit.visionRange = tmpl->visionRange;
        unit.posX = x;
        unit.posY = y;

        uint32_t id = unit.unitId;
        m_units[id] = unit;
        return id;
    }

    void RTSUnitSystem::KillUnit(uint32_t unitId)
    {
        auto it = m_units.find(unitId);
        if (it != m_units.end())
        {
            it->second.state = RTSUnitState::Dead;
            it->second.health = 0.0f;
        }
    }

    // === Queries ===

    const UnitData* RTSUnitSystem::GetUnit(uint32_t unitId) const
    {
        auto it = m_units.find(unitId);
        return it != m_units.end() ? &it->second : nullptr;
    }

    UnitData* RTSUnitSystem::GetUnitMutable(uint32_t unitId)
    {
        auto it = m_units.find(unitId);
        return it != m_units.end() ? &it->second : nullptr;
    }

    std::vector<uint32_t> RTSUnitSystem::GetUnitsByFaction(RTSFaction faction) const
    {
        std::vector<uint32_t> result;
        for (const auto& [id, unit] : m_units)
        {
            if (unit.faction == faction)
                result.push_back(id);
        }
        return result;
    }

    size_t RTSUnitSystem::GetUnitCount() const
    {
        return m_units.size();
    }

    size_t RTSUnitSystem::GetUnitCountByFaction(RTSFaction faction) const
    {
        size_t count = 0;
        for (const auto& [id, unit] : m_units)
        {
            if (unit.faction == faction)
                count++;
        }
        return count;
    }

    const UnitTemplate* RTSUnitSystem::GetTemplate(RTSUnitType type, RTSFaction faction) const
    {
        for (const auto& tmpl : m_templates)
        {
            if (tmpl.type == type && tmpl.faction == faction)
                return &tmpl;
        }
        return nullptr;
    }

    std::string RTSUnitSystem::GetUnitListString() const
    {
        std::string result = "=== RTS Units ===\n";
        result += "Total: " + std::to_string(m_units.size()) + "\n";

        for (const auto& [id, unit] : m_units)
        {
            result += "  [" + std::to_string(id) + "] ";
            result += "HP:" + std::to_string(static_cast<int>(unit.health));
            result += "/" + std::to_string(static_cast<int>(unit.maxHealth));
            result += " Pos:(" + std::to_string(static_cast<int>(unit.posX));
            result += "," + std::to_string(static_cast<int>(unit.posY)) + ")\n";
        }
        return result;
    }

    // === Unit state ===

    void RTSUnitSystem::SetUnitState(uint32_t unitId, RTSUnitState state)
    {
        auto it = m_units.find(unitId);
        if (it != m_units.end())
            it->second.state = state;
    }

    void RTSUnitSystem::SetUnitTarget(uint32_t unitId, uint32_t targetId)
    {
        auto it = m_units.find(unitId);
        if (it != m_units.end())
            it->second.targetId = targetId;
    }

    // === Internal ===

    void RTSUnitSystem::RegisterFactionTemplates(RTSFaction faction)
    {
        // Stat multipliers per faction to differentiate playstyles
        float healthMul = 1.0f;
        float damageMul = 1.0f;
        float speedMul = 1.0f;
        float costMul = 1.0f;

        switch (faction)
        {
        case RTSFaction::Human:
            // Balanced baseline
            break;
        case RTSFaction::Sentinel:
            healthMul = 1.3f; // Tougher units
            damageMul = 1.2f; // Higher damage
            costMul = 1.4f;   // More expensive
            speedMul = 0.9f;  // Slightly slower
            break;
        case RTSFaction::Swarm:
            healthMul = 0.7f; // Fragile
            damageMul = 0.8f; // Lower individual damage
            costMul = 0.6f;   // Cheap, mass-producible
            speedMul = 1.3f;  // Fast
            break;
        default:
            break;
        }

        auto addTemplate = [&](RTSUnitType type, const std::string& name, float hp, float dmg, float atkSpd, float spd,
                               float vision, float range, float buildTime, int min, int gas, int supply)
        {
            UnitTemplate tmpl;
            tmpl.type = type;
            tmpl.faction = faction;
            tmpl.name = name;
            tmpl.maxHealth = hp * healthMul;
            tmpl.damage = dmg * damageMul;
            tmpl.attackSpeed = atkSpd;
            tmpl.moveSpeed = spd * speedMul;
            tmpl.visionRange = vision;
            tmpl.attackRange = range;
            tmpl.buildTime = buildTime;
            tmpl.cost = {static_cast<int>(min * costMul), static_cast<int>(gas * costMul), supply};
            m_templates.push_back(tmpl);
        };

        // Base unit roster (stats before faction multipliers)
        addTemplate(RTSUnitType::Worker, "Worker", 40, 5, 0.8f, 3.0f, 7.0f, 1.0f, 12.0f, 50, 0, 1);
        addTemplate(RTSUnitType::Marine, "Marine", 55, 12, 1.0f, 3.5f, 8.0f, 5.0f, 18.0f, 50, 0, 1);
        addTemplate(RTSUnitType::Tank, "Tank", 150, 30, 0.5f, 2.0f, 9.0f, 7.0f, 30.0f, 150, 75, 3);
        addTemplate(RTSUnitType::Flyer, "Flyer", 80, 18, 1.2f, 5.0f, 10.0f, 5.0f, 25.0f, 100, 100, 2);
        addTemplate(RTSUnitType::Scout, "Scout", 35, 8, 1.5f, 6.0f, 12.0f, 4.0f, 15.0f, 25, 0, 1);
        addTemplate(RTSUnitType::Hero, "Hero", 300, 40, 1.0f, 3.0f, 10.0f, 2.0f, 60.0f, 300, 200, 6);
        addTemplate(RTSUnitType::Medic, "Medic", 50, 0, 0.0f, 3.0f, 8.0f, 4.0f, 20.0f, 50, 50, 1);
        addTemplate(RTSUnitType::Siege, "Siege", 120, 50, 0.3f, 1.5f, 8.0f, 12.0f, 35.0f, 200, 100, 3);
    }

    void RTSUnitSystem::UpdateUnitAI(UnitData& unit, float deltaTime)
    {
        (void)deltaTime;

        // Simple state-based AI skeleton
        switch (unit.state)
        {
        case RTSUnitState::Idle:
            // Idle units do nothing; await commands
            break;

        case RTSUnitState::Moving:
            // Movement would be handled by pathfinding integration
            break;

        case RTSUnitState::Attacking:
            // Attack logic handled by CommandSystem targeting
            break;

        case RTSUnitState::Gathering:
            // Resource gathering handled by ResourceSystem
            break;

        default:
            break;
        }
    }

    void RTSUnitSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RTS Unit System"))
        {
            ImGui::Text("Active units: %zu", m_units.size());
            ImGui::Text("Templates: %zu", m_templates.size());

            if (ImGui::TreeNode("Units by Faction"))
            {
                ImGui::Text("Human: %zu", GetUnitCountByFaction(RTSFaction::Human));
                ImGui::Text("Sentinel: %zu", GetUnitCountByFaction(RTSFaction::Sentinel));
                ImGui::Text("Swarm: %zu", GetUnitCountByFaction(RTSFaction::Swarm));
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace RTS
