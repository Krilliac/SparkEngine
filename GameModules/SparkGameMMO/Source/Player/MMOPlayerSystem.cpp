/**
 * @file MMOPlayerSystem.cpp
 * @brief MMO player spawning, replication, prediction, and area migration
 */

#include "MMOPlayerSystem.h"
#include "Utils/ContainerUtils.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Input/InputManager.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include "Engine/World/SpatialGrid.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <sstream>

namespace MMO
{

    bool MMOPlayerSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;
        if (m_initialized || m_context)
            Shutdown();

        m_context = context;

        SetupNetworkHandlers();

        // Spawn a default local player in the TownSquare (area 1)
        SpawnLocalPlayer("Player", 1);

        m_initialized = true;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "MMO player system initialized");
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO Player] Player system initialized");
        return true;
    }

    void MMOPlayerSystem::SetupNetworkHandlers()
    {
#ifdef ENABLE_NETWORKING
        // Resolve networking through the injected engine context, not the global
        // singleton — the module is handed its NetworkManager via Initialize(context).
        auto* netMgr = m_context ? m_context->GetNetwork() : nullptr;
        if (!netMgr)
            return;

        // EntitySpawn uses NetworkManager's canonical payload:
        // networkId, ownerId, entityType, position, rotation.
        netMgr->RegisterHandler(Spark::Net::MessageType::EntitySpawn,
                                [this](const Spark::Net::NetworkMessage& netMsg)
                                {
                                    if (netMsg.payload.size() < sizeof(uint32_t) * 2)
                                        return;

                                    Spark::Net::NetBuffer buf;
                                    buf.WriteBytes(netMsg.payload.data(), netMsg.payload.size());
                                    uint32_t networkId = buf.ReadUint32();
                                    uint32_t clientId = buf.ReadUint32();
                                    std::string entityType = buf.ReadString();
                                    const auto position = buf.ReadVector3();
                                    (void)buf.ReadVector3(); // Rotation is not shown in the MMO player summary.
                                    if (buf.HasError() || entityType != "MMOPlayer" || clientId == m_localClientId)
                                        return;

                                    if (!Spark::ContainerUtils::Contains(m_players, clientId))
                                    {
                                        MMOPlayer player{};
                                        player.clientId = clientId;
                                        player.networkId = networkId;
                                        player.name = "Player_" + std::to_string(clientId);
                                        player.currentAreaId = 0;
                                        player.posX = player.targetPosX = position.x;
                                        player.posY = player.targetPosY = position.y;
                                        player.posZ = player.targetPosZ = position.z;
                                        m_players[clientId] = player;

                                        auto& console = Spark::SimpleConsole::GetInstance();
                                        console.LogInfo("[MMO Player] Spawned remote player: " + player.name);
                                    }
                                });

        // EntityDestroy carries a network entity ID, not a client ID.
        netMgr->RegisterHandler(Spark::Net::MessageType::EntityDestroy,
                                [this](const Spark::Net::NetworkMessage& netMsg)
                                {
                                    if (netMsg.payload.size() < sizeof(uint32_t))
                                        return;

                                    Spark::Net::NetBuffer buf;
                                    buf.WriteBytes(netMsg.payload.data(), netMsg.payload.size());
                                    const uint32_t networkId = buf.ReadUint32();
                                    if (buf.HasError())
                                        return;
                                    if (auto* player = FindPlayerByNetworkId(networkId))
                                        RemovePlayer(player->clientId);
                                });

        // Do not replace NetworkManager's EntityStateUpdate handler here. It owns
        // canonical deserialization and delta acknowledgements; UpdateInterpolation
        // reads the resulting replicated entity snapshots instead.

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO Player] Network handlers registered");
#endif
    }

    uint32_t MMOPlayerSystem::SpawnLocalPlayer(const std::string& name, uint32_t areaId)
    {
        uint32_t networkId = m_nextNetworkId++;
        m_localClientId = 1; // Local client always gets ID 1 in single-player demo

#ifdef ENABLE_NETWORKING
        auto* netMgr = m_context ? m_context->GetNetwork() : nullptr;
        if (netMgr && netMgr->GetRole() == Spark::Net::NetworkRole::Client &&
            netMgr->GetLocalClientID() != Spark::Net::INVALID_CLIENT)
        {
            m_localClientId = netMgr->GetLocalClientID();
        }
#endif

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
        player.targetPosX = player.posX;
        player.targetPosY = player.posY;
        player.targetPosZ = player.posZ;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Local player spawned: %s in area %u", name.c_str(), areaId);
        m_players[m_localClientId] = player;

#ifdef ENABLE_NETWORKING
        // Register with NetworkManager for replication via the injected engine context
        if (netMgr)
        {
            Spark::Net::ReplicatedEntity repEntity;
            repEntity.networkID = 0;
            repEntity.ownerID = m_localClientId;
            repEntity.entityType = "MMOPlayer";
            repEntity.position = {player.posX, player.posY, player.posZ};
            repEntity.areaId = areaId;
            networkId = netMgr->RegisterReplicatedEntity(repEntity);
            m_players[m_localClientId].networkId = networkId;
        }
#endif

        m_localStateDirty = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO Player] Spawned local player '" + name + "' in area " + std::to_string(areaId));
        return networkId;
    }

    void MMOPlayerSystem::RemovePlayer(uint32_t clientId)
    {
        auto it = m_players.find(clientId);
        if (it != m_players.end())
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Player removed: %s (client %u)", it->second.name.c_str(),
                            clientId);
            auto& console = Spark::SimpleConsole::GetInstance();
            console.LogInfo("[MMO Player] Removed player: " + it->second.name);
            m_players.erase(it);
        }
    }

    MMOPlayer* MMOPlayerSystem::GetLocalPlayerMutable()
    {
        auto it = m_players.find(m_localClientId);
        return it != m_players.end() ? &it->second : nullptr;
    }

    const MMOPlayer* MMOPlayerSystem::GetLocalPlayer() const
    {
        auto it = m_players.find(m_localClientId);
        return it != m_players.end() ? &it->second : nullptr;
    }

    MMOPlayer* MMOPlayerSystem::FindPlayerByNetworkId(uint32_t networkId)
    {
        for (auto& [clientId, player] : m_players)
        {
            (void)clientId;
            if (player.networkId == networkId)
                return &player;
        }
        return nullptr;
    }

    void MMOPlayerSystem::IntegrateMovement(MMOPlayer& player, const MMOPlayerInput& input, float deltaTime)
    {
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
        {
            player.velocityX = player.velocityY = player.velocityZ = 0.0f;
            return;
        }

        const float safeDelta = std::clamp(deltaTime, 0.0f, 0.25f);
        float moveX = std::isfinite(input.moveX) ? std::clamp(input.moveX, -1.0f, 1.0f) : 0.0f;
        float moveZ = std::isfinite(input.moveZ) ? std::clamp(input.moveZ, -1.0f, 1.0f) : 0.0f;
        const float lengthSquared = moveX * moveX + moveZ * moveZ;
        if (lengthSquared > 1.0f)
        {
            const float inverseLength = 1.0f / std::sqrt(lengthSquared);
            moveX *= inverseLength;
            moveZ *= inverseLength;
        }

        const float speed = WALK_SPEED * (input.sprint ? SPRINT_MULTIPLIER : 1.0f);
        player.velocityX = moveX * speed;
        player.velocityY = 0.0f;
        player.velocityZ = moveZ * speed;
        player.posX += player.velocityX * safeDelta;
        player.posZ += player.velocityZ * safeDelta;
        player.targetPosX = player.posX;
        player.targetPosY = player.posY;
        player.targetPosZ = player.posZ;
    }

    bool MMOPlayerSystem::ApplyLocalInput(const MMOPlayerInput& input, float deltaTime)
    {
        auto* local = GetLocalPlayerMutable();
        if (!local || local->health <= 0.0f || deltaTime <= 0.0f)
            return false;

        const float previousX = local->posX;
        const float previousZ = local->posZ;
        IntegrateMovement(*local, input, deltaTime);
        const bool moved = local->posX != previousX || local->posZ != previousZ;
        if (moved)
            MarkLocalStateDirty();
        return moved;
    }

    void MMOPlayerSystem::ProcessInput(float deltaTime)
    {
        auto* input = m_context ? m_context->GetInput() : nullptr;
        if (!input)
            return;

        MMOPlayerInput movement;
        movement.moveX = (input->IsKeyDown('D') ? 1.0f : 0.0f) - (input->IsKeyDown('A') ? 1.0f : 0.0f);
        movement.moveZ = (input->IsKeyDown('W') ? 1.0f : 0.0f) - (input->IsKeyDown('S') ? 1.0f : 0.0f);
        movement.sprint = input->IsKeyDown(0x10); // Cross-platform virtual-key value for Shift.
        ApplyLocalInput(movement, deltaTime);
    }

    bool MMOPlayerSystem::TeleportLocalPlayer(uint32_t areaId, float x, float y, float z)
    {
        auto* local = GetLocalPlayerMutable();
        if (!local || areaId == 0 || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            return false;

        const uint32_t oldAreaId = local->currentAreaId;
        local->currentAreaId = areaId;
        local->posX = local->targetPosX = x;
        local->posY = local->targetPosY = y;
        local->posZ = local->targetPosZ = z;
        local->velocityX = local->velocityY = local->velocityZ = 0.0f;
        MarkLocalStateDirty();
        if (oldAreaId != areaId && m_areaTransitionCallback)
            m_areaTransitionCallback(oldAreaId, areaId);
        return true;
    }

    bool MMOPlayerSystem::ConfigureLocalPlayer(const std::string& name, int level, float maxHealth, uint32_t areaId,
                                               float x, float y, float z)
    {
        auto* local = GetLocalPlayerMutable();
        if (!local || name.empty() || level < 1 || !std::isfinite(maxHealth) || maxHealth <= 0.0f)
            return false;

        local->name = name;
        local->level = level;
        local->maxHealth = maxHealth;
        local->health = maxHealth;
        return TeleportLocalPlayer(areaId, x, y, z);
    }

    bool MMOPlayerSystem::DamageLocalPlayer(float amount)
    {
        auto* local = GetLocalPlayerMutable();
        if (!local || !std::isfinite(amount) || amount <= 0.0f || local->health <= 0.0f)
            return false;
        local->health = std::max(0.0f, local->health - amount);
        MarkLocalStateDirty();
        return true;
    }

    bool MMOPlayerSystem::HealLocalPlayer(float amount)
    {
        auto* local = GetLocalPlayerMutable();
        if (!local || !std::isfinite(amount) || amount <= 0.0f || local->health <= 0.0f ||
            local->health >= local->maxHealth)
            return false;
        local->health = std::min(local->maxHealth, local->health + amount);
        MarkLocalStateDirty();
        return true;
    }

    bool MMOPlayerSystem::RespawnLocalPlayer(uint32_t areaId, float x, float y, float z)
    {
        auto* local = GetLocalPlayerMutable();
        if (!local || local->health > 0.0f)
            return false;
        local->health = local->maxHealth;
        return TeleportLocalPlayer(areaId, x, y, z);
    }

    void MMOPlayerSystem::MarkLocalStateDirty()
    {
        m_localStateDirty = true;
    }

    void MMOPlayerSystem::SyncLocalPlayerState()
    {
#ifdef ENABLE_NETWORKING
        auto* local = GetLocalPlayerMutable();
        auto* netMgr = m_context ? m_context->GetNetwork() : nullptr;
        if (!local || !netMgr)
            return;

        Spark::Net::ReplicatedEntityUpdate update;
        update.position = DirectX::XMFLOAT3{local->posX, local->posY, local->posZ};
        update.velocity = DirectX::XMFLOAT3{local->velocityX, local->velocityY, local->velocityZ};
        update.areaId = local->currentAreaId;
        update.needsFullSync = true;
        (void)netMgr->UpdateReplicatedEntity(local->networkId, update);

        if (netMgr->GetRole() == Spark::Net::NetworkRole::Client &&
            netMgr->GetConnectionState() == Spark::Net::ConnectionState::Connected)
        {
            Spark::Net::NetBuffer payload;
            payload.WriteUint32(local->networkId);
            payload.WriteVector3({local->posX, local->posY, local->posZ});
            payload.WriteVector3({0.0f, 0.0f, 0.0f});
            payload.WriteVector3({local->velocityX, local->velocityY, local->velocityZ});
            payload.WriteUint16(0); // No custom replicated properties.

            Spark::Net::NetworkMessage message;
            message.type = Spark::Net::MessageType::EntityStateUpdate;
            message.channel = Spark::Net::ChannelType::Unreliable;
            message.payload = payload.GetData();
            netMgr->SendMessage(message);
        }
#endif
    }

    void MMOPlayerSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        ProcessInput(deltaTime);
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
        m_networkSendTimer += std::max(fixedDeltaTime, 0.0f);
        if (m_localStateDirty && m_networkSendTimer >= NETWORK_SEND_INTERVAL)
        {
            SyncLocalPlayerState();
            m_networkSendTimer = 0.0f;
            m_localStateDirty = false;
        }
#else
        (void)fixedDeltaTime;
#endif
    }

    void MMOPlayerSystem::UpdateInterpolation(float deltaTime)
    {
#ifdef ENABLE_NETWORKING
        auto* netMgr = m_context ? m_context->GetNetwork() : nullptr;
        const float safeDelta = std::clamp(deltaTime, 0.0f, 0.25f);
        const float alpha = 1.0f - std::exp(-REMOTE_INTERPOLATION_RATE * safeDelta);

        // Pull canonical snapshots deserialized by NetworkManager, then smooth
        // the visible MMO player state toward them.
        for (auto& [clientId, player] : m_players)
        {
            if (player.isLocalPlayer)
                continue;

            if (netMgr)
            {
                if (const auto replicated = netMgr->GetReplicatedEntitySnapshot(player.networkId))
                {
                    player.targetPosX = replicated->position.x;
                    player.targetPosY = replicated->position.y;
                    player.targetPosZ = replicated->position.z;
                    player.currentAreaId = replicated->areaId;
                }
            }

            player.posX += (player.targetPosX - player.posX) * alpha;
            player.posY += (player.targetPosY - player.posY) * alpha;
            player.posZ += (player.targetPosZ - player.posZ) * alpha;
        }
#else
        (void)deltaTime;
#endif
    }

    void MMOPlayerSystem::CheckAreaBoundaries()
    {
        auto it = m_players.find(m_localClientId);
        if (it == m_players.end())
            return;

        auto& local = it->second;
        if (!m_areaResolver)
            return;

        const uint32_t newAreaId = m_areaResolver(local.posX, local.posY, local.posZ, local.currentAreaId);
        if (newAreaId == 0 || newAreaId == local.currentAreaId)
            return;

        const uint32_t oldAreaId = local.currentAreaId;
        local.currentAreaId = newAreaId;
        MarkLocalStateDirty();

        Spark::SimpleConsole::GetInstance().LogInfo("[MMO Player] Area transition " + std::to_string(oldAreaId) +
                                                    " -> " + std::to_string(newAreaId));
        if (m_areaTransitionCallback)
            m_areaTransitionCallback(oldAreaId, newAreaId);
    }

    void MMOPlayerSystem::Render()
    {
        if (!m_initialized)
            return;

        // World meshes are rendered by the host scene/RHI. The module-owned
        // player presentation (status, health, nameplates) is drawn in OnImGui.
    }

    void MMOPlayerSystem::Shutdown()
    {
#ifdef ENABLE_NETWORKING
        if (auto* netMgr = m_context ? m_context->GetNetwork() : nullptr)
        {
            if (const auto* local = GetLocalPlayer(); local && local->networkId != 0)
                netMgr->UnregisterReplicatedEntity(local->networkId);

            // NetworkManager has no per-handler unregister API. Replace module-owned
            // handlers so no callback retains this DLL object after hot unload.
            netMgr->RegisterHandler(Spark::Net::MessageType::EntitySpawn, [](const Spark::Net::NetworkMessage&) {});
            netMgr->RegisterHandler(Spark::Net::MessageType::EntityDestroy, [](const Spark::Net::NetworkMessage&) {});
        }
#endif
        m_players.clear();
        m_context = nullptr;
        m_localClientId = 0;
        m_nextNetworkId = 1;
        m_networkSendTimer = 0.0f;
        m_localStateDirty = false;
        m_areaResolver = {};
        m_areaTransitionCallback = {};
        m_initialized = false;
    }

    void MMOPlayerSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("MMO Players"))
            return;

        ImGui::Text("Total Players: %zu", m_players.size());
        ImGui::Text("Local Client ID: %u", m_localClientId);
        ImGui::TextDisabled("Controls: WASD move, Shift sprint");
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
                ImGui::Text("Velocity: (%.1f, %.1f, %.1f)", player.velocityX, player.velocityY, player.velocityZ);
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

    std::string MMOPlayerSystem::GetLocalPlayerStatusString() const
    {
        const auto* player = GetLocalPlayer();
        if (!player)
            return "Local player unavailable";

        std::ostringstream status;
        status << player->name << " - Lv" << player->level << "\n";
        status << "Area: " << player->currentAreaId << "\n";
        status << "Position: (" << player->posX << ", " << player->posY << ", " << player->posZ << ")\n";
        status << "Health: " << player->health << "/" << player->maxHealth << "\n";
        status << "State: " << (player->health > 0.0f ? "Alive" : "Defeated") << "\n";
        return status.str();
    }

} // namespace MMO
