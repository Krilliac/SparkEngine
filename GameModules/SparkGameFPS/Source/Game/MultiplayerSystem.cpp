/**
 * @file MultiplayerSystem.cpp
 * @brief FPS multiplayer system implementation
 */

#include "MultiplayerSystem.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>

namespace SparkFPS
{

    namespace
    {
        // Player hitbox: an axis-aligned box standing on the player origin. The eye
        // (the projectile muzzle height in ApplyClientInput/SendInput) sits inside it.
        constexpr float kHitboxHalfWidth = 0.4f;
        constexpr float kHitboxHeight = 1.8f;
        constexpr float kEyeHeight = 1.0f;

        // The server's validation ray starts this far past the face where it leaves the
        // attacker's own hitbox, so the shooter never occludes the shot at any elevation.
        constexpr float kMuzzleClearance = 0.01f;

        // A client damage report names a victim; the attacker's last applied yaw must point
        // at that victim's hitbox within this extra slack, which covers the aim drift between
        // the input the server last applied and the shot.
        constexpr float kAimToleranceRadians = 0.1745f; // 10 degrees

        constexpr float kPi = 3.14159265358979f;

        constexpr size_t kMaxSnapshotHistory = 4;

        // Every input is one fixed simulation step on the client's prediction and on the server.
        constexpr float kInputStep = 1.0f / 60.0f;

        // Server-side input rate budget: a player earns simulated time as the server clock
        // advances and spends one step per applied input, so flooding inputs cannot move a
        // player faster than real time. The cap absorbs network jitter bursts.
        constexpr float kMaxInputBudget = 0.25f;
        constexpr float kInputBudgetEpsilon = 1e-4f;

        // Server-owned fire interval: 600 rounds per minute, the default WeaponStats rate. Held
        // fire in 60 Hz inputs spawns one projectile every 6 steps, whatever the client sends.
        // With ProjectileData's fixed 3 s lifetime this also bounds each owner to 30 live projectiles.
        constexpr float kServerFireInterval = 0.1f;

        // StateSnapshot wire layout (see FPSMessageType::StateSnapshot).
        constexpr size_t kSnapshotHeaderSize = sizeof(uint32_t) + sizeof(uint16_t);
        constexpr size_t kSnapshotScoreSize = 4 * sizeof(uint32_t);
        constexpr size_t kSnapshotRecordSize = NetworkPlayerState::SerializedSize + kSnapshotScoreSize;

        bool IsFiniteState(const NetworkPlayerState& state)
        {
            return std::isfinite(state.posX) && std::isfinite(state.posY) && std::isfinite(state.posZ) &&
                   std::isfinite(state.velX) && std::isfinite(state.velY) && std::isfinite(state.velZ) &&
                   std::isfinite(state.yaw) && std::isfinite(state.pitch) && std::isfinite(state.health);
        }

        void HitboxBounds(const NetworkPlayerState& state, DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax)
        {
            outMin = {state.posX - kHitboxHalfWidth, state.posY, state.posZ - kHitboxHalfWidth};
            outMax = {state.posX + kHitboxHalfWidth, state.posY + kHitboxHeight, state.posZ + kHitboxHalfWidth};
        }

        // Distance along a unit @p dir from @p point (inside the box) to where the ray
        // leaves the box: the nearest far-side slab.
        float RayExitDistance(const std::array<float, 3>& point, const std::array<float, 3>& dir,
                              const DirectX::XMFLOAT3& boxMin, const DirectX::XMFLOAT3& boxMax)
        {
            const std::array<float, 3> lo{boxMin.x, boxMin.y, boxMin.z};
            const std::array<float, 3> hi{boxMax.x, boxMax.y, boxMax.z};
            float exit = std::numeric_limits<float>::max();
            for (size_t axis = 0; axis < 3; ++axis)
            {
                if (std::abs(dir[axis]) < 1e-8f)
                    continue;
                const float farFace = dir[axis] > 0.0f ? hi[axis] : lo[axis];
                exit = (std::min)(exit, (farFace - point[axis]) / dir[axis]);
            }
            return (std::max)(0.0f, exit);
        }

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

        // Swept test of the segment [from, to] against an AABB. A fast projectile moves
        // several metres per tick, so a point-in-radius test would tunnel through players.
        bool SegmentEntersBox(const std::array<float, 3>& from, const std::array<float, 3>& to,
                              const DirectX::XMFLOAT3& boxMin, const DirectX::XMFLOAT3& boxMax, float& outEntry)
        {
            const std::array<float, 3> lo{boxMin.x, boxMin.y, boxMin.z};
            const std::array<float, 3> hi{boxMax.x, boxMax.y, boxMax.z};
            float entry = 0.0f;
            float exit = 1.0f;
            for (size_t axis = 0; axis < 3; ++axis)
            {
                const float delta = to[axis] - from[axis];
                if (std::abs(delta) < 1e-8f)
                {
                    if (from[axis] < lo[axis] || from[axis] > hi[axis])
                        return false;
                    continue;
                }

                float t1 = (lo[axis] - from[axis]) / delta;
                float t2 = (hi[axis] - from[axis]) / delta;
                if (t1 > t2)
                    std::swap(t1, t2);
                entry = (std::max)(entry, t1);
                exit = (std::min)(exit, t2);
                if (entry > exit)
                    return false;
            }
            outEntry = entry;
            return true;
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

        auto& console = Spark::SimpleConsole::GetInstance();
        console.Log("[FPSMultiplayer] Initialized (" + std::string(isServer ? "Server" : "Client") + " mode)");
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
        // The host is a player too, so the session never exceeds kMaxPlayers.
        if (maxPlayers == 0 || maxPlayers >= kMaxPlayers)
            return false;

        auto& network = Spark::Net::NetworkManager::GetInstance();
        if (!network.Initialize() || !network.StartServer(port, static_cast<int>(maxPlayers)))
            return false;

        RegisterNetworkHandlers();
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
        auto& network = Spark::Net::NetworkManager::GetInstance();
        // NetworkManager::Shutdown clears message handlers but not the timeout handler, and
        // the NetworkManager outlives this module's image: release the callback into it here.
        network.SetTimeoutHandler(nullptr);
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
        if (!network.Initialize() || !network.Connect(address, port, "FPSPlayer"))
            return false;

        RegisterNetworkHandlers();
        m_isActive = true;
        m_isServer = false;
        m_localClientId = Spark::Net::INVALID_CLIENT;
        m_lastSnapshotBatch = 0;

        auto& console = Spark::SimpleConsole::GetInstance();
        console.Log("[FPSMultiplayer] Connecting to " + address + ":" + std::to_string(port));
        return true;
    }

    void FPSMultiplayerSystem::Disconnect()
    {
        auto& network = Spark::Net::NetworkManager::GetInstance();
        // See StopServer: the timeout handler must not outlive the session. NetworkManager
        // invokes a copy of it, so clearing it from inside that callback is safe.
        network.SetTimeoutHandler(nullptr);
        network.Disconnect();
        m_isActive = false;
        m_playerStates.clear();
        m_localClientId = Spark::Net::INVALID_CLIENT;
    }

    void FPSMultiplayerSystem::SendInput(const PlayerInput& input)
    {
        if (!m_isActive)
            return;

        if (m_isServer)
        {
            // Listen server: the host's own input is authoritative and applied directly.
            const auto lastIt = m_lastInputByPlayer.find(m_localClientId);
            PlayerInput hostInput = input;
            hostInput.sequenceNumber = lastIt != m_lastInputByPlayer.end() ? lastIt->second.sequenceNumber + 1 : 1;
            ApplyClientInput(m_localClientId, hostInput, kInputStep);
            return;
        }

        // Until the handshake assigns an id there is no player to predict or to send for.
        if (m_localClientId == Spark::Net::INVALID_CLIENT)
            return;

        Spark::PredictedInput predicted{};
        predicted.timestamp = static_cast<float>(m_clientPrediction.GetCurrentSequence() + 1) * (1.0f / 60.0f);
        predicted.moveDirection = {input.strafe, 0.0f, input.forward};
        predicted.lookYaw = input.yaw;
        predicted.lookPitch = input.pitch;
        predicted.jump = input.jump;
        predicted.crouch = input.crouch;
        predicted.fire = input.fire;
        predicted.reload = input.reload;
        const uint32_t assignedSequence = m_clientPrediction.RecordInput(predicted);
        predicted.sequenceNumber = assignedSequence;

        PlayerInput sent = input;
        sent.sequenceNumber = assignedSequence;
        m_lastInputByPlayer[m_localClientId] = sent;

        m_clientPrediction.ApplyPrediction(m_localPredictedState, predicted, 1.0f / 60.0f);
        CopyPredictedMotionToLocal();

        // FPS input travels as its own message, not MessageType::ClientInput: NetworkManager
        // renumbers ClientInput sequences, and the server must acknowledge the sequence
        // client prediction assigned.
        Spark::Net::NetworkMessage message;
        message.type = static_cast<Spark::Net::MessageType>(FPSMessageType::PlayerInput);
        message.channel = Spark::Net::ChannelType::Unreliable;
        message.senderID = m_localClientId;
        message.payload = sent.Serialize();
        Spark::Net::NetworkManager::GetInstance().SendMessage(message);

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

    void FPSMultiplayerSystem::RecordLagCompensationHistory()
    {
        auto& network = Spark::Net::NetworkManager::GetInstance();
        Spark::Net::HistorySnapshot snapshot;
        snapshot.timestamp = network.GetServerTime();
        snapshot.entities.reserve(m_playerStates.size());
        for (const auto& [id, state] : m_playerStates)
        {
            if (!state.isAlive)
                continue;

            Spark::Net::HistorySnapshot::EntityState entity{};
            entity.networkID = id;
            entity.position = {state.posX, state.posY, state.posZ};
            entity.rotation = {state.pitch, state.yaw, 0.0f};
            HitboxBounds(state, entity.boundsMin, entity.boundsMax);
            snapshot.entities.push_back(entity);
        }
        network.GetLagCompensator().RecordSnapshot(snapshot);
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
            while (history.size() > kMaxSnapshotHistory)
            {
                history.pop_front();
            }
        }

        if (m_playerStates.size() > kMaxPlayers)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "FPSMultiplayerSystem: %zu players exceed the %u-player batch",
                            m_playerStates.size(), kMaxPlayers);
            return;
        }

        std::vector<uint8_t> payload;
        payload.reserve(kSnapshotHeaderSize + m_playerStates.size() * kSnapshotRecordSize);
        Detail::WriteU32(payload, m_stateSequence);
        const auto count = static_cast<uint16_t>(m_playerStates.size());
        payload.push_back(static_cast<uint8_t>(count));
        payload.push_back(static_cast<uint8_t>(count >> 8));
        for (const auto& [id, state] : m_playerStates)
        {
            const std::vector<uint8_t> record = state.Serialize();
            payload.insert(payload.end(), record.begin(), record.end());
            const auto scoreIt = m_scores.find(id);
            const bool scored = scoreIt != m_scores.end();
            Detail::WriteU32(payload, scored ? scoreIt->second.kills : 0);
            Detail::WriteU32(payload, scored ? scoreIt->second.deaths : 0);
            Detail::WriteU32(payload, scored ? scoreIt->second.assists : 0);
            Detail::WriteU32(payload, scored ? static_cast<uint32_t>(scoreIt->second.score) : 0);
        }

        Spark::Net::NetworkMessage message;
        message.type = static_cast<Spark::Net::MessageType>(FPSMessageType::StateSnapshot);
        message.channel = Spark::Net::ChannelType::Unreliable;
        message.payload = std::move(payload);
        Spark::Net::NetworkManager::GetInstance().SendToAll(message);
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
        input.pitch = std::clamp(input.pitch, -0.5f * kPi, 0.5f * kPi);
        input.yaw = std::remainder(input.yaw, 2.0f * kPi);

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

    void FPSMultiplayerSystem::ValidateHit(uint32_t attackerId, uint32_t victimId, float damage)
    {
        // Damage arrives from projectiles and from client damage reports; a NaN or
        // non-positive amount would corrupt health or heal the victim.
        if (!std::isfinite(damage) || damage <= 0.0f)
            return;

        // The amount is server-owned: a report or client-fired projectile can deal at most
        // the one weapon's per-hit damage, never the figure the client chose.
        damage = (std::min)(damage, ProjectileData{}.damage);

        auto victimIt = m_playerStates.find(victimId);
        if (victimIt == m_playerStates.end() || !victimIt->second.isAlive)
            return;

        if (m_isServer)
        {
            // Only a present attacker can be credited; otherwise m_scores would grow a
            // default-constructed entry for an unknown id.
            auto attackerIt = m_playerStates.find(attackerId);
            if (attackerIt == m_playerStates.end() || attackerId == victimId)
                return;

            const NetworkPlayerState& attacker = attackerIt->second;
            const NetworkPlayerState& victim = victimIt->second;
            const float dx = victim.posX - attacker.posX;
            const float dy = victim.posY - attacker.posY;
            const float dz = victim.posZ - attacker.posZ;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!(distance > 1e-4f))
                return;

            // Eye-to-eye ray against the lag-compensated hitboxes. The hit counts only
            // when the first box along the ray is the victim's, so another player
            // standing in the line of fire blocks it.
            // The ray starts where it leaves the attacker's own hitbox, whatever the
            // elevation, so the shooter's box is never the first one hit.
            const std::array<float, 3> dir{dx / distance, dy / distance, dz / distance};
            const std::array<float, 3> eye{attacker.posX, attacker.posY + kEyeHeight, attacker.posZ};
            DirectX::XMFLOAT3 attackerMin;
            DirectX::XMFLOAT3 attackerMax;
            HitboxBounds(attacker, attackerMin, attackerMax);
            const float muzzle = RayExitDistance(eye, dir, attackerMin, attackerMax) + kMuzzleClearance;
            const DirectX::XMFLOAT3 rayDir(dir[0], dir[1], dir[2]);
            const DirectX::XMFLOAT3 rayOrigin(eye[0] + dir[0] * muzzle, eye[1] + dir[1] * muzzle,
                                              eye[2] + dir[2] * muzzle);
            auto& network = Spark::Net::NetworkManager::GetInstance();
            const float halfRTT = network.GetEstimatedRTT() * 0.0005f;
            const auto result = network.ValidateHit(network.GetServerTime(), halfRTT, rayOrigin, rayDir);
            if (!result.hit || result.entityID != victimId)
                return;
        }

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
        it->second.pitch = 0.0f;
        it->second.velX = 0.0f;
        it->second.velY = 0.0f;
        it->second.velZ = 0.0f;
        it->second.actionFlags = ActionNone;
        it->second.health = 100.0f;
        it->second.isAlive = true;
    }

    // ============================================================================
    // Client Logic
    // ============================================================================

    void FPSMultiplayerSystem::ClientUpdate(float dt)
    {
        // Reconcile only against a server snapshot. Reconciling against the local
        // player's own predicted state would replay every pending input on top of
        // movement already applied, and the player would run away from the server.
        if (m_hasPendingLocalAuthority)
        {
            m_hasPendingLocalAuthority = false;
            ReconcileToAuthoritativeState(m_pendingLocalAuthority);
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
            current.health = target.health;
            current.isAlive = target.isAlive;
            current.isCrouching = target.isCrouching;
            current.currentWeapon = target.currentWeapon;
            current.sequenceNumber = target.sequenceNumber;
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
        if (m_isServer)
            m_inputBudget[clientId] = kMaxInputBudget;

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
        m_remoteSnapshots.erase(clientId);
        m_lastInputByPlayer.erase(clientId);
        m_inputBudget.erase(clientId);
        m_fireCooldown.erase(clientId);

        auto& console = Spark::SimpleConsole::GetInstance();
        console.Log("[FPSMultiplayer] Player " + std::to_string(clientId) + " left");
    }

    void FPSMultiplayerSystem::OnPlayerInputReceived(uint32_t clientId, const PlayerInput& input)
    {
        if (!m_isServer)
            return;
        ApplyClientInput(clientId, input, kInputStep);
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

        // A report is a client claim. Beyond ValidateHit's line-of-fire check, the attacker
        // must be alive and aiming at the victim: its last applied yaw must point into the
        // victim's hitbox (widened by kAimToleranceRadians).
        const auto attackerIt = m_playerStates.find(attackerId);
        const auto victimIt = m_playerStates.find(victimId);
        if (attackerIt == m_playerStates.end() || victimIt == m_playerStates.end() || !attackerIt->second.isAlive)
            return;

        const NetworkPlayerState& attacker = attackerIt->second;
        const NetworkPlayerState& victim = victimIt->second;
        const float dx = victim.posX - attacker.posX;
        const float dz = victim.posZ - attacker.posZ;
        const float planarDistance = std::sqrt(dx * dx + dz * dz);
        const float hitboxHalfDiagonal = kHitboxHalfWidth * std::sqrt(2.0f);
        if (planarDistance > hitboxHalfDiagonal)
        {
            float aimError = std::atan2(dz, dx) - attacker.yaw;
            aimError = std::remainder(aimError, 2.0f * kPi);
            const float allowedError = std::atan(hitboxHalfDiagonal / planarDistance) + kAimToleranceRadians;
            if (!(std::abs(aimError) <= allowedError))
                return;
        }

        ValidateHit(attackerId, victimId, damage);
    }

    void FPSMultiplayerSystem::OnStateSnapshotReceived(const NetworkPlayerState& snapshot)
    {
        // Until the handshake assigns an id, m_localClientId is INVALID_CLIENT (0), which is
        // also the host's player id: the host's snapshots would be reconciled as our own.
        if (m_isServer || m_localClientId == Spark::Net::INVALID_CLIENT)
            return;

        if (snapshot.clientId == m_localClientId)
        {
            // Server snapshot sequences start at 1; 0 is a default-constructed or
            // truncated payload and never authoritative.
            if (snapshot.sequenceNumber <= m_lastLocalAuthoritySequence)
                return;
            m_lastLocalAuthoritySequence = snapshot.sequenceNumber;
            m_pendingLocalAuthority = snapshot;
            m_hasPendingLocalAuthority = true;
            return;
        }

        auto& history = m_remoteSnapshots[snapshot.clientId];
        if (!history.empty() && snapshot.sequenceNumber <= history.back().sequenceNumber)
            return;
        history.push_back(snapshot);
        while (history.size() > kMaxSnapshotHistory)
            history.pop_front();
    }

    // ============================================================================
    // NetworkManager Message Flow
    // ============================================================================

    void FPSMultiplayerSystem::RegisterNetworkHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& network = Spark::Net::NetworkManager::GetInstance();

        // NetworkManager surfaces each admitted client once as a Connect event whose
        // senderID is the id it assigned.
        network.RegisterHandler(MessageType::Connect,
                                [this](const NetworkMessage& message)
                                {
                                    if (m_isServer && m_isActive && message.senderID != Spark::Net::INVALID_CLIENT &&
                                        !m_playerStates.contains(message.senderID))
                                    {
                                        OnPlayerJoined(message.senderID);
                                    }
                                });
        network.RegisterHandler(MessageType::Disconnect,
                                [this](const NetworkMessage& message) { HandlePeerDisconnected(message.senderID); });
        network.SetTimeoutHandler([this](Spark::Net::ClientID clientId) { HandlePeerDisconnected(clientId); });
        network.RegisterHandler(static_cast<MessageType>(FPSMessageType::PlayerInput),
                                [this](const NetworkMessage& message) { HandleInputMessage(message); });
        network.RegisterHandler(static_cast<MessageType>(FPSMessageType::StateSnapshot),
                                [this](const NetworkMessage& message) { HandleSnapshotMessage(message); });
    }

    void FPSMultiplayerSystem::HandlePeerDisconnected(uint32_t clientId)
    {
        if (!m_isActive)
            return;

        if (m_isServer)
        {
            // The host's own player is never removed by a peer event.
            if (clientId != m_localClientId && m_playerStates.contains(clientId))
                OnPlayerLeft(clientId);
            return;
        }

        // A client hears Disconnect only from its server endpoint: the session is over.
        Spark::SimpleConsole::GetInstance().Log("[FPSMultiplayer] Server closed the session");
        Disconnect();
    }

    void FPSMultiplayerSystem::HandleInputMessage(const Spark::Net::NetworkMessage& message)
    {
        if (!m_isServer || !m_isActive)
            return;

        // senderID is stamped by NetworkManager from the admitted endpoint, never read off the wire.
        const uint32_t clientId = message.senderID;
        if (clientId == m_localClientId || !m_playerStates.contains(clientId))
            return;
        if (message.payload.size() != PlayerInput::SerializedSize)
            return;

        const auto budgetIt = m_inputBudget.find(clientId);
        if (budgetIt == m_inputBudget.end() || budgetIt->second + kInputBudgetEpsilon < kInputStep)
            return;

        // ApplyClientInput rejects non-finite, replayed and zero-sequence input; only an applied
        // input spends one step of the player's budget.
        const PlayerInput input = PlayerInput::Deserialize(message.payload.data(), message.payload.size());
        if (ApplyClientInput(clientId, input, kInputStep))
            budgetIt->second = (std::max)(0.0f, budgetIt->second - kInputStep);
    }

    void FPSMultiplayerSystem::HandleSnapshotMessage(const Spark::Net::NetworkMessage& message)
    {
        if (m_isServer || !m_isActive || m_localClientId == Spark::Net::INVALID_CLIENT)
            return;

        const std::vector<uint8_t>& payload = message.payload;
        if (payload.size() < kSnapshotHeaderSize)
            return;

        size_t offset = 0;
        const uint32_t batch = Detail::ReadU32(payload.data(), offset);
        const uint32_t count =
            static_cast<uint32_t>(payload[offset]) | (static_cast<uint32_t>(payload[offset + 1]) << 8);
        if (count > kMaxPlayers || payload.size() != kSnapshotHeaderSize + count * kSnapshotRecordSize)
            return;
        if (batch <= m_lastSnapshotBatch)
            return;

        auto recordAt = [&payload](uint32_t index)
        { return payload.data() + kSnapshotHeaderSize + static_cast<size_t>(index) * kSnapshotRecordSize; };

        // Validate the whole batch before applying any of it.
        for (uint32_t index = 0; index < count; ++index)
        {
            const NetworkPlayerState state =
                NetworkPlayerState::Deserialize(recordAt(index), NetworkPlayerState::SerializedSize);
            if (!IsFiniteState(state) || state.sequenceNumber != batch)
                return;
        }
        m_lastSnapshotBatch = batch;

        for (uint32_t index = 0; index < count; ++index)
        {
            const uint8_t* record = recordAt(index);
            const NetworkPlayerState state =
                NetworkPlayerState::Deserialize(record, NetworkPlayerState::SerializedSize);
            OnStateSnapshotReceived(state);

            size_t scoreOffset = NetworkPlayerState::SerializedSize;
            auto [scoreIt, inserted] = m_scores.try_emplace(state.clientId);
            PlayerScore& score = scoreIt->second;
            if (inserted)
                score.playerName = "Player_" + std::to_string(state.clientId);
            score.clientId = state.clientId;
            score.kills = Detail::ReadU32(record, scoreOffset);
            score.deaths = Detail::ReadU32(record, scoreOffset);
            score.assists = Detail::ReadU32(record, scoreOffset);
            score.score = static_cast<int32_t>(Detail::ReadU32(record, scoreOffset));
        }

        // The batch lists every player in the session; a remote player missing from it left.
        auto inBatch = [&](uint32_t clientId)
        {
            for (uint32_t index = 0; index < count; ++index)
            {
                size_t idOffset = 0;
                if (Detail::ReadU32(recordAt(index), idOffset) == clientId)
                    return true;
            }
            return false;
        };
        for (auto it = m_playerStates.begin(); it != m_playerStates.end();)
        {
            if (it->first != m_localClientId && !inBatch(it->first))
            {
                const uint32_t departed = it->first;
                ++it;
                OnPlayerLeft(departed);
            }
            else
            {
                ++it;
            }
        }
        for (auto it = m_remoteSnapshots.begin(); it != m_remoteSnapshots.end();)
        {
            if (it->first != m_localClientId && !inBatch(it->first))
                it = m_remoteSnapshots.erase(it);
            else
                ++it;
        }
        for (auto it = m_scores.begin(); it != m_scores.end();)
        {
            if (it->first != m_localClientId && !inBatch(it->first))
                it = m_scores.erase(it);
            else
                ++it;
        }
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
            const std::array<float, 3> from{projectile.positionX, projectile.positionY, projectile.positionZ};
            projectile.positionX += projectile.velocityX * dt;
            projectile.positionY += projectile.velocityY * dt;
            projectile.positionZ += projectile.velocityZ * dt;
            projectile.lifetime -= dt;
            const std::array<float, 3> to{projectile.positionX, projectile.positionY, projectile.positionZ};

            bool despawned = (projectile.lifetime <= 0.0f);
            if (!despawned && m_isServer)
            {
                // The nearest hitbox along this tick's path takes the hit.
                bool struck = false;
                uint32_t struckId = 0;
                float nearestEntry = 2.0f;
                for (const auto& [playerId, state] : m_playerStates)
                {
                    if (playerId == projectile.ownerId || !state.isAlive)
                        continue;

                    DirectX::XMFLOAT3 boxMin;
                    DirectX::XMFLOAT3 boxMax;
                    HitboxBounds(state, boxMin, boxMax);
                    float entry = 0.0f;
                    if (SegmentEntersBox(from, to, boxMin, boxMax, entry) && entry < nearestEntry)
                    {
                        nearestEntry = entry;
                        struckId = playerId;
                        struck = true;
                    }
                }

                if (struck)
                {
                    ValidateHit(projectile.ownerId, struckId, projectile.damage);
                    despawned = true;
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

        // Later inputs predict from the reconciled state, not the pre-snapshot one.
        m_localPredictedState = m_clientPrediction.GetState();

        // Health, life and score-relevant fields are server-owned; motion is the
        // reconciled prediction.
        auto& local = m_playerStates[m_localClientId];
        local.clientId = m_localClientId;
        local.health = authoritativeState.health;
        local.isAlive = authoritativeState.isAlive;
        local.currentWeapon = authoritativeState.currentWeapon;
        local.actionFlags = authoritativeState.actionFlags;
        local.acknowledgedInputSequence = authoritativeState.acknowledgedInputSequence;
        local.sequenceNumber = authoritativeState.sequenceNumber;
        CopyPredictedMotionToLocal();
    }

    void FPSMultiplayerSystem::CopyPredictedMotionToLocal()
    {
        auto localIt = m_playerStates.find(m_localClientId);
        if (localIt == m_playerStates.end())
            return;

        const auto& predicted = m_clientPrediction.GetState();
        auto& local = localIt->second;
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
