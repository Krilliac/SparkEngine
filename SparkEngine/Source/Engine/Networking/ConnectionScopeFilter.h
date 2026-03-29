/**
 * @file ConnectionScopeFilter.h
 * @brief Per-connection entity visibility filtering for network replication
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides scope-based filtering so that each network connection only
 * receives entity updates for entities within its visibility scope.
 * Scoping criteria include:
 * - Area ID (only replicate entities in the same area)
 * - Position + radius (spatial proximity check)
 * - Team mask (bitwise AND — entities share at least one team bit)
 * - Visibility mask (custom per-entity flags checked against connection scope)
 *
 * This dramatically reduces bandwidth for large multiplayer worlds by
 * ensuring connections only receive data they can observe.
 *
 * ## Usage
 * @code
 *   auto& filter = Spark::Net::ConnectionScopeFilter::GetInstance();
 *
 *   // Set scope for a client connection
 *   ConnectionScope scope;
 *   scope.areaId = 42;
 *   scope.position = playerPosition;
 *   scope.radius = 150.0f;
 *   scope.teamMask = 0x01; // Team A only
 *   filter.SetScope(connectionId, scope);
 *
 *   // During replication, check each entity
 *   if (filter.IsEntityInScope(connectionId, entityPos, entityTeam, entityVisFlags))
 *   {
 *       ReplicateEntity(connectionId, entity);
 *   }
 * @endcode
 *
 * @see AreaServer.h, NetworkManager.h
 */

#pragma once

#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

using namespace DirectX;

namespace Spark::Net
{

    /**
     * @brief Defines the visibility scope for a single network connection
     *
     * An entity is in scope if ALL of the following are true:
     * 1. Entity's area ID matches the connection's area ID (or areaId == 0 for global)
     * 2. Entity position is within the connection's radius
     * 3. Entity team mask shares at least one bit with the connection's team mask
     * 4. Entity visibility flags share at least one bit with the connection's visibility mask
     */
    struct ConnectionScope
    {
        uint32_t areaId = 0;                  ///< Area filter (0 = accept all areas)
        XMFLOAT3 position{0, 0, 0};           ///< Center of visibility sphere
        float radius = 100.0f;                ///< Visibility radius
        uint32_t teamMask = 0xFFFFFFFF;       ///< Bit mask for team filtering
        uint32_t visibilityMask = 0xFFFFFFFF; ///< Bit mask for custom visibility flags
    };

    /**
     * @brief Per-connection entity visibility filter for network replication
     *
     * Maintains a scope per connection ID and provides fast in-scope checks
     * for entities during the replication phase.
     *
     * Thread safety: All methods are protected by a mutex and may be called
     * from the network thread or the main thread.
     */
    class ConnectionScopeFilter
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<ConnectionScopeFilter>() instead")]]
        static ConnectionScopeFilter& GetInstance()
        {
            static ConnectionScopeFilter instance;
            return instance;
        }

        /**
         * @brief Set or update the scope for a connection
         * @param connectionId Unique connection identifier
         * @param scope The visibility scope to apply
         */
        void SetScope(uint32_t connectionId, const ConnectionScope& scope)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_scopes[connectionId] = scope;
        }

        /**
         * @brief Check if an entity passes the scope filter for a connection
         *
         * @param connectionId The connection to check against
         * @param entityPos    Entity world-space position
         * @param entityAreaId Entity's area ID (0 = global entity, always passes area check)
         * @param entityTeam   Entity team bit mask
         * @param entityVisFlags Entity visibility bit flags
         * @return true if the entity should be replicated to this connection
         */
        bool IsEntityInScope(uint32_t connectionId, const XMFLOAT3& entityPos, uint32_t entityAreaId,
                             uint32_t entityTeam, uint32_t entityVisFlags) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto it = m_scopes.find(connectionId);
            if (it == m_scopes.end())
            {
                // No scope defined: default to accepting everything
                return true;
            }

            const auto& scope = it->second;

            // Area check: must match (unless either is 0, meaning global)
            if (scope.areaId != 0 && entityAreaId != 0 && scope.areaId != entityAreaId)
            {
                return false;
            }

            // Team check: at least one shared team bit
            if ((scope.teamMask & entityTeam) == 0)
            {
                return false;
            }

            // Visibility check: at least one shared visibility bit
            if ((scope.visibilityMask & entityVisFlags) == 0)
            {
                return false;
            }

            // Spatial proximity check
            float dx = entityPos.x - scope.position.x;
            float dy = entityPos.y - scope.position.y;
            float dz = entityPos.z - scope.position.z;
            float distSq = dx * dx + dy * dy + dz * dz;

            return distSq <= (scope.radius * scope.radius);
        }

        /**
         * @brief Remove a connection's scope (e.g., on disconnect)
         * @param connectionId Connection to remove
         */
        void RemoveConnection(uint32_t connectionId)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_scopes.erase(connectionId);
        }

        /**
         * @brief Get the number of tracked connections
         */
        size_t GetConnectionCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_scopes.size();
        }

        /**
         * @brief Clear all connection scopes
         */
        void Shutdown()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_scopes.clear();
        }

        /**
         * @brief Get diagnostic status string
         */
        std::string Console_GetStatus() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::string result = "ConnectionScopeFilter:\n";
            result += "  Tracked connections: " + std::to_string(m_scopes.size()) + "\n";

            for (const auto& [id, scope] : m_scopes)
            {
                result += "  [" + std::to_string(id) + "] area=" + std::to_string(scope.areaId) +
                          " radius=" + std::to_string(static_cast<int>(scope.radius)) + " team=0x" +
                          std::to_string(scope.teamMask) + "\n";
            }
            return result;
        }

      private:
        ConnectionScopeFilter() = default;
        ConnectionScopeFilter(const ConnectionScopeFilter&) = delete;
        ConnectionScopeFilter& operator=(const ConnectionScopeFilter&) = delete;

        std::unordered_map<uint32_t, ConnectionScope> m_scopes;
        mutable std::mutex m_mutex;
    };

} // namespace Spark::Net
