/**
 * @file RTSResourceSystem.cpp
 * @brief Economy: resource gathering, supply management, and node tracking
 */

#include "RTSResourceSystem.h"
#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include "Utils/LogMacros.h"

namespace RTS
{

    bool RTSResourceSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        m_gatherTimer = 0.0f;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[RTS] Resource system initialized");
        return true;
    }

    void RTSResourceSystem::Update(float deltaTime)
    {
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
            return false;

        auto& res = m_playerResources[faction];
        res.minerals -= minerals;
        res.gas -= gas;
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

    // === Resource nodes ===

    uint32_t RTSResourceSystem::CreateNode(RTSResourceType type, float x, float y, int amount)
    {
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

            int workerCount = static_cast<int>(node.assignedWorkers.size());
            int harvestAmount = 0;

            if (node.type == RTSResourceType::Minerals)
                harvestAmount = MINERALS_PER_TRIP * workerCount;
            else if (node.type == RTSResourceType::Gas)
                harvestAmount = GAS_PER_TRIP * workerCount;

            harvestAmount = std::min(harvestAmount, node.remaining);
            node.remaining -= harvestAmount;

            // Credit resources to all factions that have workers here
            // In a full implementation, workers would track their owning faction
            // For now, credit to Human as default
            if (node.type == RTSResourceType::Minerals)
                AddResources(RTSFaction::Human, harvestAmount, 0);
            else if (node.type == RTSResourceType::Gas)
                AddResources(RTSFaction::Human, 0, harvestAmount);
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
