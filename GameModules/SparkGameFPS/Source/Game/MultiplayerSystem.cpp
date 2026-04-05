/**
 * @file MultiplayerSystem.cpp
 * @brief FPS multiplayer system implementation
 */

#include "MultiplayerSystem.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cstring>
#include <random>

namespace SparkFPS
{

    // ============================================================================
    // Serialization
    // ============================================================================

    std::vector<uint8_t> NetworkPlayerState::Serialize() const
    {
        std::vector<uint8_t> buf(sizeof(NetworkPlayerState));
        std::memcpy(buf.data(), this, sizeof(NetworkPlayerState));
        return buf;
    }

    NetworkPlayerState NetworkPlayerState::Deserialize(const uint8_t* data, size_t size)
    {
        NetworkPlayerState s;
        if (data && size >= sizeof(NetworkPlayerState))
            std::memcpy(&s, data, sizeof(NetworkPlayerState));
        return s;
    }

    std::vector<uint8_t> PlayerInput::Serialize() const
    {
        std::vector<uint8_t> buf(sizeof(PlayerInput));
        std::memcpy(buf.data(), this, sizeof(PlayerInput));
        return buf;
    }

    PlayerInput PlayerInput::Deserialize(const uint8_t* data, size_t size)
    {
        PlayerInput inp;
        if (data && size >= sizeof(PlayerInput))
            std::memcpy(&inp, data, sizeof(PlayerInput));
        return inp;
    }

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
        m_stateSequence = 0;
        m_tickAccumulator = 0.0f;

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
        if (!m_isActive)
            return;

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
        m_scores.clear();
        m_isActive = false;
    }

    // ============================================================================
    // Server API
    // ============================================================================

    bool FPSMultiplayerSystem::StartServer(uint16_t port, uint32_t maxPlayers)
    {
        (void)port;
        (void)maxPlayers;
        m_isActive = true;
        m_isServer = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        console.Log("[FPSMultiplayer] Server started on port " + std::to_string(port) + " (max " +
                    std::to_string(maxPlayers) + " players)");
        return true;
    }

    void FPSMultiplayerSystem::StopServer()
    {
        m_isActive = false;
        m_playerStates.clear();
        m_scores.clear();
    }

    // ============================================================================
    // Client API
    // ============================================================================

    bool FPSMultiplayerSystem::Connect(const std::string& address, uint16_t port)
    {
        (void)address;
        (void)port;
        m_isActive = true;
        m_isServer = false;

        auto& console = Spark::SimpleConsole::GetInstance();
        console.Log("[FPSMultiplayer] Connecting to " + address + ":" + std::to_string(port));
        return true;
    }

    void FPSMultiplayerSystem::Disconnect()
    {
        m_isActive = false;
        m_playerStates.clear();
    }

    void FPSMultiplayerSystem::SendInput(const PlayerInput& input)
    {
        if (!m_isActive || m_isServer)
            return;

        // In a real implementation, this would serialize and send via NetworkManager
        (void)input;
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
                  [](const PlayerScore& a, const PlayerScore& b) { return a.score > b.score; });
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
        }
        // In real impl: serialize all player states, send unreliable to all clients
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

        state.yaw = input.yaw;
        state.pitch = input.pitch;
        state.isCrouching = input.crouch;
    }

    void FPSMultiplayerSystem::ValidateHit(uint32_t attackerId, uint32_t victimId, float damage)
    {
        auto victimIt = m_playerStates.find(victimId);
        if (victimIt == m_playerStates.end() || !victimIt->second.isAlive)
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
        InterpolateRemotePlayers(dt);
    }

    void FPSMultiplayerSystem::InterpolateRemotePlayers(float dt)
    {
        // In real impl: interpolate between last two server snapshots
        // using a 100ms interpolation buffer for smooth movement
        (void)dt;
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
        (void)clientId;
        (void)proj;
        // In real impl: validate, create server-side projectile, broadcast to all clients
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
        status += " | Tick: " + std::to_string(m_tickRate) + "Hz";
        status += " | Seq: " + std::to_string(m_stateSequence);
        return status;
    }

} // namespace SparkFPS
