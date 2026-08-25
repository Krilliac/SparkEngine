/**
 * @file MMOPlayerSystem.h
 * @brief MMO player management: spawning, replication, prediction, migration
 * @author Spark Engine Team
 * @date 2026
 *
 * Demonstrates how an MMO game module uses the engine's networking systems
 * for player lifecycle management:
 * - Player entity creation with replicated NetworkIdentity
 * - Client-side prediction via ClientPrediction
 * - Network interpolation for remote players
 * - Entity migration when players cross area boundaries
 * - Spatial grid integration for proximity awareness
 */

#pragma once

#include "Spark/IEngineContext.h"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MMO
{

    /// @brief Tracks a single MMO player's state across the game module
    struct MMOPlayer
    {
        uint32_t clientId = 0;
        uint32_t networkId = 0;
        std::string name;
        uint32_t currentAreaId = 0;
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        float targetPosX = 0.0f;
        float targetPosY = 0.0f;
        float targetPosZ = 0.0f;
        float velocityX = 0.0f;
        float velocityY = 0.0f;
        float velocityZ = 0.0f;
        float health = 100.0f;
        float maxHealth = 100.0f;
        int level = 1;
        bool isLocalPlayer = false;
    };

    /// @brief Normalized movement request used by keyboard input and deterministic tests.
    struct MMOPlayerInput
    {
        float moveX = 0.0f;
        float moveZ = 0.0f;
        bool sprint = false;
    };

    /**
     * @brief Manages MMO player entities and their network lifecycle
     *
     * On the server side:
     * - Spawns player entities when clients connect
     * - Manages entity replication via EntityReplicator
     * - Handles area migration when players cross boundaries
     * - Updates spatial grid for proximity queries
     *
     * On the client side:
     * - Applies client-side prediction for the local player
     * - Interpolates remote player positions
     * - Displays player nameplates and health bars
     */
    class MMOPlayerSystem
    {
      public:
        MMOPlayerSystem() = default;
        ~MMOPlayerSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Render();
        void Shutdown();

        void RenderDebugUI();

        size_t GetPlayerCount() const { return m_players.size(); }
        std::string GetPlayerListString() const;
        std::string GetLocalPlayerStatusString() const;

        const MMOPlayer* GetLocalPlayer() const;

        /// Apply a normalized movement request to the local player.
        bool ApplyLocalInput(const MMOPlayerInput& input, float deltaTime);

        /// Deterministic movement primitive shared by runtime input and tests.
        static void IntegrateMovement(MMOPlayer& player, const MMOPlayerInput& input, float deltaTime);

        /// Teleport the local player and mark the replicated state dirty.
        bool TeleportLocalPlayer(uint32_t areaId, float x, float y, float z);

        /// Replace the demo identity/stats when a selected character enters the world.
        bool ConfigureLocalPlayer(const std::string& name, int level, float maxHealth, uint32_t areaId, float x,
                                  float y, float z);

        bool DamageLocalPlayer(float amount);
        bool HealLocalPlayer(float amount);
        bool RespawnLocalPlayer(uint32_t areaId, float x, float y, float z);

        using AreaResolver = std::function<uint32_t(float x, float y, float z, uint32_t currentAreaId)>;
        using AreaTransitionCallback = std::function<void(uint32_t oldAreaId, uint32_t newAreaId)>;
        void SetAreaResolver(AreaResolver resolver) { m_areaResolver = std::move(resolver); }
        void SetAreaTransitionCallback(AreaTransitionCallback callback)
        {
            m_areaTransitionCallback = std::move(callback);
        }

        /// Spawn a local player at the given area's spawn point
        uint32_t SpawnLocalPlayer(const std::string& name, uint32_t areaId);

        /// Remove a player by client ID
        void RemovePlayer(uint32_t clientId);

      private:
        void SetupNetworkHandlers();
        void ProcessInput(float deltaTime);
        void UpdatePrediction(float fixedDeltaTime);
        void UpdateInterpolation(float deltaTime);
        void CheckAreaBoundaries();
        void MarkLocalStateDirty();
        void SyncLocalPlayerState();
        MMOPlayer* GetLocalPlayerMutable();
        MMOPlayer* FindPlayerByNetworkId(uint32_t networkId);

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, MMOPlayer> m_players; // keyed by clientId
        uint32_t m_localClientId{0};
        uint32_t m_nextNetworkId{1};
        float m_networkSendTimer{0.0f};
        bool m_localStateDirty{false};
        AreaResolver m_areaResolver;
        AreaTransitionCallback m_areaTransitionCallback;
        bool m_initialized{false};

        static constexpr float WALK_SPEED = 6.0f;
        static constexpr float SPRINT_MULTIPLIER = 1.75f;
        static constexpr float REMOTE_INTERPOLATION_RATE = 12.0f;
        static constexpr float NETWORK_SEND_INTERVAL = 0.05f;
    };

} // namespace MMO
