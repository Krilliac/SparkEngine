/**
 * @file TFWorldSetupNet.cpp
 * @brief TFWorldSetup networking boot (frozen API): StartHost/StartDedicated/
 *        Connect, the WorldServer + single-continent AreaServer bring-up,
 *        NetworkManager session bridging and teardown. Scene/terrain load
 *        lives in TFWorldSetup.cpp (same class, split per the repo file-size
 *        rules — mirrors the TFRegionSystem/-Net split).
 */
#include "World/TFWorldSetup.h"

#include "Net/TFServerSim.h"
#include "Net/TFClientNet.h"

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/WorldServer.h"
#include "Engine/Networking/AreaServer.h"
#include "Engine/Networking/IAreaSimulation.h"
#endif

#include <string>
#include <type_traits>

namespace Terrafront
{

    namespace
    {

#ifdef ENABLE_NETWORKING
        /// Attach TFServerSim to the AreaServer tick if (and only if) it implements
        /// Spark::Net::IAreaSimulation. Template so this file compiles even while the
        /// TFServerSim agent has not landed the interface yet; at final build time
        /// the true branch is taken (frozen contract, DESIGN.md §2).
        template <typename TSim> void AttachSimulation(Spark::Net::AreaServer& area, TSim* sim)
        {
            if constexpr (std::is_base_of_v<Spark::Net::IAreaSimulation, TSim>)
            {
                area.SetSimulation(sim);
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFServerSim attached to AreaServer tick (%.0f Hz)",
                               kServerTickHz);
            }
            else
            {
                (void)area;
                (void)sim;
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "[TF] TFServerSim does not implement IAreaSimulation; area tick hook not attached");
            }
        }
#endif // ENABLE_NETWORKING

    } // namespace

    // ---------------------------------------------------------------------------
    // Networking boot (frozen API)
    // ---------------------------------------------------------------------------

    bool TFWorldSetup::StartHost(uint16_t port)
    {
#ifdef ENABLE_NETWORKING
        return BootServer(port, NetRole::ListenHost);
#else
        (void)port;
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] StartHost: built without ENABLE_NETWORKING");
        return false;
#endif
    }

    bool TFWorldSetup::StartDedicated(uint16_t port)
    {
#ifdef ENABLE_NETWORKING
        return BootServer(port, NetRole::DedicatedServer);
#else
        (void)port;
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] StartDedicated: built without ENABLE_NETWORKING");
        return false;
#endif
    }

    bool TFWorldSetup::Connect(const std::string& ip, uint16_t port)
    {
#ifdef ENABLE_NETWORKING
        // Reconnect hygiene (multimap-plumbing W13 gap #2, docs/TERRAFRONT_MULTIMAP.md
        // §4 item 2): Connect() can be called a second time on an already-used
        // instance — TFTravelSystem::ClientRequestContinentHop does exactly this
        // for a continent hop. Without tearing down first, the OLD m_worldServer/
        // m_areaServer/m_knownClients survive (leaked + still ticking) and the new
        // NetworkManager::Connect races whatever socket state the previous session
        // left behind. StopNetworking() is the same reset Shutdown() already uses;
        // it does NOT touch scene/collision/camera (those stay owned by the
        // single-continent-per-process boot — see the design doc), only the
        // networking layer, so this is safe to run before every (re)connect.
        if (m_netBooted)
        {
            if (m_ctx->clientNet)
                m_ctx->clientNet->Disconnect();
            StopNetworking();
        }

        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() && !nm.Initialize())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] Connect: NetworkManager init failed");
            return false;
        }
        if (!nm.Connect(ip, port, "TerrafrontPlayer"))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] Connect to %s:%u failed", ip.c_str(), port);
            return false;
        }
        m_netBooted = true;
        m_ctx->role = NetRole::Client;
        // TFClientNet observes the NetworkManager connection in its Update and
        // runs the TF handshake (WorldWelcome / FactionSelect / SpawnRequest).
        Spark::SimpleConsole::GetInstance().LogInfo("[TF] Connecting to " + ip + ":" + std::to_string(port));
        return true;
#else
        (void)ip;
        (void)port;
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] Connect: built without ENABLE_NETWORKING");
        return false;
#endif
    }

#ifdef ENABLE_NETWORKING

    bool TFWorldSetup::BootServer(uint16_t port, NetRole role)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() && !nm.Initialize())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] BootServer: NetworkManager init failed");
            return false;
        }
        if (!nm.StartServer(port, static_cast<int>(kMaxPlayers)))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] BootServer: StartServer on port %u failed", port);
            return false;
        }

        // WorldServer + ONE AreaServer covering the whole 4096m continent
        // (DESIGN.md §2 topology; region hexes are game logic, not areas).
        m_worldServer = std::make_unique<Spark::Net::WorldServer>();
        Spark::Net::WorldServerConfig wc{};
        wc.worldName = "TERRAFRONT " + std::string("Cindral Wastes");
        wc.port = static_cast<uint16_t>(port + 1);
        wc.interServerPort = static_cast<uint16_t>(port + 2);
        wc.maxTotalClients = static_cast<int>(kMaxPlayers);
        wc.tickRate = 10.0f;
        wc.enableLoadBalancing = false; // single area, nothing to balance
        if (!m_worldServer->Start(wc))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] WorldServer failed to start (continuing: NetworkManager carries gameplay)");
            m_worldServer.reset();
        }

        Spark::Net::AreaServerConfig ac{};
        ac.areaId = 1;
        ac.areaName = "CindralWastes";
        ac.scenePath = m_scenePath;
        ac.port = static_cast<uint16_t>(port + 3);
        ac.interServerPort = static_cast<uint16_t>(port + 4);
        ac.tickRate = kServerTickHz;
        ac.maxClients = static_cast<int>(kMaxPlayers);
        ac.enableAI = false;
        ac.enablePhysics = true;
        ac.enableScripting = false;
        if (m_worldServer)
            m_worldServer->RegisterAreaServer(ac);

        m_areaServer = std::make_unique<Spark::Net::AreaServer>();
        if (m_areaServer->Start(ac))
        {
            if (m_ctx->serverSim)
                AttachSimulation(*m_areaServer, m_ctx->serverSim);
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] AreaServer failed to start");
            m_areaServer.reset();
        }

        m_knownClients.clear();
        m_netBooted = true;
        m_ctx->role = role;

        const char* roleName = (role == NetRole::DedicatedServer) ? "dedicated server" : "listen host";
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] Serving Cindral Wastes as %s on port %u", roleName, port);
        Spark::SimpleConsole::GetInstance().LogInfo("[TF] " + std::string(roleName) + " up on port " +
                                                    std::to_string(port));
        return true;
    }

    void TFWorldSetup::BridgeWorldServerSessions()
    {
        if (!m_worldServer || !m_worldServer->IsRunning())
            return;

        auto& nm = Spark::Net::NetworkManager::GetInstance();
        const auto& clients = nm.GetClients();

        for (const auto& [clientId, info] : clients)
        {
            if (m_knownClients.insert(clientId).second)
                m_worldServer->HandlePlayerConnect(clientId, info.name, {0.0f, 0.0f, 0.0f});
        }
        for (auto it = m_knownClients.begin(); it != m_knownClients.end();)
        {
            if (clients.find(*it) == clients.end())
            {
                m_worldServer->HandlePlayerDisconnect(*it);
                it = m_knownClients.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void TFWorldSetup::StopNetworking()
    {
        if (m_areaServer)
        {
            m_areaServer->Stop();
            m_areaServer.reset();
        }
        // The transport owns only sockets/queues. Terrafront owns authenticated
        // sessions and must drain them while player/progression/database systems
        // are still valid, before NetworkManager discards its client list and
        // handler table. This also resets handler registration for a same-process
        // stop -> host restart.
        if (m_ctx && m_ctx->serverSim)
            m_ctx->serverSim->PrepareNetworkStop();
        if (m_worldServer)
        {
            m_worldServer->Stop();
            m_worldServer.reset();
        }
        if (m_netBooted)
        {
            auto& nm = Spark::Net::NetworkManager::GetInstance();
            if (nm.GetRole() == Spark::Net::NetworkRole::Server)
                nm.StopServer();
            else
                nm.Disconnect();
            nm.Shutdown();
            m_netBooted = false;
        }
        m_knownClients.clear();
        if (m_ctx)
            m_ctx->role = NetRole::Standalone;
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
