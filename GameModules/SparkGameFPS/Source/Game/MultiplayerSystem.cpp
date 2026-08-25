/**
 * @file MultiplayerSystem.cpp
 * @brief FPS multiplayer system implementation
 */

#include "MultiplayerSystem.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace SparkFPS
{

    // ============================================================================
    // Singleton
    // ============================================================================

    FPSMultiplayerSystem& FPSMultiplayerSystem::GetInstance()
    {
        static FPSMultiplayerSystem instance;
        return instance;
    }

    // ============================================================================
    // Lifecycle
    // ============================================================================

    void FPSMultiplayerSystem::Initialize(bool isServer)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Network, "FPSMultiplayerSystem::Initialize — mode=%s",
                       isServer ? "Server" : "Client");
        m_isServer = isServer;
        m_isActive = false;
        m_playerStates.clear();
        m_scores.clear();
        m_respawnTimers.clear();
        m_projectiles.clear();
        m_remoteSnapshots.clear();
        m_lastInputByPlayer.clear();
        m_stateSequence = 0;
        m_nextProjectileId = 1;
        m_correctionCount = 0;
        m_tickAccumulator = 0.0f;
        m_clientPrediction.SetMaxPendingInputs(256);
        m_clientPrediction.SetSmoothCorrection(true, 10.0f);

        // Default spawn points if none configured
        if (m_spawnPoints.empty())
        {
            m_spawnPoints.push_back({0.0f, 1.0f, 0.0f, 0.0f});
            m_spawnPoints.push_back({10.0f, 1.0f, 0.0f, 90.0f});
            m_spawnPoints.push_back({-10.0f, 1.0f, 10.0f, 180.0f});
            m_spawnPoints.push_back({5.0f, 1.0f, -10.0f, 270.0f});
        }

        auto& console = Spark::SimpleConsole::GetInstance();
        console.Log("[FPSMultiplayer] Initialized (" + std::string(isServer ? "Server" : "Client") + " mode)");
    }

    void FPSMultiplayerSystem::Update(float deltaTime)
    {
        if (!m_isActive || !std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;

        auto& network = Spark::Net::NetworkManager::GetInstance();
        network.Update(deltaTime);

        if (!m_isServer)
        {
            const uint32_t assignedClientId = network.GetLocalClientID();
            if (assignedClientId != Spark::Net::INVALID_CLIENT && assignedClientId != m_localClientId)
            {
                m_localClientId = assignedClientId;
                OnPlayerJoined(m_localClientId);
            }
        }

        if (m_isServer)
            ServerUpdate(deltaTime);
        else
            ClientUpdate(deltaTime);
    }

    void FPSMultiplayerSystem::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Network, "FPSMultiplayerSystem::Shutdown — %zu players active",
                       m_playerStates.size());
        if (m_isServer)
            StopServer();
        else
            Disconnect();

        m_playerStates.clear();
        m_projectiles.clear();
        m_remoteSnapshots.clear();
        m_scores.clear();
        m_isActive = false;
    }

    // ============================================================================
    // Server API
    // ============================================================================

    bool FPSMultiplayerSystem::StartServer(uint16_t port, uint32_t maxPlayers)
    {
        if (maxPlayers == 0 || maxPlayers > 256)
            return false;

        auto& network = Spark::Net::NetworkManager::GetInstance();
        if (!network.Initialize() || !network.StartServer(port, static_cast<int>(maxPlayers)))
            return false;

        m_isActive = true;
        m_isServer = true;
        m_localClientId = network.GetLocalClientID();
        OnPlayerJoined(m_localClientId);

        auto& console = Spark::SimpleConsole::GetInstance();
        console.Log("[FPSMultiplayer] Server started on port " + std::to_string(port) + " (max " +
                    std::to_string(maxPlayers) + " players)");
        return true;
    }

    void FPSMultiplayerSystem::StopServer()
    {
        Spark::Net::NetworkManager::GetInstance().StopServer();
        m_isActive = false;
        m_playerStates.clear();
        m_scores.clear();
    }

    // ============================================================================
    // Client API
    // ============================================================================

    bool FPSMultiplayerSystem::Connect(const std::string& address, uint16_t port)
    {
        if (address.empty() || port == 0)
            return false;

        auto& network = Spark::Net::NetworkManager::GetInstance();
        if (!network.Initialize() || !network.Connect(address, port, "FPSPlayer"))
            return false;

        m_isActive = true;
        m_isServer = false;
        m_localClientId = Spark::Net::INVALID_CLIENT;

        auto& console = Spark::SimpleConsole::GetInstance();
        console.Log("[FPSMultiplayer] Connecting to " + address + ":" + std::to_string(port));
        return true;
    }

    void FPSMultiplayerSystem::Disconnect()
    {
        Spark::Net::NetworkManager::GetInstance().Disconnect();
        m_isActive = false;
        m_playerStates.clear();
        m_localClientId = Spark::Net::INVALID_CLIENT;
    }

    void FPSMultiplayerSystem::SendInput(const PlayerInput& input)
    {
        if (!m_isActive || m_isServer)
            return;

        m_lastInputByPlayer[m_localClientId] = input;

        Spark::PredictedInput predicted{};
        predicted.timestamp = static_cast<float>(input.sequenceNumber) * (1.0f / 60.0f);
        predicted.moveDirection = {input.strafe, 0.0f, input.forward};
        predicted.lookYaw = input.yaw;
        predicted.lookPitch = input.pitch;
        predicted.jump = input.jump;
        predicted.crouch = input.crouch;
        predicted.fire = input.fire;
        predicted.reload = input.reload;
        const uint32_t assignedSequence = m_clientPrediction.RecordInput(predicted);
        predicted.sequenceNumber = assignedSequence;

        m_clientPrediction.ApplyPrediction(m_localPredictedState, predicted, 1.0f / 60.0f);

        Spark::Net::ClientInputState networkInput{};
        networkInput.inputSequence = input.sequenceNumber;
        networkInput.moveForward = input.forward;
        networkInput.moveRight = input.strafe;
        networkInput.lookYaw = input.yaw;
        networkInput.lookPitch = input.pitch;
        networkInput.jump = input.jump;
        networkInput.fire = input.fire;
        networkInput.reload = input.reload;
        networkInput.crouch = input.crouch;
        networkInput.deltaTime = 1.0f / 60.0f;
        networkInput.timestamp = predicted.timestamp;
        Spark::Net::NetworkManager::GetInstance().SendClientInput(networkInput);

        if (input.fire)
        {
            ProjectileData projectile;
            projectile.projectileId = m_nextProjectileId++;
            projectile.ownerId = m_localClientId;
            projectile.originX = m_localPredictedState.position.x;
            projectile.originY = m_localPredictedState.position.y + 1.0f;
            projectile.originZ = m_localPredictedState.position.z;
            projectile.positionX = projectile.originX;
            projectile.positionY = projectile.originY;
            projectile.positionZ = projectile.originZ;
            projectile.dirX = std::cos(input.yaw);
            projectile.dirY = 0.0f;
            projectile.dirZ = std::sin(input.yaw);
            projectile.velocityX = projectile.dirX * projectile.speed;
            projectile.velocityY = projectile.dirY * projectile.speed;
            projectile.velocityZ = projectile.dirZ * projectile.speed;
            projectile.active = true;
            m_projectiles[projectile.projectileId] = projectile; // local fire prediction hook
        }
    }

    // ============================================================================
    // Shared API
    // ============================================================================

    std::vector<PlayerScore> FPSMultiplayerSystem::GetScoreboard() const
    {
        std::vector<PlayerScore> board;
        board.reserve(m_scores.size());
        for (const auto& [id, score] : m_scores)
            board.push_back(score);

        std::sort(board.begin(), board.end(),
                  [](const PlayerScore& a, const PlayerScore& b)
                  {
                      if (a.score != b.score)
                          return a.score > b.score;
                      if (a.kills != b.kills)
                          return a.kills > b.kills;
                      if (a.deaths != b.deaths)
                          return a.deaths < b.deaths;
                      return a.clientId < b.clientId;
                  });
        return board;
    }

    const NetworkPlayerState* FPSMultiplayerSystem::GetPlayerState(uint32_t clientId) const
    {
        auto it = m_playerStates.find(clientId);
        return it != m_playerStates.end() ? &it->second : nullptr;
    }

    const std::unordered_map<uint32_t, NetworkPlayerState>& FPSMultiplayerSystem::GetAllPlayerStates() const
    {
        return m_playerStates;
    }

    void FPSMultiplayerSystem::AddSpawnPoint(const SpawnPoint& point)
    {
        m_spawnPoints.push_back(point);
    }

    // ============================================================================
    // Server Logic
    // ============================================================================

    void FPSMultiplayerSystem::ServerUpdate(float dt)
    {
        // Process respawn timers
        for (auto it = m_respawnTimers.begin(); it != m_respawnTimers.end();)
        {
            it->second -= dt;
            if (it->second <= 0.0f)
            {
                RespawnPlayer(it->first);
                it = m_respawnTimers.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Send state snapshots at tick rate
        m_tickAccumulator += dt;
        UpdateProjectiles(dt);
        float tickInterval = 1.0f / static_cast<float>(m_tickRate);
        if (m_tickAccumulator >= tickInterval)
        {
            m_tickAccumulator -= tickInterval;
            SendStateSnapshot();
        }
    }

    void FPSMultiplayerSystem::SendStateSnapshot()
    {
        ++m_stateSequence;
        for (auto& [id, state] : m_playerStates)
        {
            state.sequenceNumber = m_stateSequence;
            auto inputIt = m_lastInputByPlayer.find(id);
            if (inputIt != m_lastInputByPlayer.end())
            {
                state.acknowledgedInputSequence = inputIt->second.sequenceNumber;
            }
            auto& history = m_remoteSnapshots[id];
            history.push_back(state);
            constexpr size_t kMaxSnapshots = 4;
            while (history.size() > kMaxSnapshots)
            {
                history.pop_front();
            }
        }
    }

    void FPSMultiplayerSystem::ApplyClientInput(uint32_t clientId, const PlayerInput& input, float dt)
    {
        auto it = m_playerStates.find(clientId);
        if (it == m_playerStates.end() || !it->second.isAlive)
            return;

        auto& state = it->second;

        // Apply movement
        float sinYaw = std::sin(state.yaw);
        float cosYaw = std::cos(state.yaw);

        state.posX += (input.forward * cosYaw + input.strafe * sinYaw) * m_moveSpeed * dt;
        state.posZ += (input.forward * sinYaw - input.strafe * cosYaw) * m_moveSpeed * dt;
        state.velX = (input.forward * cosYaw + input.strafe * sinYaw) * m_moveSpeed;
        state.velY = 0.0f;
        state.velZ = (input.forward * sinYaw - input.strafe * cosYaw) * m_moveSpeed;

        state.yaw = input.yaw;
        state.pitch = input.pitch;
        state.isCrouching = input.crouch;
        state.actionFlags = ActionNone;
        state.actionFlags |= input.jump ? ActionJump : ActionNone;
        state.actionFlags |= input.fire ? ActionFire : ActionNone;
        state.actionFlags |= input.reload ? ActionReload : ActionNone;
        state.actionFlags |= input.crouch ? ActionCrouch : ActionNone;
        m_lastInputByPlayer[clientId] = input;

        if (input.fire)
        {
            ProjectileData projectile;
            projectile.projectileId = m_nextProjectileId++;
            projectile.ownerId = clientId;
            projectile.originX = state.posX;
            projectile.originY = state.posY + 1.0f;
            projectile.originZ = state.posZ;
            projectile.positionX = projectile.originX;
            projectile.positionY = projectile.originY;
            projectile.positionZ = projectile.originZ;
            projectile.dirX = std::cos(state.yaw);
            projectile.dirY = 0.0f;
            projectile.dirZ = std::sin(state.yaw);
            projectile.velocityX = projectile.dirX * projectile.speed;
            projectile.velocityY = 0.0f;
            projectile.velocityZ = projectile.dirZ * projectile.speed;
            projectile.active = true;
            m_projectiles[projectile.projectileId] = projectile;
        }
    }

    void FPSMultiplayerSystem::ValidateHit(uint32_t attackerId, uint32_t victimId, float damage)
    {
        auto victimIt = m_playerStates.find(victimId);
        if (victimIt == m_playerStates.end() || !victimIt->second.isAlive)
            return;

        bool validated = true;
        if (m_isServer)
        {
            auto attackerIt = m_playerStates.find(attackerId);
            if (attackerIt != m_playerStates.end())
            {
                const auto rayOrigin =
                    DirectX::XMFLOAT3(attackerIt->second.posX, attackerIt->second.posY + 1.0f, attackerIt->second.posZ);
                const auto rayDir = DirectX::XMFLOAT3(victimIt->second.posX - attackerIt->second.posX,
                                                      victimIt->second.posY - attackerIt->second.posY,
                                                      victimIt->second.posZ - attackerIt->second.posZ);
                const float halfRTT = Spark::Net::NetworkManager::GetInstance().GetEstimatedRTT() * 0.0005f;
                const float now = Spark::Net::NetworkManager::GetInstance().GetServerTime();
                auto result = Spark::Net::NetworkManager::GetInstance().ValidateHit(now, halfRTT, rayOrigin, rayDir);
                validated = result.hit;
            }
        }
        if (!validated)
            return;

        victimIt->second.health -= damage;

        if (victimIt->second.health <= 0.0f)
        {
            victimIt->second.health = 0.0f;
            victimIt->second.isAlive = false;

            // Update scores
            m_scores[attackerId].kills++;
            m_scores[attackerId].score += 100;
            m_scores[victimId].deaths++;

            // Start respawn timer
            m_respawnTimers[victimId] = m_respawnTime;
        }
    }

    SpawnPoint FPSMultiplayerSystem::GetRandomSpawnPoint() const
    {
        if (m_spawnPoints.empty())
            return {0.0f, 1.0f, 0.0f, 0.0f};

        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, m_spawnPoints.size() - 1);
        return m_spawnPoints[dist(rng)];
    }

    void FPSMultiplayerSystem::RespawnPlayer(uint32_t clientId)
    {
        auto it = m_playerStates.find(clientId);
        if (it == m_playerStates.end())
            return;

        auto spawn = GetRandomSpawnPoint();
        it->second.posX = spawn.x;
        it->second.posY = spawn.y;
        it->second.posZ = spawn.z;
        it->second.yaw = spawn.yaw;
        it->second.health = 100.0f;
        it->second.isAlive = true;
    }

    // ============================================================================
    // Client Logic
    // ============================================================================

    void FPSMultiplayerSystem::ClientUpdate(float dt)
    {
        auto localIt = m_playerStates.find(m_localClientId);
        if (localIt != m_playerStates.end())
        {
            ReconcileToAuthoritativeState(localIt->second);
        }
        InterpolateRemotePlayers(dt);
        UpdateProjectiles(dt);
    }

    void FPSMultiplayerSystem::InterpolateRemotePlayers(float dt)
    {
        const float blend = (std::min)(1.0f, dt * 10.0f);
        for (auto& [playerId, snapshots] : m_remoteSnapshots)
        {
            if (playerId == m_localClientId || snapshots.empty())
                continue;

            auto target = snapshots.back();
            auto currentIt = m_playerStates.find(playerId);
            if (currentIt == m_playerStates.end())
            {
                m_playerStates[playerId] = target;
                continue;
            }

            auto& current = currentIt->second;
            current.posX += (target.posX - current.posX) * blend;
            current.posY += (target.posY - current.posY) * blend;
            current.posZ += (target.posZ - current.posZ) * blend;
            current.velX = target.velX;
            current.velY = target.velY;
            current.velZ = target.velZ;
            current.yaw = target.yaw;
            current.pitch = target.pitch;
            current.actionFlags = target.actionFlags;
        }
    }

    // ============================================================================
    // Message Handlers
    // ============================================================================

    void FPSMultiplayerSystem::OnPlayerJoined(uint32_t clientId)
    {
        auto spawn = GetRandomSpawnPoint();
        NetworkPlayerState state;
        state.clientId = clientId;
        state.posX = spawn.x;
        state.posY = spawn.y;
        state.posZ = spawn.z;
        state.yaw = spawn.yaw;
        state.health = 100.0f;
        state.isAlive = true;

        m_playerStates[clientId] = state;
        m_remoteSnapshots[clientId].push_back(state);

        PlayerScore score;
        score.clientId = clientId;
        score.playerName = "Player_" + std::to_string(clientId);
        m_scores[clientId] = score;

        auto& console = Spark::SimpleConsole::GetInstance();
        console.Log("[FPSMultiplayer] Player " + std::to_string(clientId) + " joined");
    }

    void FPSMultiplayerSystem::OnPlayerLeft(uint32_t clientId)
    {
        m_playerStates.erase(clientId);
        m_scores.erase(clientId);
        m_respawnTimers.erase(clientId);

        auto& console = Spark::SimpleConsole::GetInstance();
        console.Log("[FPSMultiplayer] Player " + std::to_string(clientId) + " left");
    }

    void FPSMultiplayerSystem::OnPlayerInputReceived(uint32_t clientId, const PlayerInput& input)
    {
        if (!m_isServer)
            return;
        ApplyClientInput(clientId, input, 1.0f / 60.0f);
    }

    void FPSMultiplayerSystem::OnProjectileFired(uint32_t clientId, const ProjectileData& proj)
    {
        if (!m_isServer)
            return;

        auto ownerIt = m_playerStates.find(clientId);
        if (ownerIt == m_playerStates.end() || !ownerIt->second.isAlive)
            return;

        ProjectileData serverProj = proj;
        serverProj.projectileId = (serverProj.projectileId == 0) ? m_nextProjectileId++ : serverProj.projectileId;
        serverProj.ownerId = clientId;
        serverProj.active = true;
        serverProj.positionX = serverProj.originX;
        serverProj.positionY = serverProj.originY;
        serverProj.positionZ = serverProj.originZ;
        serverProj.velocityX = serverProj.dirX * serverProj.speed;
        serverProj.velocityY = serverProj.dirY * serverProj.speed;
        serverProj.velocityZ = serverProj.dirZ * serverProj.speed;
        m_projectiles[serverProj.projectileId] = serverProj;
    }

    void FPSMultiplayerSystem::OnPlayerDamaged(uint32_t attackerId, uint32_t victimId, float damage)
    {
        if (!m_isServer)
            return;
        ValidateHit(attackerId, victimId, damage);
    }

    // ============================================================================
    // Console
    // ============================================================================

    std::string FPSMultiplayerSystem::Console_GetStatus() const
    {
        std::string status = "FPSMultiplayer: ";
        if (!m_isActive)
        {
            status += "Inactive";
            return status;
        }

        status += m_isServer ? "Server" : "Client";
        status += " | Players: " + std::to_string(m_playerStates.size());
        status += " | Projectiles: " + std::to_string(m_projectiles.size());
        status += " | Tick: " + std::to_string(m_tickRate) + "Hz";
        status += " | Seq: " + std::to_string(m_stateSequence);
        auto metrics = GetDebugMetrics();
        status += " | RTT: " + std::to_string(static_cast<int>(metrics.rttMs)) + "ms";
        status += " | Loss: " + std::to_string(static_cast<int>(metrics.packetLossPercent)) + "%";
        status += " | Corrections: " + std::to_string(metrics.correctionCount);
        return status;
    }

    FPSMultiplayerSystem::MultiplayerDebugMetrics FPSMultiplayerSystem::GetDebugMetrics() const
    {
        MultiplayerDebugMetrics out;
        const auto& stats = Spark::Net::NetworkManager::GetInstance().GetStats();
        out.packetLossPercent = stats.packetLoss * 100.0f;
        out.rttMs = stats.ping;
        out.correctionCount = m_correctionCount;
        return out;
    }

    void FPSMultiplayerSystem::UpdateProjectiles(float dt)
    {
        if (!(dt >= 0.0f && std::isfinite(dt)))
            return;
        for (auto it = m_projectiles.begin(); it != m_projectiles.end();)
        {
            auto& projectile = it->second;
            projectile.positionX += projectile.velocityX * dt;
            projectile.positionY += projectile.velocityY * dt;
            projectile.positionZ += projectile.velocityZ * dt;
            projectile.lifetime -= dt;

            bool despawned = (projectile.lifetime <= 0.0f);
            if (!despawned && m_isServer)
            {
                for (const auto& [playerId, state] : m_playerStates)
                {
                    if (playerId == projectile.ownerId || !state.isAlive)
                        continue;

                    const float dx = projectile.positionX - state.posX;
                    const float dy = projectile.positionY - state.posY;
                    const float dz = projectile.positionZ - state.posZ;
                    const float distSq = dx * dx + dy * dy + dz * dz;
                    if (distSq <= 1.0f)
                    {
                        ValidateHit(projectile.ownerId, playerId, projectile.damage);
                        despawned = true;
                        break;
                    }
                }
            }

            if (despawned)
                it = m_projectiles.erase(it);
            else
                ++it;
        }
    }

    void FPSMultiplayerSystem::ReconcileToAuthoritativeState(const NetworkPlayerState& authoritativeState)
    {
        Spark::PredictedState serverState;
        serverState.position = {authoritativeState.posX, authoritativeState.posY, authoritativeState.posZ};
        serverState.velocity = {authoritativeState.velX, authoritativeState.velY, authoritativeState.velZ};
        serverState.yaw = authoritativeState.yaw;
        serverState.pitch = authoritativeState.pitch;
        serverState.isCrouching = authoritativeState.isCrouching;
        serverState.lastProcessedInput = authoritativeState.acknowledgedInputSequence;

        const float before = m_clientPrediction.GetLastCorrectionMagnitude();
        m_clientPrediction.Reconcile(serverState, 1.0f / 60.0f);
        const float after = m_clientPrediction.GetLastCorrectionMagnitude();
        if (after > 0.01f && after != before)
        {
            ++m_correctionCount;
            Spark::Net::NetworkManager::GetInstance().SetPredictionCorrectionCount(m_correctionCount);
        }

        const auto& predicted = m_clientPrediction.GetState();
        auto& local = m_playerStates[m_localClientId];
        local.posX = predicted.position.x;
        local.posY = predicted.position.y;
        local.posZ = predicted.position.z;
        local.velX = predicted.velocity.x;
        local.velY = predicted.velocity.y;
        local.velZ = predicted.velocity.z;
        local.yaw = predicted.yaw;
        local.pitch = predicted.pitch;
        local.isCrouching = predicted.isCrouching;
    }

} // namespace SparkFPS
