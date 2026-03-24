/**
 * @file RTSResourceSystem.h
 * @brief Economy: minerals, gas, supply, resource nodes, and worker assignment
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages the RTS economy including resource nodes (mineral patches, gas
 * geysers), per-player resource pools, worker-to-node assignment, and
 * supply tracking.
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

    /// @brief Per-player resource pool
    struct PlayerResources
    {
        int minerals = 400; ///< Starting minerals
        int gas = 0;
        int currentSupply = 0;
        int maxSupply = 10; ///< From command centers and supply depots
    };

    /// @brief A harvestable resource node on the map
    struct ResourceNode
    {
        uint32_t nodeId = 0;
        RTSResourceType type = RTSResourceType::Minerals;
        float posX = 0.0f;
        float posY = 0.0f;
        int remaining = 1500; ///< Resources left to harvest
        int maxWorkers = 3;   ///< Max workers that can gather here
        std::vector<uint32_t> assignedWorkers;
    };

    /**
     * @brief Manages resources, nodes, worker assignment, and supply
     */
    class RTSResourceSystem
    {
      public:
        RTSResourceSystem() = default;
        ~RTSResourceSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // === Player resources ===
        void InitializePlayer(RTSFaction faction);
        bool CanAfford(RTSFaction faction, int minerals, int gas) const;
        bool SpendResources(RTSFaction faction, int minerals, int gas);
        void AddResources(RTSFaction faction, int minerals, int gas);
        const PlayerResources* GetPlayerResources(RTSFaction faction) const;
        void AddSupply(RTSFaction faction, int amount);
        void UseSupply(RTSFaction faction, int amount);
        void FreeSupply(RTSFaction faction, int amount);

        // === Resource nodes ===
        uint32_t CreateNode(RTSResourceType type, float x, float y, int amount);
        bool AssignWorker(uint32_t nodeId, uint32_t workerId);
        void UnassignWorker(uint32_t nodeId, uint32_t workerId);

        // === Queries ===
        size_t GetNodeCount() const;
        std::string GetResourceListString() const;

      private:
        void GatherResources(float deltaTime);

        Spark::IEngineContext* m_context{nullptr};

        std::unordered_map<RTSFaction, PlayerResources> m_playerResources;
        std::unordered_map<uint32_t, ResourceNode> m_nodes;
        uint32_t m_nextNodeId = 1;

        static constexpr float GATHER_INTERVAL = 2.0f; ///< Seconds per resource tick
        static constexpr int MINERALS_PER_TRIP = 8;
        static constexpr int GAS_PER_TRIP = 4;
        float m_gatherTimer = 0.0f;
    };

} // namespace RTS
