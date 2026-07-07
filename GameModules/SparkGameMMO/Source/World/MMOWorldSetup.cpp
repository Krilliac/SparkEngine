/**
 * @file MMOWorldSetup.cpp
 * @brief MMO world area definitions and server topology configuration
 */

#include "MMOWorldSetup.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/WorldServer.h"
#include "Engine/Networking/AreaServer.h"
#include "Engine/Networking/NetworkManager.h"
#endif

#include "Engine/Streaming/SeamlessAreaManager.h"
#include "Engine/World/WorldOriginSystem.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace MMO
{

    MMOWorldSetup::~MMOWorldSetup()
    {
        Shutdown();
    }

    bool MMOWorldSetup::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Initializing MMO world setup");

        DefineWorldAreas();
        RegisterAreasWithStreaming();
        ConfigureWorldServer();
        ConfigureOriginRebasing();

        m_initialized = true;
        return true;
    }

    void MMOWorldSetup::DefineWorldAreas()
    {
        m_areas.clear();

        // NOTE: Area bounds and metadata are also defined in each area's scene file
        // (Assets/Scenes/MMO/*.scene) as JSON header fields (areaId, maxPlayers, pvpEnabled).
        // Area boundary volumes can be placed as [AreaBoundary] entities in the editor.
        // The code below defines the server-side authoritative area list.
        // Both approaches work — the scene files provide visual editor placement,
        // while this code provides the server topology.

        // Area 1: TownSquare — social hub, safe zone, vendors/NPCs
        // Editor: place AreaBoundary entity in town_square.scene with matching bounds
        MMOAreaInfo town{};
        town.areaId = 1;
        town.name = "TownSquare";
        town.boundsMinX = -500.0f;
        town.boundsMinY = -50.0f;
        town.boundsMinZ = -500.0f;
        town.boundsMaxX = 500.0f;
        town.boundsMaxY = 200.0f;
        town.boundsMaxZ = 500.0f;
        town.maxPlayers = 128;
        town.isPvPEnabled = false;
        town.isInstanced = false;
        town.description = "Central hub - safe zone with vendors and quest givers";
        m_areas.push_back(town);

        // Area 2: Wilderness — open world PvE exploration
        // Editor: place AreaBoundary entity in wilderness.scene with matching bounds
        MMOAreaInfo wilderness{};
        wilderness.areaId = 2;
        wilderness.name = "Wilderness";
        wilderness.boundsMinX = 500.0f;
        wilderness.boundsMinY = -100.0f;
        wilderness.boundsMinZ = -2000.0f;
        wilderness.boundsMaxX = 5000.0f;
        wilderness.boundsMaxY = 500.0f;
        wilderness.boundsMaxZ = 2000.0f;
        wilderness.maxPlayers = 200;
        wilderness.isPvPEnabled = false;
        wilderness.isInstanced = false;
        wilderness.description = "Open-world PvE zone with mob spawns and resource nodes";
        m_areas.push_back(wilderness);

        // Area 3: Dungeon — instanced PvE content
        // Editor: place AreaBoundary entity in shadow_crypt.scene with matching bounds
        MMOAreaInfo dungeon{};
        dungeon.areaId = 3;
        dungeon.name = "ShadowCrypt";
        dungeon.boundsMinX = -200.0f;
        dungeon.boundsMinY = -300.0f;
        dungeon.boundsMinZ = -200.0f;
        dungeon.boundsMaxX = 200.0f;
        dungeon.boundsMaxY = 0.0f;
        dungeon.boundsMaxZ = 200.0f;
        dungeon.maxPlayers = 5;
        dungeon.isPvPEnabled = false;
        dungeon.isInstanced = true;
        dungeon.description = "5-player instanced dungeon with boss encounters";
        m_areas.push_back(dungeon);

        // Area 4: Battleground — PvP arena
        // Editor: place AreaBoundary entity in battleground.scene with matching bounds
        MMOAreaInfo battleground{};
        battleground.areaId = 4;
        battleground.name = "Battleground";
        battleground.boundsMinX = -1000.0f;
        battleground.boundsMinY = -50.0f;
        battleground.boundsMinZ = 2000.0f;
        battleground.boundsMaxX = 1000.0f;
        battleground.boundsMaxY = 300.0f;
        battleground.boundsMaxZ = 4000.0f;
        battleground.maxPlayers = 40;
        battleground.isPvPEnabled = true;
        battleground.isInstanced = true;
        battleground.description = "20v20 PvP battleground with capture points";
        m_areas.push_back(battleground);

        SPARK_LOG_INFO(Spark::LogCategory::Game, "MMO world: %zu areas defined", m_areas.size());
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO World] Defined " + std::to_string(m_areas.size()) + " world areas");
    }

    void MMOWorldSetup::RegisterAreasWithStreaming()
    {
        auto& streamingMgr = Spark::Streaming::SeamlessAreaManager::GetInstance();

        for (const auto& area : m_areas)
        {
            // Skip instanced areas — they are loaded on-demand, not streamed
            if (area.isInstanced)
                continue;

            Spark::Streaming::AreaDefinition def{};
            def.areaId = area.areaId;
            def.name = area.name;
            def.boundsMin = {area.boundsMinX, area.boundsMinY, area.boundsMinZ};
            def.boundsMax = {area.boundsMaxX, area.boundsMaxY, area.boundsMaxZ};
            def.scenePath = "Assets/Scenes/" + area.name + ".scene";
            def.priority = area.isInstanced ? 0 : 1;

            streamingMgr.RegisterArea(def);
        }

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO World] Registered streaming areas with SeamlessAreaManager");
    }

    void MMOWorldSetup::ConfigureWorldServer()
    {
#ifdef ENABLE_NETWORKING
        auto& console = Spark::SimpleConsole::GetInstance();

        // Configure the WorldServer to coordinate all area servers
        Spark::Net::WorldServerConfig worldConfig{};
        worldConfig.worldName = "SparkMMO Demo World";
        worldConfig.port = 27020;
        worldConfig.interServerPort = 27021;
        worldConfig.maxTotalClients = 500;
        worldConfig.tickRate = 10.0f;
        worldConfig.enableLoadBalancing = true;
        worldConfig.loadBalanceInterval = 30.0f;

        m_worldServer = std::make_unique<Spark::Net::WorldServer>();
        if (m_worldServer->Start(worldConfig))
        {
            console.LogInfo("[MMO World] WorldServer started on port " + std::to_string(worldConfig.port));

            // Register each area with the WorldServer
            uint16_t basePort = 27030;
            for (const auto& area : m_areas)
            {
                Spark::Net::AreaServerConfig areaConfig{};
                areaConfig.areaId = area.areaId;
                areaConfig.areaName = area.name;
                areaConfig.scenePath = "Assets/Scenes/" + area.name + ".scene";
                areaConfig.port = basePort++;
                areaConfig.interServerPort = basePort++;
                areaConfig.tickRate = 60.0f;
                areaConfig.maxClients = area.maxPlayers;
                areaConfig.enableAI = !area.isPvPEnabled; // AI mobs in PvE areas
                areaConfig.enablePhysics = true;
                areaConfig.enableScripting = true;

                m_worldServer->RegisterAreaServer(areaConfig);
                console.LogInfo("[MMO World] Registered area: " + area.name + " (port " +
                                std::to_string(areaConfig.port) + ")");
            }
        }
        else
        {
            console.LogWarning("[MMO World] WorldServer start failed (networking may be disabled)");
        }
#else
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO World] Networking disabled — WorldServer skipped (define ENABLE_NETWORKING)");
#endif
    }

    void MMOWorldSetup::ConfigureOriginRebasing()
    {
        // Large MMO worlds need origin rebasing to prevent floating-point jitter
        // at extreme distances. Set a 5km threshold.
        auto& originSystem = m_originSystem;
        originSystem.SetRebasingThreshold(5000.0f);
        originSystem.SetEnabled(true);

        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Origin rebasing enabled (threshold: 5000m)");
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO World] Origin rebasing enabled (threshold: 5000m)");
    }

    void MMOWorldSetup::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        m_worldTime += deltaTime;

        // Update seamless area streaming based on player position
        auto& streamingMgr = Spark::Streaming::SeamlessAreaManager::GetInstance();
        streamingMgr.Update(deltaTime);
    }

#ifdef ENABLE_NETWORKING

    bool MMOWorldSetup::StartNetworkServer(uint16_t port)
    {
        // Networking is resolved through the injected engine context, not the global singleton
        auto* nm = m_context ? m_context->GetNetwork() : nullptr;
        if (!nm)
            return false;

        if (!nm->IsInitialized())
        {
            if (!nm->Initialize())
                return false;
        }

        if (!nm->StartServer(port, 128))
            return false;

        // Register server-side chat broadcast handler
        nm->RegisterHandler(Spark::Net::MessageType::ChatMessage,
                            [nm](const Spark::Net::NetworkMessage& msg)
                            {
                                if (nm->GetRole() != Spark::Net::NetworkRole::Server)
                                    return;

                                // Broadcast to all other connected clients
                                nm->SendToAllExcept(msg.senderID, msg);
                            });

        // Register server-side position relay handler
        nm->RegisterHandler(Spark::Net::MessageType::EntityStateUpdate,
                            [nm](const Spark::Net::NetworkMessage& msg)
                            {
                                if (nm->GetRole() != Spark::Net::NetworkRole::Server)
                                    return;

                                // Relay position update to all other clients
                                nm->SendToAllExcept(msg.senderID, msg);
                            });

        m_networkServerRunning = true;
        m_knownClients.clear();

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO World] Network server started on port " + std::to_string(port));
        return true;
    }

    void MMOWorldSetup::ServerTick(float deltaTime)
    {
        if (!m_networkServerRunning)
            return;

        // Networking is resolved through the injected engine context, not the global singleton
        auto* nm = m_context ? m_context->GetNetwork() : nullptr;
        if (!nm)
            return;

        nm->Update(deltaTime);

        // Detect new client connections and bridge to WorldServer
        if (m_worldServer && m_worldServer->IsRunning())
        {
            const auto& clients = nm->GetClients();

            // Detect new connections
            for (const auto& [clientId, info] : clients)
            {
                if (m_knownClients.find(clientId) == m_knownClients.end())
                {
                    m_knownClients[clientId] = true;
                    m_worldServer->HandlePlayerConnect(clientId, info.name, {0.0f, 0.0f, 0.0f});
                }
            }

            // Detect disconnections
            std::vector<Spark::Net::ClientID> disconnected;
            for (const auto& [clientId, _] : m_knownClients)
            {
                if (clients.find(clientId) == clients.end())
                {
                    disconnected.push_back(clientId);
                }
            }
            for (auto clientId : disconnected)
            {
                m_knownClients.erase(clientId);
                m_worldServer->HandlePlayerDisconnect(clientId);
            }
        }

        m_worldTime += deltaTime;
    }

    void MMOWorldSetup::StopNetworkServer()
    {
        if (!m_networkServerRunning)
            return;

        // Networking is resolved through the injected engine context, not the global singleton
        auto* nm = m_context ? m_context->GetNetwork() : nullptr;
        if (nm)
        {
            nm->StopServer();
            nm->Shutdown();
        }
        m_knownClients.clear();
        m_networkServerRunning = false;

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO World] Network server stopped");
    }

#endif // ENABLE_NETWORKING

    void MMOWorldSetup::Shutdown()
    {
        if (!m_initialized)
            return;

#ifdef ENABLE_NETWORKING
        if (m_worldServer)
        {
            m_worldServer->Stop();
            m_worldServer.reset();
        }
#endif

        m_areas.clear();
        m_initialized = false;
    }

    void MMOWorldSetup::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("MMO World"))
            return;

        ImGui::Text("World Time: %.1f s", m_worldTime);
        ImGui::Text("Areas: %zu", m_areas.size());
        ImGui::Separator();

        for (const auto& area : m_areas)
        {
            ImGui::PushID(static_cast<int>(area.areaId));
            if (ImGui::TreeNode(area.name.c_str()))
            {
                ImGui::Text("ID: %u", area.areaId);
                ImGui::Text("Max Players: %d", area.maxPlayers);
                ImGui::Text("PvP: %s", area.isPvPEnabled ? "Yes" : "No");
                ImGui::Text("Instanced: %s", area.isInstanced ? "Yes" : "No");
                ImGui::TextWrapped("Description: %s", area.description.c_str());
                ImGui::Text("Bounds: (%.0f,%.0f,%.0f) - (%.0f,%.0f,%.0f)", area.boundsMinX, area.boundsMinY,
                            area.boundsMinZ, area.boundsMaxX, area.boundsMaxY, area.boundsMaxZ);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
#endif
    }

    std::string MMOWorldSetup::GetWorldStatusString() const
    {
        std::string status;
        status += "World Time: " + std::to_string(m_worldTime) + "s\n";

        auto& originSystem = m_originSystem;
        auto stats = originSystem.GetStats();
        status += "Origin Rebases: " + std::to_string(stats.totalRebases) + "\n";
        status += "Max Distance: " + std::to_string(stats.maxDistanceFromOrigin) + "m\n";
        return status;
    }

    std::string MMOWorldSetup::GetAreaListString() const
    {
        std::string list = "=== MMO World Areas ===\n";
        for (const auto& area : m_areas)
        {
            list += "[" + std::to_string(area.areaId) + "] " + area.name;
            if (area.isPvPEnabled)
                list += " [PvP]";
            if (area.isInstanced)
                list += " [Instanced]";
            list += " (max " + std::to_string(area.maxPlayers) + " players)\n";
            list += "    " + area.description + "\n";
        }
        return list;
    }

} // namespace MMO
