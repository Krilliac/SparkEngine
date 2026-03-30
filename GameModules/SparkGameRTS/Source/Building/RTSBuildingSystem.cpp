/**
 * @file RTSBuildingSystem.cpp
 * @brief Building templates, placement, construction timers, and production queues
 */

#include "RTSBuildingSystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>

namespace RTS
{

    bool RTSBuildingSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;

        RegisterFactionTemplates(RTSFaction::Human);
        RegisterFactionTemplates(RTSFaction::Sentinel);
        RegisterFactionTemplates(RTSFaction::Swarm);

        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Building system initialized (" +
                                                    std::to_string(m_templates.size()) + " templates)");
        return true;
    }

    void RTSBuildingSystem::Update(float deltaTime)
    {
        UpdateConstruction(deltaTime);
        UpdateProduction(deltaTime);

        // Remove destroyed buildings
        for (auto it = m_buildings.begin(); it != m_buildings.end();)
        {
            if (it->second.health <= 0.0f)
                it = m_buildings.erase(it);
            else
                ++it;
        }
    }

    void RTSBuildingSystem::Shutdown()
    {
        m_buildings.clear();
        m_templates.clear();
    }

    // === Building lifecycle ===

    uint32_t RTSBuildingSystem::PlaceBuilding(RTSBuildingType type, RTSFaction faction, float x, float y)
    {
        const BuildingTemplate* tmpl = GetTemplate(type, faction);
        if (!tmpl)
            return 0;

        BuildingData building;
        building.buildingId = m_nextBuildingId++;
        building.type = type;
        building.faction = faction;
        building.health = tmpl->maxHealth * 0.1f; // Starts at 10% HP during construction
        building.maxHealth = tmpl->maxHealth;
        building.posX = x;
        building.posY = y;
        building.constructionComplete = false;
        building.constructionProgress = 0.0f;
        building.constructionTime = tmpl->buildTime;

        uint32_t id = building.buildingId;
        m_buildings[id] = std::move(building);
        return id;
    }

    void RTSBuildingSystem::DestroyBuilding(uint32_t buildingId)
    {
        auto it = m_buildings.find(buildingId);
        if (it != m_buildings.end())
            it->second.health = 0.0f;
    }

    // === Production ===

    bool RTSBuildingSystem::StartProduction(uint32_t buildingId, RTSUnitType unitType)
    {
        auto it = m_buildings.find(buildingId);
        if (it == m_buildings.end() || !it->second.constructionComplete)
            return false;

        const BuildingTemplate* tmpl = GetTemplate(it->second.type, it->second.faction);
        if (!tmpl)
            return false;

        // Verify this building can produce the requested unit type
        bool canProduce = false;
        for (RTSUnitType producible : tmpl->producibleUnits)
        {
            if (producible == unitType)
            {
                canProduce = true;
                break;
            }
        }
        if (!canProduce)
            return false;

        // Queue limit: max 5 items per building
        if (it->second.productionQueue.size() >= 5)
            return false;

        ProductionEntry entry;
        entry.unitType = unitType;
        entry.totalTime = 18.0f; // Default production time; would look up from unit template
        entry.timeRemaining = entry.totalTime;
        it->second.productionQueue.push_back(entry);
        return true;
    }

    // === Queries ===

    const BuildingData* RTSBuildingSystem::GetBuilding(uint32_t buildingId) const
    {
        auto it = m_buildings.find(buildingId);
        return it != m_buildings.end() ? &it->second : nullptr;
    }

    std::vector<uint32_t> RTSBuildingSystem::GetBuildingsByFaction(RTSFaction faction) const
    {
        std::vector<uint32_t> result;
        for (const auto& [id, bld] : m_buildings)
        {
            if (bld.faction == faction)
                result.push_back(id);
        }
        return result;
    }

    size_t RTSBuildingSystem::GetBuildingCount() const
    {
        return m_buildings.size();
    }

    size_t RTSBuildingSystem::GetBuildingCountByFaction(RTSFaction faction) const
    {
        size_t count = 0;
        for (const auto& [id, bld] : m_buildings)
        {
            if (bld.faction == faction)
                count++;
        }
        return count;
    }

    const BuildingTemplate* RTSBuildingSystem::GetTemplate(RTSBuildingType type, RTSFaction faction) const
    {
        for (const auto& tmpl : m_templates)
        {
            if (tmpl.type == type && tmpl.faction == faction)
                return &tmpl;
        }
        return nullptr;
    }

    int RTSBuildingSystem::GetSupplyProvided(RTSFaction faction) const
    {
        int total = 0;
        for (const auto& [id, bld] : m_buildings)
        {
            if (bld.faction == faction && bld.constructionComplete)
            {
                const BuildingTemplate* tmpl = GetTemplate(bld.type, faction);
                if (tmpl)
                    total += tmpl->supplyProvided;
            }
        }
        return total;
    }

    std::string RTSBuildingSystem::GetBuildingListString() const
    {
        std::string result = "=== RTS Buildings ===\n";
        result += "Total: " + std::to_string(m_buildings.size()) + "\n";

        for (const auto& [id, bld] : m_buildings)
        {
            result += "  [" + std::to_string(id) + "] ";
            result += "HP:" + std::to_string(static_cast<int>(bld.health));
            result += "/" + std::to_string(static_cast<int>(bld.maxHealth));
            result += bld.constructionComplete ? " [Complete]" : " [Building]";
            result += " Queue:" + std::to_string(bld.productionQueue.size()) + "\n";
        }
        return result;
    }

    // === Internal ===

    void RTSBuildingSystem::RegisterFactionTemplates(RTSFaction faction)
    {
        float healthMul = 1.0f;
        float costMul = 1.0f;
        float buildMul = 1.0f;

        switch (faction)
        {
        case RTSFaction::Human:
            break;
        case RTSFaction::Sentinel:
            healthMul = 1.4f;
            costMul = 1.3f;
            buildMul = 1.2f;
            break;
        case RTSFaction::Swarm:
            healthMul = 0.8f;
            costMul = 0.7f;
            buildMul = 0.8f;
            break;
        default:
            break;
        }

        auto add = [&](RTSBuildingType type, const std::string& name, float hp, float time, int min, int gas,
                       RTSBuildingType techReq, std::vector<RTSUnitType> units, int supply)
        {
            BuildingTemplate tmpl;
            tmpl.type = type;
            tmpl.faction = faction;
            tmpl.name = name;
            tmpl.maxHealth = hp * healthMul;
            tmpl.buildTime = time * buildMul;
            tmpl.cost = {static_cast<int>(min * costMul), static_cast<int>(gas * costMul)};
            tmpl.techRequirement = techReq;
            tmpl.producibleUnits = std::move(units);
            tmpl.supplyProvided = supply;
            m_templates.push_back(std::move(tmpl));
        };

        using BT = RTSBuildingType;
        using UT = RTSUnitType;

        add(BT::CommandCenter, "Command Center", 1500, 60, 400, 0, BT::Count, {UT::Worker}, 10);
        add(BT::SupplyDepot, "Supply Depot", 400, 20, 100, 0, BT::Count, {}, 8);
        add(BT::Barracks, "Barracks", 800, 40, 150, 0, BT::CommandCenter, {UT::Marine, UT::Medic}, 0);
        add(BT::Factory, "Factory", 1000, 50, 200, 100, BT::Barracks, {UT::Tank, UT::Siege}, 0);
        add(BT::Starport, "Starport", 900, 45, 200, 150, BT::Factory, {UT::Flyer, UT::Scout}, 0);
        add(BT::TechLab, "Tech Lab", 600, 35, 150, 100, BT::Barracks, {}, 0);
        add(BT::Refinery, "Refinery", 500, 25, 75, 0, BT::Count, {}, 0);
        add(BT::Turret, "Turret", 300, 15, 100, 50, BT::Barracks, {}, 0);
    }

    void RTSBuildingSystem::UpdateConstruction(float deltaTime)
    {
        for (auto& [id, bld] : m_buildings)
        {
            if (bld.constructionComplete)
                continue;

            bld.constructionProgress += deltaTime / bld.constructionTime;
            if (bld.constructionProgress >= 1.0f)
            {
                bld.constructionProgress = 1.0f;
                bld.constructionComplete = true;
                bld.health = bld.maxHealth;
            }
            else
            {
                // Health scales with construction progress (10% to 100%)
                float healthFraction = 0.1f + 0.9f * bld.constructionProgress;
                bld.health = bld.maxHealth * healthFraction;
            }
        }
    }

    void RTSBuildingSystem::UpdateProduction(float deltaTime)
    {
        for (auto& [id, bld] : m_buildings)
        {
            if (!bld.constructionComplete || bld.productionQueue.empty())
                continue;

            // Only process the front of the queue
            auto& front = bld.productionQueue.front();
            front.timeRemaining -= deltaTime;

            if (front.timeRemaining <= 0.0f)
            {
                // Unit produced -- the module's OnUpdate will read completed units
                Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Building " + std::to_string(id) +
                                                            " finished producing a unit");
                bld.productionQueue.erase(bld.productionQueue.begin());
            }
        }
    }

    void RTSBuildingSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RTS Building System"))
        {
            ImGui::Text("Active buildings: %zu", m_buildings.size());
            ImGui::Text("Templates: %zu", m_templates.size());

            if (ImGui::TreeNode("Buildings by Faction"))
            {
                ImGui::Text("Human: %zu", GetBuildingCountByFaction(RTSFaction::Human));
                ImGui::Text("Sentinel: %zu", GetBuildingCountByFaction(RTSFaction::Sentinel));
                ImGui::Text("Swarm: %zu", GetBuildingCountByFaction(RTSFaction::Swarm));
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace RTS
