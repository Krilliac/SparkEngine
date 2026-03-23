/**
 * @file ConnectionScope.h
 * @brief Per-connection object scoping for entity replication (Torque3D ghost-inspired)
 *
 * Determines which entities each client connection can "see" based on spatial
 * proximity and priority. Only in-scope entities are replicated to a given client,
 * saving bandwidth and preventing information leaks (e.g. enemy positions behind walls).
 *
 * The server calls UpdateScopes() each tick, which evaluates every replicated entity
 * against every connected client's viewpoint. Entities entering scope get a full
 * create packet; entities leaving scope get a destroy notification; entities staying
 * in scope get normal delta updates.
 *
 * ## Usage
 * @code
 *   ConnectionScope scope;
 *   scope.SetScopeRadius(500.0f);
 *
 *   // Each tick, update scoping for each client
 *   scope.SetClientViewpoint(clientId, playerPosition);
 *   scope.UpdateEntityPosition(entityNetId, entityPos);
 *   auto changes = scope.EvaluateScope(clientId);
 *
 *   for (auto id : changes.entered) { /* send create packet */ }
*for (auto id :
      changes.exited){/* send destroy notification */} * // Only replicate delta updates for changes.inScope entities
    *@endcode* /

#pragma once

#include "NetworkManager.h"
#include "../../Utils/ContainerUtils.h"

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

    namespace Spark::Net
{

    /// Result of scope evaluation for a single client
    struct ScopeChanges
    {
        std::vector<uint32_t> entered; ///< Entities newly entering scope (need create packet)
        std::vector<uint32_t> exited;  ///< Entities leaving scope (need destroy notification)
        std::vector<uint32_t> inScope; ///< Entities currently in scope (eligible for delta updates)
    };

    /// Per-entity spatial data for scoping
    struct ScopedEntity
    {
        uint32_t networkID = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float priorityBias = 1.0f;   ///< Multiplier on effective scope radius (>1 = easier to see)
        bool alwaysRelevant = false; ///< If true, entity is always in scope for all clients
    };

    /// Per-client viewpoint for scope queries
    struct ClientViewpoint
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /**
     * @brief Manages per-connection entity visibility based on spatial proximity.
     *
     * Thread safety: Not thread-safe. Call only from the server's main tick thread.
     */
    class ConnectionScope
    {
      public:
        ConnectionScope() = default;

        /// @brief Set the default scope radius (world units). Entities beyond this are out of scope.
        void SetScopeRadius(float radius) { m_scopeRadius = radius; }
        float GetScopeRadius() const { return m_scopeRadius; }

        /// @brief Set the maximum number of entities that can be in scope per client.
        void SetMaxEntitiesPerClient(uint32_t max) { m_maxEntitiesPerClient = max; }

        // -- Client management ---------------------------------------------------

        /// @brief Register a client for scope tracking. Call when a client connects.
        void AddClient(ClientID clientId)
        {
            m_clientViewpoints[clientId] = {};
            m_clientScopes[clientId] = {};
        }

        /// @brief Remove a client from scope tracking. Call when a client disconnects.
        void RemoveClient(ClientID clientId)
        {
            m_clientViewpoints.erase(clientId);
            m_clientScopes.erase(clientId);
        }

        /// @brief Update a client's viewpoint position (typically their player entity position).
        void SetClientViewpoint(ClientID clientId, float x, float y, float z)
        {
            auto it = m_clientViewpoints.find(clientId);
            if (it != m_clientViewpoints.end())
            {
                it->second = {x, y, z};
            }
        }

        // -- Entity management ---------------------------------------------------

        /// @brief Register an entity for scope evaluation.
        void AddEntity(uint32_t networkID, float x = 0.0f, float y = 0.0f, float z = 0.0f, bool alwaysRelevant = false)
        {
            m_entities[networkID] = {networkID, x, y, z, 1.0f, alwaysRelevant};
        }

        /// @brief Remove an entity from scope tracking.
        void RemoveEntity(uint32_t networkID)
        {
            m_entities.erase(networkID);
            // Also remove from all client scopes
            for (auto& [clientId, scope] : m_clientScopes)
            {
                scope.erase(networkID);
            }
        }

        /// @brief Update an entity's position (call each tick for moving entities).
        void UpdateEntityPosition(uint32_t networkID, float x, float y, float z)
        {
            auto it = m_entities.find(networkID);
            if (it != m_entities.end())
            {
                it->second.x = x;
                it->second.y = y;
                it->second.z = z;
            }
        }

        /// @brief Set an entity's priority bias (>1 = more likely to be in scope).
        void SetEntityPriorityBias(uint32_t networkID, float bias)
        {
            auto it = m_entities.find(networkID);
            if (it != m_entities.end())
            {
                it->second.priorityBias = bias;
            }
        }

        // -- Scope evaluation ----------------------------------------------------

        /**
         * @brief Evaluate which entities are in/out of scope for a given client.
         *
         * Compares current spatial state against the client's previous scope set.
         * Returns entered/exited/inScope lists for the caller to act on.
         *
         * @param clientId  The client to evaluate.
         * @return ScopeChanges with entered, exited, and inScope entity lists.
         */
        ScopeChanges EvaluateScope(ClientID clientId)
        {
            ScopeChanges changes;

            auto vpIt = m_clientViewpoints.find(clientId);
            auto scopeIt = m_clientScopes.find(clientId);
            if (vpIt == m_clientViewpoints.end() || scopeIt == m_clientScopes.end())
            {
                return changes;
            }

            const ClientViewpoint& vp = vpIt->second;
            std::unordered_set<uint32_t>& prevScope = scopeIt->second;
            std::unordered_set<uint32_t> newScope;
            float radiusSq = m_scopeRadius * m_scopeRadius;

            // Determine which entities are now in scope
            for (const auto& [entityId, entity] : m_entities)
            {
                bool inScope = entity.alwaysRelevant;

                if (!inScope)
                {
                    float dx = entity.x - vp.x;
                    float dy = entity.y - vp.y;
                    float dz = entity.z - vp.z;
                    float distSq = dx * dx + dy * dy + dz * dz;
                    float effectiveRadiusSq = radiusSq * entity.priorityBias * entity.priorityBias;
                    inScope = (distSq <= effectiveRadiusSq);
                }

                if (inScope)
                {
                    newScope.insert(entityId);
                }
            }

            // Compute entered (in newScope, not in prevScope)
            for (uint32_t id : newScope)
            {
                if (!Spark::ContainerUtils::Contains(prevScope, id))
                {
                    changes.entered.push_back(id);
                }
                changes.inScope.push_back(id);
            }

            // Compute exited (in prevScope, not in newScope)
            for (uint32_t id : prevScope)
            {
                if (!Spark::ContainerUtils::Contains(newScope, id))
                {
                    changes.exited.push_back(id);
                }
            }

            // Update stored scope
            prevScope = std::move(newScope);

            return changes;
        }

        /// @brief Check if an entity is currently in scope for a client.
        bool IsInScope(ClientID clientId, uint32_t networkID) const
        {
            auto it = m_clientScopes.find(clientId);
            if (it == m_clientScopes.end())
                return false;
            return it->second.count(networkID) > 0;
        }

        /// @brief Get the number of entities currently in scope for a client.
        size_t GetScopeCount(ClientID clientId) const
        {
            auto it = m_clientScopes.find(clientId);
            if (it == m_clientScopes.end())
                return 0;
            return it->second.size();
        }

        /// @brief Get total tracked entity count.
        size_t GetEntityCount() const { return m_entities.size(); }

        /// @brief Get total tracked client count.
        size_t GetClientCount() const { return m_clientViewpoints.size(); }

      private:
        float m_scopeRadius = 500.0f;
        uint32_t m_maxEntitiesPerClient = 1024;

        std::unordered_map<uint32_t, ScopedEntity> m_entities;
        std::unordered_map<ClientID, ClientViewpoint> m_clientViewpoints;
        std::unordered_map<ClientID, std::unordered_set<uint32_t>> m_clientScopes;
    };

} // namespace Spark::Net
