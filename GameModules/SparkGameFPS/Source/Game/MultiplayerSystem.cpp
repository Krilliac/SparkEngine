/**
 * @file MultiplayerSystem.cpp
 * @brief FPSMultiplayerSystem lifecycle, sessions, and the server's authoritative player simulation
 *
 * The class is split by job: client prediction and interpolation live in MultiplayerClient.cpp,
 * hit validation and projectiles in MultiplayerCombat.cpp, and the NetworkManager message flow and
 * network diagnostics in MultiplayerNetFlow.cpp.
 */

#include "MultiplayerSystem.h"
#include "Core/FPSLog.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

namespace SparkFPS
{

    namespace
    {
        // Server-side input rate budget: a player earns simulated time as the server clock
        // advances and spends one step per applied input, so flooding inputs cannot move a
        // player faster than real time. The cap absorbs network jitter bursts.
        constexpr float kMaxInputBudget = 0.25f;

        // Server-owned fire interval: 600 rounds per minute, the default WeaponStats rate. Held
        // fire in 60 Hz inputs spawns one projectile every 6 steps, whatever the client sends.
        // With ProjectileData's fixed 3 s lifetime this also bounds each owner to 30 live projectiles.
        constexpr float kServerFireInterval = 0.1f;

        // Planar movement shared by the server's authoritative step and the client's
        // prediction, so a client in sync with the server reconciles with no correction.
        // The move is rotated by the yaw the player held before this input.
        void StepPlanarMovement(float yaw, float forward, float strafe, float speed, float dt, float& posX, float& posZ,
                                float& velX, float& velZ)
        {
            const float sinYaw = std::sin(yaw);
            const float cosYaw = std::cos(yaw);
            velX = (forward * cosYaw + strafe * sinYaw) * speed;
            velZ = (forward * sinYaw - strafe * cosYaw) * speed;
            posX += velX * dt;
            posZ += velZ * dt;
        }
    } // namespace

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
        FPS_LOG_INFO("FPSMultiplayerSystem::Initialize — mode={}", isServer ? "Server" : "Client");
        m_isServer = isServer;
        m_isActive = false;
        m_playerStates.clear();
        m_scores.clear();
        m_respawnTimers.clear();
        m_projectiles.clear();
        m_remoteSnapshots.clear();
        m_lastInputByPlayer.clear();
        m_fireCooldown.clear();
        m_stateSequence = 0;
        m_nextProjectileId = 1;
        m_correctionCount = 0;
        m_tickAccumulator = 0.0f;
        m_localPredictedState = {};
        m_pendingLocalAuthority = {};
        m_hasPendingLocalAuthority = false;
        m_lastLocalAuthoritySequence = 0;
        m_lastSnapshotBatch = 0;
        m_inputBudget.clear();
        // A new session starts a new input sequence; a stale ACK from an earlier session
        // must not make the first snapshots of this one look old. The default-constructed
        // predictor's simulator captures the temporary, so it is replaced right below.
        m_clientPrediction = Spark::ClientPrediction{};
        m_clientPrediction.SetMaxPendingInputs(256);
        m_clientPrediction.SetSmoothCorrection(true, 10.0f);
        m_clientPrediction.SetMovementSimulator(
            [this](Spark::PredictedState& state, const Spark::PredictedInput& input, float dt)
            {
                StepPlanarMovement(state.yaw, input.moveDirection.z, input.moveDirection.x, m_moveSpeed, dt,
                                   state.position.x, state.position.z, state.velocity.x, state.velocity.z);
                state.velocity.y = 0.0f;
                state.yaw = input.lookYaw;
                state.pitch = input.lookPitch;
                state.isCrouching = input.crouch;
            });

        // Default spawn points if none configured
        if (m_spawnPoints.empty())
        {
            m_spawnPoints.push_back({0.0f, 1.0f, 0.0f, 0.0f});
            m_spawnPoints.push_back({10.0f, 1.0f, 0.0f, 90.0f});
            m_spawnPoints.push_back({-10.0f, 1.0f, 10.0f, 180.0f});
            m_spawnPoints.push_back({5.0f, 1.0f, -10.0f, 270.0f});
        }

        FPS_CONSOLE("[FPSMultiplayer] Initialized (" + std::string(isServer ? "Server" : "Client") + " mode)", "INFO");
    }

    void FPSMultiplayerSystem::Update(float deltaTime)
    {
        if (!m_isActive || !std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;

        auto& network = Spark::Net::NetworkManager::GetInstance();
        network.Update(deltaTime);

        // A handler may have ended the session (the server closed it).
        if (!m_isActive)
            return;

        if (!m_isServer)
        {
            // A rejected or timed-out handshake, or a lost session, leaves NetworkManager
            // Disconnected without a Disconnect message. The FPS session ends with its
            // transport (this module does not enable NetworkManager auto-reconnect).
            if (network.GetConnectionState() == Spark::Net::ConnectionState::Disconnected)
            {
                FPS_CONSOLE("[FPSMultiplayer] Connection failed: " + network.GetLastConnectionError(), "ERROR");
                Disconnect();
                return;
            }

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
        FPS_LOG_INFO("FPSMultiplayerSystem::Shutdown — {} players active", m_playerStates.size());
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
        // The host is a player too, so the session never exceeds kMaxPlayers.
        if (maxPlayers == 0 || maxPlayers >= kMaxPlayers)
            return false;

        auto& network = Spark::Net::NetworkManager::GetInstance();
        if (!network.Initialize() || !network.UseDefaultSecurityConfig(Spark::Net::NetworkRole::Server) ||
            !network.StartServer(port, static_cast<int>(maxPlayers)))
            return false;

        RegisterNetworkHandlers();
        m_isActive = true;
        m_isServer = true;
        m_localClientId = network.GetLocalClientID();
        OnPlayerJoined(m_localClientId);

        FPS_CONSOLE("[FPSMultiplayer] Server started on port " + std::to_string(port) + " (max " +
                        std::to_string(maxPlayers) + " players)",
                    "INFO");
        return true;
    }

    void FPSMultiplayerSystem::StopServer()
    {
        auto& network = Spark::Net::NetworkManager::GetInstance();
        // The NetworkManager outlives this module's image: release every callback into it here.
        UnregisterNetworkHandlers();
        network.StopServer();
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
        if (!network.Initialize() || !network.UseDefaultSecurityConfig(Spark::Net::NetworkRole::Client) ||
            !network.Connect(address, port, "FPSPlayer"))
            return false;

        RegisterNetworkHandlers();
        m_isActive = true;
        m_isServer = false;
        m_localClientId = Spark::Net::INVALID_CLIENT;
        m_lastSnapshotBatch = 0;

        FPS_CONSOLE("[FPSMultiplayer] Connecting to " + address + ":" + std::to_string(port), "INFO");
        return true;
    }

    bool FPSMultiplayerSystem::IsConnected() const
    {
        if (!m_isActive)
            return false;
        if (m_isServer)
            return true;
        return m_localClientId != Spark::Net::INVALID_CLIENT &&
               Spark::Net::NetworkManager::GetInstance().GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void FPSMultiplayerSystem::Disconnect()
    {
        auto& network = Spark::Net::NetworkManager::GetInstance();
        // See StopServer: no callback may outlive the session. NetworkManager invokes
        // copies, so clearing them from inside one of those callbacks is safe.
        UnregisterNetworkHandlers();
        network.Disconnect();
        m_isActive = false;
        m_playerStates.clear();
        m_localClientId = Spark::Net::INVALID_CLIENT;
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
        for (auto& [id, budget] : m_inputBudget)
            budget = (std::min)(budget + dt, kMaxInputBudget);

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

        // Hits resolved this frame rewind against the world state the server holds now.
        RecordLagCompensationHistory();

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

    bool FPSMultiplayerSystem::ApplyClientInput(uint32_t clientId, const PlayerInput& rawInput, float dt)
    {
        auto it = m_playerStates.find(clientId);
        if (it == m_playerStates.end() || !it->second.isAlive)
            return false;

        // Every input path (peer datagrams, the listen-server host, the test seam) reaches the
        // authoritative state only through here, so hostile values are rejected here and not
        // just in the wire decoder. A non-finite field would poison position, yaw and every
        // projectile spawned from them, and then every snapshot that carries them.
        if (!std::isfinite(rawInput.forward) || !std::isfinite(rawInput.strafe) || !std::isfinite(rawInput.yaw) ||
            !std::isfinite(rawInput.pitch))
        {
            return false;
        }

        // Unreliable delivery can duplicate and reorder, and a hostile peer can replay: only a
        // sequence newer than the last applied one moves the player. 0 is never assigned.
        const auto lastIt = m_lastInputByPlayer.find(clientId);
        if (rawInput.sequenceNumber == 0 ||
            (lastIt != m_lastInputByPlayer.end() && rawInput.sequenceNumber <= lastIt->second.sequenceNumber))
        {
            return false;
        }

        // Movement axes are unit-bounded so an oversized axis cannot scale the speed. Pitch is
        // a look angle and stops at straight up/down. Yaw is wrapped into [-pi, pi]: the value
        // is echoed in every snapshot, and the wrap is the identity for the atan2 yaw an honest
        // client sends, so it never causes a reconciliation correction.
        PlayerInput input = rawInput;
        input.forward = std::clamp(input.forward, -1.0f, 1.0f);
        input.strafe = std::clamp(input.strafe, -1.0f, 1.0f);
        input.pitch = std::clamp(input.pitch, -0.5f * std::numbers::pi_v<float>, 0.5f * std::numbers::pi_v<float>);
        input.yaw = std::remainder(input.yaw, 2.0f * std::numbers::pi_v<float>);

        auto& state = it->second;

        StepPlanarMovement(state.yaw, input.forward, input.strafe, m_moveSpeed, dt, state.posX, state.posZ, state.velX,
                           state.velZ);
        state.velY = 0.0f;

        state.yaw = input.yaw;
        state.pitch = input.pitch;
        state.isCrouching = input.crouch;
        state.actionFlags = ActionNone;
        state.actionFlags |= input.jump ? ActionJump : ActionNone;
        state.actionFlags |= input.fire ? ActionFire : ActionNone;
        state.actionFlags |= input.reload ? ActionReload : ActionNone;
        state.actionFlags |= input.crouch ? ActionCrouch : ActionNone;
        m_lastInputByPlayer[clientId] = input;

        // The cooldown runs on applied input steps, which the input budget ties to the server
        // clock, so neither held fire nor an input flood outpaces the weapon's rate.
        float& fireCooldown = m_fireCooldown[clientId];
        fireCooldown = (std::max)(0.0f, fireCooldown - dt);
        if (input.fire && fireCooldown <= kInputBudgetEpsilon)
        {
            fireCooldown = kServerFireInterval;
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
        return true;
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
        it->second.pitch = 0.0f;
        it->second.velX = 0.0f;
        it->second.velY = 0.0f;
        it->second.velZ = 0.0f;
        it->second.actionFlags = ActionNone;
        it->second.health = 100.0f;
        it->second.isAlive = true;
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
        if (m_isServer)
            m_inputBudget[clientId] = kMaxInputBudget;

        PlayerScore score;
        score.clientId = clientId;
        score.playerName = "Player_" + std::to_string(clientId);
        m_scores[clientId] = score;

        FPS_CONSOLE("[FPSMultiplayer] Player " + std::to_string(clientId) + " joined", "INFO");
    }

    void FPSMultiplayerSystem::OnPlayerLeft(uint32_t clientId)
    {
        m_playerStates.erase(clientId);
        m_scores.erase(clientId);
        m_respawnTimers.erase(clientId);
        m_remoteSnapshots.erase(clientId);
        m_lastInputByPlayer.erase(clientId);
        m_inputBudget.erase(clientId);
        m_fireCooldown.erase(clientId);

        FPS_CONSOLE("[FPSMultiplayer] Player " + std::to_string(clientId) + " left", "INFO");
    }

    void FPSMultiplayerSystem::OnPlayerInputReceived(uint32_t clientId, const PlayerInput& input)
    {
        if (!m_isServer)
            return;
        ApplyClientInput(clientId, input, kInputStep);
    }

} // namespace SparkFPS
