/**
 * @file NavMeshLink.h
 * @brief Off-mesh navigation links for jumps, ladders, and teleporters
 * @author Spark Engine Team
 * @date 2026
 *
 * Off-mesh links connect two disconnected NavMesh regions, allowing AI
 * agents to traverse gaps, climb ladders, use elevators, or teleport.
 * The NavMeshQuery system considers these links during pathfinding.
 */

#pragma once

#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::AI
{

    /**
     * @brief Traversal type for an off-mesh link.
     */
    enum class NavLinkTraversalType : uint8_t
    {
        Walk,    ///< Normal walking across a gap.
        Jump,    ///< Jump across (requires jump animation).
        Drop,    ///< Drop down (one-way, high to low).
        Climb,   ///< Climb up/down (ladder, ledge).
        Teleport ///< Instant teleport between endpoints.
    };

    /**
     * @brief An off-mesh navigation link between two world positions.
     */
    struct NavLink
    {
        uint32_t id = 0;
        XMFLOAT3 startPos{0.0f, 0.0f, 0.0f}; ///< Link start position.
        XMFLOAT3 endPos{0.0f, 0.0f, 0.0f};   ///< Link end position.
        float radius = 0.5f;                 ///< Agent radius tolerance.
        NavLinkTraversalType traversalType = NavLinkTraversalType::Walk;
        float traversalCost = 1.0f; ///< Pathfinding cost multiplier.
        bool bidirectional = true;  ///< Traversable in both directions.
        bool enabled = true;        ///< Active state.
        std::string animationName;  ///< Animation to play during traversal.
    };

    /**
     * @brief Manages off-mesh navigation links for pathfinding.
     *
     * Links are registered by ID and considered during NavMeshQuery pathfinding.
     * The query system checks if the agent's current path intersects any link
     * start positions and adds the link traversal to the path if beneficial.
     */
    class NavMeshLinkSystem
    {
      public:
        static NavMeshLinkSystem& GetInstance()
        {
            static NavMeshLinkSystem s;
            return s;
        }

        void Initialize() { m_links.clear(); }
        void Shutdown() { m_links.clear(); }

        uint32_t AddLink(const NavLink& link)
        {
            NavLink l = link;
            l.id = m_nextId++;
            m_links[l.id] = l;
            return l.id;
        }

        void RemoveLink(uint32_t linkId) { m_links.erase(linkId); }

        void EnableLink(uint32_t linkId, bool enabled)
        {
            auto it = m_links.find(linkId);
            if (it != m_links.end())
                it->second.enabled = enabled;
        }

        const NavLink* FindLink(uint32_t linkId) const
        {
            auto it = m_links.find(linkId);
            return it != m_links.end() ? &it->second : nullptr;
        }

        /**
         * @brief Find all enabled links within radius of a position.
         */
        std::vector<const NavLink*> FindLinksNear(const XMFLOAT3& pos, float searchRadius) const
        {
            std::vector<const NavLink*> result;
            float r2 = searchRadius * searchRadius;
            for (const auto& [id, link] : m_links)
            {
                if (!link.enabled)
                    continue;
                float dx = link.startPos.x - pos.x;
                float dy = link.startPos.y - pos.y;
                float dz = link.startPos.z - pos.z;
                if (dx * dx + dy * dy + dz * dz <= r2)
                    result.push_back(&link);
                if (link.bidirectional)
                {
                    dx = link.endPos.x - pos.x;
                    dy = link.endPos.y - pos.y;
                    dz = link.endPos.z - pos.z;
                    if (dx * dx + dy * dy + dz * dz <= r2)
                        result.push_back(&link);
                }
            }
            return result;
        }

        uint32_t GetLinkCount() const { return static_cast<uint32_t>(m_links.size()); }

      private:
        NavMeshLinkSystem() = default;
        std::unordered_map<uint32_t, NavLink> m_links;
        uint32_t m_nextId = 1;
    };

} // namespace Spark::AI
