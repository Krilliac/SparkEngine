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
#include <string>
#include <unordered_map>
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
        float health = 100.0f;
        float maxHealth = 100.0f;
        int level = 1;
        bool isLocalPlayer = false;
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

        /// Spawn a local player at the given area's spawn point
        uint32_t SpawnLocalPlayer(const std::string& name, uint32_t areaId);

        /// Remove a player by client ID
        void RemovePlayer(uint32_t clientId);

      private:
        void SetupNetworkHandlers();
        void UpdatePrediction(float fixedDeltaTime);
        void UpdateInterpolation(float deltaTime);
        void CheckAreaBoundaries();

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, MMOPlayer> m_players; // keyed by clientId
        uint32_t m_localClientId{0};
        uint32_t m_nextNetworkId{1};
        bool m_initialized{false};
    };

} // namespace MMO
