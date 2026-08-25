/**
 * @file RTSResourceSystem.cpp
 * @brief Economy: resource gathering, supply management, and node tracking
 */

#include "RTSResourceSystem.h"
#include "Unit/RTSUnitSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace RTS
{

    bool RTSResourceSystem::Initialize(Spark::IEngineContext* context, RTSUnitSystem* unitSystem)
    {
        m_context = context;
        m_unitSystem = unitSystem;
        m_gatherTimer = 0.0f;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "RTS resource system initialized");
        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Resource system initialized");
        return true;
    }

    void RTSResourceSystem::Update(float deltaTime)
    {
        if (std::isfinite(deltaTime) && deltaTime > 0.0f)
            GatherResources(deltaTime);

        // Remove depleted nodes with no assigned workers
        for (auto it = m_nodes.begin(); it != m_nodes.end();)
        {
            if (it->second.remaining <= 0 && it->second.assignedWorkers.empty())
                it = m_nodes.erase(it);
            else
                ++it;
        }
    }

    void RTSResourceSystem::Shutdown()
    {
        m_playerResources.clear();
        m_nodes.clear();
        m_unitSystem = nullptr;
        m_context = nullptr;
        m_gatherTimer = 0.0f;
        m_nextNodeId = 1;
    }

    // === Player resources ===

    void RTSResourceSystem::InitializePlayer(RTSFaction faction)
    {
        PlayerResources resources;
        resources.minerals = 400;
        resources.gas = 0;
        resources.currentSupply = 0;
        resources.maxSupply = 10; // Starting supply from first command center
        m_playerResources[faction] = resources;
    }

    bool RTSResourceSystem::CanAfford(RTSFaction faction, int minerals, int gas) const
    {
        auto it = m_playerResources.find(faction);
        if (it == m_playerResources.end())
            return false;

        return it->second.minerals >= minerals && it->second.gas >= gas;
    }

    bool RTSResourceSystem::SpendResources(RTSFaction faction, int minerals, int gas)
    {
        if (!CanAfford(faction, minerals, gas))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "RTS cannot afford: need %d minerals, %d gas", minerals, gas);
            return false;
        }

        auto& res = m_playerResources[faction];
        res.minerals -= minerals;
        res.gas -= gas;
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "RTS resources spent: %d minerals, %d gas", minerals, gas);
        return true;
    }

    void RTSResourceSystem::AddResources(RTSFaction faction, int minerals, int gas)
    {
        auto it = m_playerResources.find(faction);
        if (it == m_playerResources.end())
            return;

        it->second.minerals += minerals;
        it->second.gas += gas;
    }

    const PlayerResources* RTSResourceSystem::GetPlayerResources(RTSFaction faction) const
    {
        auto it = m_playerResources.find(faction);
        return it != m_playerResources.end() ? &it->second : nullptr;
    }

    void RTSResourceSystem::AddSupply(RTSFaction faction, int amount)
    {
        auto it = m_playerResources.find(faction);
        if (it != m_playerResources.end())
            it->second.maxSupply += amount;
    }

    void RTSResourceSystem::UseSupply(RTSFaction faction, int amount)
    {
        auto it = m_playerResources.find(faction);
        if (it != m_playerResources.end())
            it->second.currentSupply += amount;
    }

    void RTSResourceSystem::FreeSupply(RTSFaction faction, int amount)
    {
        auto it = m_playerResources.find(faction);
        if (it != m_playerResources.end())
            it->second.currentSupply = std::max(0, it->second.currentSupply - amount);
    }

    bool RTSResourceSystem::CanUseSupply(RTSFaction faction, int amount) const
    {
        const auto it = m_playerResources.find(faction);
        if (it == m_playerResources.end() || amount < 0)
            return false;
        return it->second.currentSupply + amount <= it->second.maxSupply;
    }

    // === Resource nodes ===

    uint32_t RTSResourceSystem::CreateNode(RTSResourceType type, float x, float y, int amount)
    {
        if ((type != RTSResourceType::Minerals && type != RTSResourceType::Gas) || !std::isfinite(x) ||
            !std::isfinite(y) || amount <= 0)
        {
            return 0;
        }

        ResourceNode node;
        node.nodeId = m_nextNodeId++;
        node.type = type;
        node.posX = x;
        node.posY = y;
        node.remaining = amount;
        node.maxWorkers = (type == RTSResourceType::Minerals) ? 3 : 2;

        uint32_t id = node.nodeId;
        m_nodes[id] = std::move(node);
        return id;
    }

    bool RTSResourceSystem::AssignWorker(uint32_t nodeId, uint32_t workerId)
    {
        auto it = m_nodes.find(nodeId);
        if (it == m_nodes.end())
            return false;

        if (m_unitSystem)
        {
            const UnitData* unit = m_unitSystem->GetUnit(workerId);
            if (!unit || unit->type != RTSUnitType::Worker || unit->state == RTSUnitState::Dead)
                return false;
        }

        auto& node = it->second;
        if (static_cast<int>(node.assignedWorkers.size()) >= node.maxWorkers)
            return false;

        // Prevent duplicate assignment
        for (uint32_t id : node.assignedWorkers)
        {
            if (id == workerId)
                return false;
        }

        node.assignedWorkers.push_back(workerId);
        return true;
    }

    void RTSResourceSystem::UnassignWorker(uint32_t nodeId, uint32_t workerId)
    {
        auto it = m_nodes.find(nodeId);
        if (it == m_nodes.end())
            return;

        auto& workers = it->second.assignedWorkers;
        workers.erase(std::remove(workers.begin(), workers.end(), workerId), workers.end());
    }

    // === Queries ===

    size_t RTSResourceSystem::GetNodeCount() const
    {
        return m_nodes.size();
    }

    const std::unordered_map<uint32_t, ResourceNode>& RTSResourceSystem::GetNodes() const
    {
        return m_nodes;
    }

    std::string RTSResourceSystem::GetResourceListString() const
    {
        std::string result = "=== RTS Resources ===\n";

        for (const auto& [faction, res] : m_playerResources)
        {
            std::string factionName;
            switch (faction)
            {
            case RTSFaction::Human:
                factionName = "Human";
                break;
            case RTSFaction::Sentinel:
                factionName = "Sentinel";
                break;
            case RTSFaction::Swarm:
                factionName = "Swarm";
                break;
            default:
                factionName = "Unknown";
                break;
            }

            result += factionName + ": ";
            result += "Minerals=" + std::to_string(res.minerals);
            result += " Gas=" + std::to_string(res.gas);
            result += " Supply=" + std::to_string(res.currentSupply);
            result += "/" + std::to_string(res.maxSupply) + "\n";
        }

        result += "Resource nodes: " + std::to_string(m_nodes.size()) + "\n";
        return result;
    }

    bool RTSResourceSystem::RestoreState(const std::vector<std::pair<RTSFaction, PlayerResources>>& players,
                                         const std::vector<ResourceNode>& nodes)
    {
        std::unordered_map<RTSFaction, PlayerResources> restoredPlayers;
        restoredPlayers.reserve(players.size());
        for (const auto& [faction, resources] : players)
        {
            if (faction >= RTSFaction::Count || resources.minerals < 0 || resources.gas < 0 ||
                resources.currentSupply < 0 || resources.maxSupply < 0 ||
                resources.currentSupply > resources.maxSupply || !restoredPlayers.emplace(faction, resources).second)
            {
                return false;
            }
        }

        std::unordered_map<uint32_t, ResourceNode> restoredNodes;
        restoredNodes.reserve(nodes.size());
        uint32_t nextId = 1;
        for (const ResourceNode& node : nodes)
        {
            if (node.nodeId == 0 || node.nodeId == std::numeric_limits<uint32_t>::max() ||
                (node.type != RTSResourceType::Minerals && node.type != RTSResourceType::Gas) ||
                !std::isfinite(node.posX) || !std::isfinite(node.posY) || node.remaining < 0 || node.maxWorkers <= 0 ||
                node.assignedWorkers.size() > static_cast<size_t>(node.maxWorkers))
            {
                return false;
            }
            std::unordered_set<uint32_t> workers;
            for (uint32_t workerId : node.assignedWorkers)
            {
                if (workerId == 0 || !workers.insert(workerId).second)
                    return false;
            }
            if (!restoredNodes.emplace(node.nodeId, node).second)
                return false;
            nextId = std::max(nextId, node.nodeId + 1);
        }

        m_playerResources = std::move(restoredPlayers);
        m_nodes = std::move(restoredNodes);
        m_nextNodeId = nextId;
        m_gatherTimer = 0.0f;
        return true;
    }

    // === Internal ===

    void RTSResourceSystem::GatherResources(float deltaTime)
    {
        m_gatherTimer += deltaTime;
        if (m_gatherTimer < GATHER_INTERVAL)
            return;

        m_gatherTimer -= GATHER_INTERVAL;

        for (auto& [nodeId, node] : m_nodes)
        {
            if (node.remaining <= 0 || node.assignedWorkers.empty())
                continue;

            if (m_unitSystem)
            {
                std::erase_if(node.assignedWorkers,
                              [this](uint32_t workerId)
                              {
                                  const UnitData* unit = m_unitSystem->GetUnit(workerId);
                                  return !unit || unit->type != RTSUnitType::Worker ||
                                         unit->state == RTSUnitState::Dead;
                              });
            }

            for (uint32_t workerId : node.assignedWorkers)
            {
                if (node.remaining <= 0)
                    break;

                const UnitData* worker = m_unitSystem ? m_unitSystem->GetUnit(workerId) : nullptr;
                const RTSFaction faction = worker ? worker->faction : RTSFaction::Human;
                const int perWorker = node.type == RTSResourceType::Minerals ? MINERALS_PER_TRIP : GAS_PER_TRIP;
                const int harvestAmount = std::min(perWorker, node.remaining);
                node.remaining -= harvestAmount;

                if (node.type == RTSResourceType::Minerals)
                    AddResources(faction, harvestAmount, 0);
                else if (node.type == RTSResourceType::Gas)
                    AddResources(faction, 0, harvestAmount);
            }
        }
    }

    void RTSResourceSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RTS Resource System"))
        {
            for (const auto& [faction, res] : m_playerResources)
            {
                const char* name = "Unknown";
                switch (faction)
                {
                case RTSFaction::Human:
                    name = "Human";
                    break;
                case RTSFaction::Sentinel:
                    name = "Sentinel";
                    break;
                case RTSFaction::Swarm:
                    name = "Swarm";
                    break;
                default:
                    break;
                }

                ImGui::Text("%s: M=%d G=%d Supply=%d/%d", name, res.minerals, res.gas, res.currentSupply,
                            res.maxSupply);
            }
            ImGui::Text("Resource nodes: %zu", m_nodes.size());
            ImGui::TreePop();
        }
#endif
    }

} // namespace RTS
