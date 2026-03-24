/**
 * @file MMOPlayerSystem.cpp
 * @brief MMO player spawning, replication, prediction, and area migration
 */

#include "MMOPlayerSystem.h"
#include "Utils/ContainerUtils.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include "Engine/World/SpatialGrid.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace MMO
{

    bool MMOPlayerSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        SetupNetworkHandlers();

        // Spawn a default local player in the TownSquare (area 1)
        SpawnLocalPlayer("Player", 1);

        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO Player] Player system initialized");
        return true;
    }

    void MMOPlayerSystem::SetupNetworkHandlers()
    {
#ifdef ENABLE_NETWORKING
        auto& netMgr = Spark::Net::NetworkManager::GetInstance();

        // Handle player spawn messages from server
        netMgr.RegisterHandler(Spark::Net::MessageType::EntitySpawn,
                               [this](const Spark::Net::NetworkMessage& netMsg)
                               {
                                   if (netMsg.payload.size() < sizeof(uint32_t) * 3)
                                       return;

                                   // Parse: networkId, clientId, areaId
                                   Spark::Net::NetBuffer buf;
                                   buf.WriteBytes(netMsg.payload.data(), netMsg.payload.size());
                                   uint32_t networkId = buf.ReadUint32();
                                   uint32_t clientId = buf.ReadUint32();
                                   uint32_t areaId = buf.ReadUint32();

                                   if (!Spark::ContainerUtils::Contains(m_players, clientId))
                                   {
                                       MMOPlayer player{};
                                       player.clientId = clientId;
                                       player.networkId = networkId;
                                       player.name = "Player_" + std::to_string(clientId);
                                       player.currentAreaId = areaId;
                                       player.isLocalPlayer = (clientId == m_localClientId);
                                       m_players[clientId] = player;

                                       auto& console = Spark::SimpleConsole::GetInstance();
                                       console.LogInfo("[MMO Player] Spawned remote player: " + player.name);
                                   }
                               });

        // Handle player disconnect
        netMgr.RegisterHandler(Spark::Net::MessageType::EntityDestroy,
                               [this](const Spark::Net::NetworkMessage& netMsg)
                               {
                                   if (netMsg.payload.size() < sizeof(uint32_t))
                                       return;

                                   Spark::Net::NetBuffer buf;
                                   buf.WriteBytes(netMsg.payload.data(), netMsg.payload.size());
                                   uint32_t clientId = buf.ReadUint32();
                                   RemovePlayer(clientId);
                               });

        // Handle position updates from server (for remote players)
        netMgr.RegisterHandler(Spark::Net::MessageType::EntityStateUpdate,
                               [this](const Spark::Net::NetworkMessage& netMsg)
                               {
                                   if (netMsg.payload.size() < sizeof(uint32_t) + sizeof(float) * 3)
                                       return;

                                   Spark::Net::NetBuffer buf;
                                   buf.WriteBytes(netMsg.payload.data(), netMsg.payload.size());
                                   uint32_t clientId = buf.ReadUint32();
                                   float x = buf.ReadFloat();
                                   float y = buf.ReadFloat();
                                   float z = buf.ReadFloat();

                                   auto it = m_players.find(clientId);
                                   if (it != m_players.end() && !it->second.isLocalPlayer)
                                   {
                                       it->second.posX = x;
                                       it->second.posY = y;
                                       it->second.posZ = z;
                                   }
                               });

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO Player] Network handlers registered");
#endif
    }

    uint32_t MMOPlayerSystem::SpawnLocalPlayer(const std::string& name, uint32_t areaId)
    {
        uint32_t networkId = m_nextNetworkId++;
        m_localClientId = 1; // Local client always gets ID 1 in single-player demo

        MMOPlayer player{};
        player.clientId = m_localClientId;
        player.networkId = networkId;
        player.name = name;
        player.currentAreaId = areaId;
        player.isLocalPlayer = true;

        // Spawn at area-appropriate position
        switch (areaId)
        {
        case 1: // TownSquare center
            player.posX = 0.0f;
            player.posY = 1.0f;
            player.posZ = 0.0f;
            break;
        case 2: // Wilderness entrance
            player.posX = 550.0f;
            player.posY = 1.0f;
            player.posZ = 0.0f;
            break;
        default:
            player.posX = 0.0f;
            player.posY = 1.0f;
            player.posZ = 0.0f;
            break;
        }

        m_players[m_localClientId] = player;

#ifdef ENABLE_NETWORKING
        // Register with NetworkManager for replication
        auto& netMgr = Spark::Net::NetworkManager::GetInstance();
        Spark::Net::ReplicatedEntity repEntity;
        repEntity.networkID = networkId;
        repEntity.ownerID = m_localClientId;
        repEntity.entityType = "MMOPlayer";
        repEntity.position = {player.posX, player.posY, player.posZ};
        netMgr.RegisterReplicatedEntity(repEntity);
#endif

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO Player] Spawned local player '" + name + "' in area " + std::to_string(areaId));
        return networkId;
    }

    void MMOPlayerSystem::RemovePlayer(uint32_t clientId)
    {
        auto it = m_players.find(clientId);
        if (it != m_players.end())
        {
            auto& console = Spark::SimpleConsole::GetInstance();
            console.LogInfo("[MMO Player] Removed player: " + it->second.name);
            m_players.erase(it);
        }
    }

    void MMOPlayerSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        UpdateInterpolation(deltaTime);
        CheckAreaBoundaries();
    }

    void MMOPlayerSystem::FixedUpdate(float fixedDeltaTime)
    {
        if (!m_initialized)
            return;

        UpdatePrediction(fixedDeltaTime);
    }

    void MMOPlayerSystem::UpdatePrediction(float fixedDeltaTime)
    {
#ifdef ENABLE_NETWORKING
        // Apply client-side prediction for the local player
        auto it = m_players.find(m_localClientId);
        if (it == m_players.end())
            return;

        auto& local = it->second;

        // Client-side prediction: in a full implementation, a Spark::ClientPrediction
        // instance would be maintained per-player and fed inputs each frame.
        // For now, the local player position is driven by direct input in Update().
        (void)local;
#else
        (void)fixedDeltaTime;
#endif
    }

    void MMOPlayerSystem::UpdateInterpolation(float deltaTime)
    {
#ifdef ENABLE_NETWORKING
        // Interpolate remote player positions for smooth rendering
        for (auto& [clientId, player] : m_players)
        {
            if (player.isLocalPlayer)
                continue;

            // NetworkInterpolation provides smooth position between server snapshots
            // In a full implementation, each remote player would have its own
            // interpolation buffer fed by EntityStateUpdate messages
        }
#endif
        (void)deltaTime;
    }

    void MMOPlayerSystem::CheckAreaBoundaries()
    {
        auto it = m_players.find(m_localClientId);
        if (it == m_players.end())
            return;

        auto& local = it->second;

        // Check if the local player has crossed into a different area's bounds
        // This would trigger entity migration via WorldServer in a networked game
        // For the demo, we just log the transition
        (void)local;
    }

    void MMOPlayerSystem::Render()
    {
        if (!m_initialized)
            return;

        // In a full implementation, this would render player models,
        // nameplates, health bars, etc. via the GraphicsEngine
    }

    void MMOPlayerSystem::Shutdown()
    {
        m_players.clear();
        m_initialized = false;
    }

    void MMOPlayerSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("MMO Players"))
            return;

        ImGui::Text("Total Players: %zu", m_players.size());
        ImGui::Text("Local Client ID: %u", m_localClientId);
        ImGui::Separator();

        for (const auto& [clientId, player] : m_players)
        {
            ImGui::PushID(static_cast<int>(clientId));
            std::string label = player.name;
            if (player.isLocalPlayer)
                label += " [LOCAL]";

            if (ImGui::TreeNode(label.c_str()))
            {
                ImGui::Text("Client ID: %u", player.clientId);
                ImGui::Text("Network ID: %u", player.networkId);
                ImGui::Text("Area: %u", player.currentAreaId);
                ImGui::Text("Position: (%.1f, %.1f, %.1f)", player.posX, player.posY, player.posZ);
                ImGui::Text("Health: %.0f / %.0f", player.health, player.maxHealth);
                ImGui::Text("Level: %d", player.level);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
#endif
    }

    std::string MMOPlayerSystem::GetPlayerListString() const
    {
        std::string list = "=== MMO Players ===\n";
        for (const auto& [clientId, player] : m_players)
        {
            list += "[" + std::to_string(clientId) + "] " + player.name;
            list += " Lv." + std::to_string(player.level);
            list += " Area:" + std::to_string(player.currentAreaId);
            list += " HP:" + std::to_string(static_cast<int>(player.health));
            if (player.isLocalPlayer)
                list += " *LOCAL*";
            list += "\n";
        }
        return list;
    }

} // namespace MMO
