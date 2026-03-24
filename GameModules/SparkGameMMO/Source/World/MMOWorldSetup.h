/**
 * @file MMOWorldSetup.h
 * @brief Configures the MMO world: area definitions, server topology, streaming
 * @author Spark Engine Team
 * @date 2026
 *
 * Demonstrates how a game module sets up the WorldServer/AreaServer
 * architecture with multiple world areas, seamless streaming boundaries,
 * and world origin rebasing for large-world support.
 *
 * World layout (showcase):
 *   - TownSquare (safe zone, social hub, area 1)
 *   - Wilderness  (open-world PvE, area 2)
 *   - Dungeon     (instanced PvE, area 3)
 *   - Battleground (PvP zone, area 4)
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Engine/World/WorldOriginSystem.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/WorldServer.h"
#endif

namespace MMO
{

    /// @brief Describes a single world area for the MMO demo
    struct MMOAreaInfo
    {
        uint32_t areaId = 0;
        std::string name;
        float boundsMinX = 0.0f;
        float boundsMinY = 0.0f;
        float boundsMinZ = 0.0f;
        float boundsMaxX = 0.0f;
        float boundsMaxY = 0.0f;
        float boundsMaxZ = 0.0f;
        int maxPlayers = 64;
        bool isPvPEnabled = false;
        bool isInstanced = false;
        std::string description;
    };

    /**
     * @brief Sets up the MMO world topology and wires engine subsystems
     *
     * On Initialize:
     * 1. Defines world areas (town, wilderness, dungeon, battleground)
     * 2. Registers areas with SeamlessAreaManager for predictive streaming
     * 3. Configures AreaServer instances via WorldServer (when networking enabled)
     * 4. Sets up WorldOriginSystem rebasing threshold for large worlds
     * 5. Configures the SpatialGrid for efficient entity queries
     */
    class MMOWorldSetup
    {
      public:
        MMOWorldSetup() = default;
        ~MMOWorldSetup();

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();

        void RenderDebugUI();

#ifdef ENABLE_NETWORKING
        /// Start NetworkManager as a dedicated server and wire all handlers
        bool StartNetworkServer(uint16_t port);

        /// Process one server tick: NM update + client tracking + WorldServer bridging
        void ServerTick(float deltaTime);

        /// Stop the network server
        void StopNetworkServer();

        /// Get the WorldServer instance (for tests)
        Spark::Net::WorldServer* GetWorldServer() const { return m_worldServer.get(); }
#endif

        size_t GetAreaCount() const { return m_areas.size(); }
        const std::vector<MMOAreaInfo>& GetAreas() const { return m_areas; }
        std::string GetWorldStatusString() const;
        std::string GetAreaListString() const;

      private:
        void DefineWorldAreas();
        void RegisterAreasWithStreaming();
        void ConfigureWorldServer();
        void ConfigureOriginRebasing();

        Spark::IEngineContext* m_context{nullptr};
        std::vector<MMOAreaInfo> m_areas;
        Spark::World::WorldOriginSystem m_originSystem;
#ifdef ENABLE_NETWORKING
        std::unique_ptr<Spark::Net::WorldServer> m_worldServer;
        std::unordered_map<Spark::Net::ClientID, bool> m_knownClients; ///< Clients we've seen (for delta detection)
        bool m_networkServerRunning{false};
#endif
        float m_worldTime{0.0f};
        bool m_initialized{false};
    };

} // namespace MMO
