/**
 * @file MultiplayerSystem.h
 * @brief Networked multiplayer system for the FPS game module
 * @author Spark Engine Team
 * @date 2026
 *
 * Wires the engine's NetworkManager into the FPS game to provide real
 * networked multiplayer: player replication, projectile sync, hit
 * validation, scoreboard, and respawn logic.
 *
 * ## Architecture
 * - Server mode: authoritative simulation, broadcasts one state batch at 20Hz
 * - Client mode: sends input, receives state, interpolates remote players
 * - FPSMultiplayerSystem is the FPS module's only network path. It registers its
 *   handlers with NetworkManager (the engine transport SparkGameMMOFPS also uses) on
 *   every StartServer/Connect: admission (Connect), departure (Disconnect, timeout),
 *   FPSMessageType::PlayerInput (server) and FPSMessageType::StateSnapshot (client).
 *
 * ## Contract
 * - Thread affinity: game thread only. NetworkManager::Update runs inside Update(),
 *   and the handlers run synchronously from it.
 * - Ownership: process-lifetime singleton; the registered handlers capture it.
 * - Allocation: per-player maps grow on join; each 20Hz snapshot builds one payload.
 * - Scalability: at most kMaxPlayers players per session (the batch size limit).
 */

#pragma once

#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/ClientPrediction.h"
#include "Core/Platform.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace SparkFPS
{

    namespace Detail
    {
        inline void WriteU32(std::vector<uint8_t>& bytes, uint32_t value)
        {
            bytes.push_back(static_cast<uint8_t>(value));
            bytes.push_back(static_cast<uint8_t>(value >> 8));
            bytes.push_back(static_cast<uint8_t>(value >> 16));
            bytes.push_back(static_cast<uint8_t>(value >> 24));
        }

        inline void WriteFloat(std::vector<uint8_t>& bytes, float value)
        {
            WriteU32(bytes, std::bit_cast<uint32_t>(value));
        }

        inline uint32_t ReadU32(const uint8_t* bytes, size_t& offset)
        {
            const uint32_t value =
                static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
            offset += 4;
            return value;
        }

        inline float ReadFloat(const uint8_t* bytes, size_t& offset)
        {
            return std::bit_cast<float>(ReadU32(bytes, offset));
        }
    } // namespace Detail

    // ============================================================================
    // Message Types
    // ============================================================================

    /**
     * @brief FPS message types carried on NetworkManager.
     *
     * Built-in types below MessageType::UserDefined are rejected by the engine's packet
     * validator, and custom types are accepted only from admitted peers.
     */
    enum class FPSMessageType : uint16_t
    {
        /// Client to server, unreliable: one PlayerInput (PlayerInput::SerializedSize bytes).
        PlayerInput = static_cast<uint16_t>(Spark::Net::MessageType::UserDefined) + 53,
        /// Server to clients, unreliable: u32 batch sequence, u16 player count, then per
        /// player a NetworkPlayerState followed by kills, deaths, assists (u32) and score (i32).
        StateSnapshot = static_cast<uint16_t>(Spark::Net::MessageType::UserDefined) + 58
    };

    /// Most players one session holds; also the largest snapshot batch a client accepts.
    inline constexpr uint32_t kMaxPlayers = 256;

    // ============================================================================
    // Player State
    // ============================================================================

    /** @brief Replicated player state sent in each snapshot. */
    enum PlayerActionFlags : uint32_t
    {
        ActionNone = 0,
        ActionJump = 1u << 0,
        ActionFire = 1u << 1,
        ActionReload = 1u << 2,
        ActionCrouch = 1u << 3,
        ActionSprint = 1u << 4
    };

    struct NetworkPlayerState
    {
        static constexpr size_t SerializedSize = 55;

        uint32_t clientId = 0;
        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float velX = 0.0f, velY = 0.0f, velZ = 0.0f;
        float yaw = 0.0f, pitch = 0.0f;
        float health = 100.0f;
        uint32_t actionFlags = ActionNone;
        uint32_t acknowledgedInputSequence = 0;
        uint8_t currentWeapon = 0;
        bool isAlive = true;
        bool isCrouching = false;
        uint32_t sequenceNumber = 0;

        /** @brief Serialize state into byte buffer. */
        std::vector<uint8_t> Serialize() const
        {
            std::vector<uint8_t> bytes;
            bytes.reserve(SerializedSize);
            Detail::WriteU32(bytes, clientId);
            Detail::WriteFloat(bytes, posX);
            Detail::WriteFloat(bytes, posY);
            Detail::WriteFloat(bytes, posZ);
            Detail::WriteFloat(bytes, velX);
            Detail::WriteFloat(bytes, velY);
            Detail::WriteFloat(bytes, velZ);
            Detail::WriteFloat(bytes, yaw);
            Detail::WriteFloat(bytes, pitch);
            Detail::WriteFloat(bytes, health);
            Detail::WriteU32(bytes, actionFlags);
            Detail::WriteU32(bytes, acknowledgedInputSequence);
            bytes.push_back(currentWeapon);
            bytes.push_back(static_cast<uint8_t>(isAlive));
            bytes.push_back(static_cast<uint8_t>(isCrouching));
            Detail::WriteU32(bytes, sequenceNumber);
            return bytes;
        }

        /** @brief Deserialize state from byte buffer. */
        static NetworkPlayerState Deserialize(const uint8_t* data, size_t size)
        {
            if (!data || size < SerializedSize)
            {
                return {};
            }

            NetworkPlayerState state;
            size_t offset = 0;
            state.clientId = Detail::ReadU32(data, offset);
            state.posX = Detail::ReadFloat(data, offset);
            state.posY = Detail::ReadFloat(data, offset);
            state.posZ = Detail::ReadFloat(data, offset);
            state.velX = Detail::ReadFloat(data, offset);
            state.velY = Detail::ReadFloat(data, offset);
            state.velZ = Detail::ReadFloat(data, offset);
            state.yaw = Detail::ReadFloat(data, offset);
            state.pitch = Detail::ReadFloat(data, offset);
            state.health = Detail::ReadFloat(data, offset);
            state.actionFlags = Detail::ReadU32(data, offset);
            state.acknowledgedInputSequence = Detail::ReadU32(data, offset);
            state.currentWeapon = data[offset++];
            state.isAlive = data[offset++] != 0;
            state.isCrouching = data[offset++] != 0;
            state.sequenceNumber = Detail::ReadU32(data, offset);
            return state;
        }
    };

    /** @brief Client input sent to server each tick. */
    struct PlayerInput
    {
        static constexpr size_t SerializedSize = 24;

        float forward = 0.0f;
        float strafe = 0.0f;
        float yaw = 0.0f;
        float pitch = 0.0f;
        bool jump = false;
        bool fire = false;
        bool reload = false;
        bool crouch = false;
        uint32_t sequenceNumber = 0;

        std::vector<uint8_t> Serialize() const
        {
            std::vector<uint8_t> bytes;
            bytes.reserve(SerializedSize);
            Detail::WriteFloat(bytes, forward);
            Detail::WriteFloat(bytes, strafe);
            Detail::WriteFloat(bytes, yaw);
            Detail::WriteFloat(bytes, pitch);
            bytes.push_back(static_cast<uint8_t>(jump));
            bytes.push_back(static_cast<uint8_t>(fire));
            bytes.push_back(static_cast<uint8_t>(reload));
            bytes.push_back(static_cast<uint8_t>(crouch));
            Detail::WriteU32(bytes, sequenceNumber);
            return bytes;
        }

        static PlayerInput Deserialize(const uint8_t* data, size_t size)
        {
            if (!data || size < SerializedSize)
            {
                return {};
            }

            PlayerInput input;
            size_t offset = 0;
            input.forward = Detail::ReadFloat(data, offset);
            input.strafe = Detail::ReadFloat(data, offset);
            input.yaw = Detail::ReadFloat(data, offset);
            input.pitch = Detail::ReadFloat(data, offset);
            input.jump = data[offset++] != 0;
            input.fire = data[offset++] != 0;
            input.reload = data[offset++] != 0;
            input.crouch = data[offset++] != 0;
            input.sequenceNumber = Detail::ReadU32(data, offset);
            return input;
        }
    };

    /** @brief Per-player score tracking. */
    struct PlayerScore
    {
        uint32_t clientId = 0;
        std::string playerName;
        uint32_t kills = 0;
        uint32_t deaths = 0;
        uint32_t assists = 0;
        int32_t score = 0;
        uint32_t ping = 0;
    };

    /** @brief Projectile replication data. */
    struct ProjectileData
    {
        uint32_t projectileId = 0;
        uint32_t ownerId = 0;
        uint8_t weaponType = 0;
        float originX = 0.0f, originY = 0.0f, originZ = 0.0f;
        float dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f;
        float positionX = 0.0f, positionY = 0.0f, positionZ = 0.0f;
        float velocityX = 0.0f, velocityY = 0.0f, velocityZ = 0.0f;
        float lifetime = 3.0f;
        float speed = 500.0f;
        float damage = 25.0f;
        bool active = false;
    };

    // ============================================================================
    // Spawn Points
    // ============================================================================

    /** @brief Spawn point definition. */
    struct SpawnPoint
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float yaw = 0.0f;
    };

    // ============================================================================
    // FPSMultiplayerSystem
    // ============================================================================

    /**
     * @brief Multiplayer system wiring NetworkManager into the FPS game module
     *
     * Handles player replication, projectile sync, hit validation,
     * scoreboard tracking, and respawn logic.
     */
    class FPSMultiplayerSystem
    {
      public:
        static FPSMultiplayerSystem& GetInstance();

        /**
         * @brief Initialize the multiplayer system
         * @param isServer True for dedicated/listen server, false for client
         */
        void Initialize(bool isServer);

        /** @brief Per-frame update. */
        void Update(float deltaTime);

        /** @brief Clean shutdown. */
        void Shutdown();

        // -- Server API --

        /**
         * @brief Start hosting a game on the specified port and register the network handlers.
         * @param port UDP port; 0 binds an ephemeral port.
         * @param maxPlayers Client slots, 1..kMaxPlayers - 1 (the host holds one more player).
         * @return false when the arguments are out of range or NetworkManager cannot host.
         */
        bool StartServer(uint16_t port = 27015, uint32_t maxPlayers = 16);

        /** @brief Stop the server. */
        void StopServer();

        // -- Client API --

        /** @brief Start connecting to a server and register the network handlers. */
        bool Connect(const std::string& address, uint16_t port = 27015);

        /** @brief Disconnect from the server. */
        void Disconnect();

        /**
         * @brief Predict and send local player input to the server.
         *
         * The input sequence number is assigned by client prediction (monotonic within
         * a session, reset by Initialize) and overrides @p input.sequenceNumber, so the server's acknowledged
         * sequence always names an input the client still holds for reconciliation. Each call simulates one
         * 1/60 s step, so callers send at 60Hz. A client sends nothing until the handshake assigns its id;
         * on a listen server the input drives the host's own player directly.
         */
        void SendInput(const PlayerInput& input);

        // -- Shared API --

        /** @brief Get the current scoreboard. */
        std::vector<PlayerScore> GetScoreboard() const;

        /** @brief Get the state of a specific player. */
        const NetworkPlayerState* GetPlayerState(uint32_t clientId) const;

        /** @brief Get all connected player states. */
        const std::unordered_map<uint32_t, NetworkPlayerState>& GetAllPlayerStates() const;

        /** @brief Check if this instance is the server. */
        bool IsServer() const { return m_isServer; }

        /** @brief Check if connected (client) or hosting (server). */
        bool IsActive() const { return m_isActive; }

        /** @brief Get local client ID. */
        uint32_t GetLocalClientId() const { return m_localClientId; }

        /** @brief Add a spawn point. */
        void AddSpawnPoint(const SpawnPoint& point);

        /** @brief Console status string. */
        std::string Console_GetStatus() const;

        struct MultiplayerDebugMetrics
        {
            float packetLossPercent = 0.0f;
            float rttMs = 0.0f;
            uint32_t correctionCount = 0;
        };
        MultiplayerDebugMetrics GetDebugMetrics() const;

      private:
        FPSMultiplayerSystem() = default;

        // Narrow test seam (Tests/TestFPSMultiplayer.cpp) that invokes the private
        // message handlers the way NetworkManager dispatch will; it adds no behavior.
        friend struct FPSMultiplayerSystemTestAccess;

        // -- Message handlers --
        void OnPlayerJoined(uint32_t clientId);
        void OnPlayerLeft(uint32_t clientId);
        void OnPlayerInputReceived(uint32_t clientId, const PlayerInput& input);
        void OnProjectileFired(uint32_t clientId, const ProjectileData& proj);
        void OnPlayerDamaged(uint32_t attackerId, uint32_t victimId, float damage);
        /// Client: accept one authoritative player snapshot from the server. The local
        /// player's snapshot is reconciled on the next ClientUpdate; remote snapshots feed
        /// interpolation. Snapshots older than the newest one already accepted, and every
        /// snapshot that arrives before the handshake assigns this client an id, are dropped.
        void OnStateSnapshotReceived(const NetworkPlayerState& snapshot);

        // -- NetworkManager message flow --
        /// Register this system's observers with NetworkManager. NetworkManager::Shutdown clears the
        /// message handlers; StopServer and Disconnect clear the timeout handler, which it keeps.
        void RegisterNetworkHandlers();
        /// Server: decode one client's PlayerInput. Malformed, non-finite, replayed and
        /// over-rate inputs are dropped; movement axes are clamped to [-1, 1]. Fire spawns at
        /// most one projectile per server-owned fire interval.
        void HandleInputMessage(const Spark::Net::NetworkMessage& message);
        /// Client: decode one snapshot batch. Malformed or stale batches are dropped whole;
        /// remote players absent from an accepted batch have left the session.
        void HandleSnapshotMessage(const Spark::Net::NetworkMessage& message);
        /// Admitted peer disconnected (server) or the server closed the session (client).
        void HandlePeerDisconnected(uint32_t clientId);

        // -- Server logic --
        void ServerUpdate(float dt);
        /// Record every living player's hitbox into NetworkManager's lag compensator at the
        /// current server time so ValidateHit has a server-owned world state to rewind.
        void RecordLagCompensationHistory();
        void SendStateSnapshot();
        void ApplyClientInput(uint32_t clientId, const PlayerInput& input, float dt);
        void ValidateHit(uint32_t attackerId, uint32_t victimId, float damage);
        void UpdateProjectiles(float dt);
        SpawnPoint GetRandomSpawnPoint() const;
        void RespawnPlayer(uint32_t clientId);

        // -- Client logic --
        void ClientUpdate(float dt);
        void InterpolateRemotePlayers(float dt);
        void ReconcileToAuthoritativeState(const NetworkPlayerState& authoritativeState);
        void CopyPredictedMotionToLocal();

        bool m_isServer = false;
        bool m_isActive = false;
        uint32_t m_localClientId = 0;
        uint32_t m_tickRate = 20; // snapshots per second
        float m_tickAccumulator = 0.0f;
        float m_moveSpeed = 8.0f;
        float m_respawnTime = 5.0f;

        std::unordered_map<uint32_t, NetworkPlayerState> m_playerStates;
        std::unordered_map<uint32_t, ProjectileData> m_projectiles;
        std::unordered_map<uint32_t, PlayerScore> m_scores;
        std::unordered_map<uint32_t, float> m_respawnTimers;
        std::unordered_map<uint32_t, std::deque<NetworkPlayerState>> m_remoteSnapshots;
        std::unordered_map<uint32_t, PlayerInput> m_lastInputByPlayer;
        std::vector<SpawnPoint> m_spawnPoints;
        uint32_t m_stateSequence = 0;
        uint32_t m_nextProjectileId = 1;
        Spark::ClientPrediction m_clientPrediction;
        Spark::PredictedState m_localPredictedState{};
        NetworkPlayerState m_pendingLocalAuthority{};
        bool m_hasPendingLocalAuthority = false;
        uint32_t m_lastLocalAuthoritySequence = 0;
        uint32_t m_lastSnapshotBatch = 0;
        /// Server: simulated seconds of input each player may still submit (anti speed-hack).
        std::unordered_map<uint32_t, float> m_inputBudget;
        /// Server: seconds of applied input until each player may fire again (fire-rate limit).
        std::unordered_map<uint32_t, float> m_fireCooldown;
        uint32_t m_correctionCount = 0;
    };

} // namespace SparkFPS
