/**
 * @file NavMeshObstacles.cpp
 * @brief Implementation of dynamic NavMesh obstacle carving
 * @author Spark Engine Team
 * @date 2026
 */

#include "NavMeshObstacles.h"
#include "../../Utils/Validate.h"

#include <cmath>
#include <sstream>


namespace Spark::AI
{

    // =========================================================================
    // Singleton
    // =========================================================================

    NavMeshObstacleManager& NavMeshObstacleManager::GetInstance()
    {
        static NavMeshObstacleManager instance;
        return instance;
    }

    // =========================================================================
    // NavMesh attachment
    // =========================================================================

    void NavMeshObstacleManager::SetNavMesh(NavMeshData* navMesh)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);
        SPARK_WARN_IF_NULL(Spark::LogCategory::AI, navMesh);
        // Restore all existing carves on the old NavMesh before switching
        if (m_navMesh != nullptr)
        {
            Clear();
        }
        m_navMesh = navMesh;
    }

    NavMeshData* NavMeshObstacleManager::GetNavMesh() const
    {
        return m_navMesh;
    }

    // =========================================================================
    // Obstacle CRUD
    // =========================================================================

    ObstacleHandle NavMeshObstacleManager::AddObstacle(const ObstacleDesc& desc)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);
        if (m_navMesh == nullptr)
        {
            return InvalidObstacleHandle;
        }

        const ObstacleHandle handle = m_nextHandle++;
        ObstacleRecord& record = m_obstacles[handle];
        record.desc = desc;
        record.dirty = false;

        CarveObstacle(record);

        return handle;
    }

    bool NavMeshObstacleManager::UpdateObstacle(ObstacleHandle handle, const ObstacleDesc& desc)
    {
        auto it = m_obstacles.find(handle);
        if (it == m_obstacles.end())
        {
            return false;
        }

        // Restore previously carved triangles, then re-carve at new position
        RestoreObstacle(it->second);
        it->second.affectedTriangles.clear();
        it->second.desc = desc;
        it->second.dirty = false;

        CarveObstacle(it->second);

        return true;
    }

    bool NavMeshObstacleManager::RemoveObstacle(ObstacleHandle handle)
    {
        auto it = m_obstacles.find(handle);
        if (it == m_obstacles.end())
        {
            return false;
        }

        RestoreObstacle(it->second);
        m_obstacles.erase(it);

        return true;
    }

    bool NavMeshObstacleManager::MarkObstacleDirty(ObstacleHandle handle, const ObstacleDesc& desc)
    {
        auto it = m_obstacles.find(handle);
        if (it == m_obstacles.end())
        {
            return false;
        }

        it->second.desc = desc;
        it->second.dirty = true;

        return true;
    }

    void NavMeshObstacleManager::ApplyDirtyObstacles()
    {
        if (m_navMesh == nullptr)
        {
            return;
        }

        for (auto& [handle, record] : m_obstacles)
        {
            if (!record.dirty)
            {
                continue;
            }

            // Restore old carve, then re-carve at the new position
            RestoreObstacle(record);
            record.affectedTriangles.clear();
            CarveObstacle(record);
            record.dirty = false;
        }
    }

    // =========================================================================
    // Query
    // =========================================================================

    bool NavMeshObstacleManager::IsTriangleCarved(uint32_t triangleIndex) const
    {
        for (const auto& [handle, record] : m_obstacles)
        {
            if (record.affectedTriangles.count(triangleIndex) > 0)
            {
                return true;
            }
        }
        return false;
    }

    size_t NavMeshObstacleManager::GetObstacleCount() const
    {
        return m_obstacles.size();
    }

    bool NavMeshObstacleManager::GetObstacleDesc(ObstacleHandle handle, ObstacleDesc& outDesc) const
    {
        auto it = m_obstacles.find(handle);
        if (it == m_obstacles.end())
        {
            return false;
        }

        outDesc = it->second.desc;
        return true;
    }

    void NavMeshObstacleManager::Clear()
    {
        SPARK_LOG_INFO(Spark::LogCategory::AI, "NavMeshObstacleManager::Clear — removing %zu obstacles.",
                       m_obstacles.size());
        if (m_navMesh != nullptr)
        {
            // Restore all carved triangles
            for (const auto& [handle, record] : m_obstacles)
            {
                for (uint32_t triIdx : record.affectedTriangles)
                {
                    if (triIdx < static_cast<uint32_t>(m_navMesh->triangles.size()))
                    {
                        m_navMesh->triangles[triIdx].flags |= WalkableFlag;
                    }
                }
            }
        }
        m_obstacles.clear();
    }

    // =========================================================================
    // Console integration
    // =========================================================================

    std::string NavMeshObstacleManager::Console_ListObstacles() const
    {
        if (m_obstacles.empty())
        {
            return "No active obstacles.\n";
        }

        std::ostringstream oss;
        oss << "Active obstacles: " << m_obstacles.size() << "\n";

        for (const auto& [handle, record] : m_obstacles)
        {
            const auto& desc = record.desc;
            oss << "  Handle " << handle << ": ";

            if (desc.shape == ObstacleShape::Box)
            {
                oss << "Box at (" << desc.position.x << ", " << desc.position.y << ", " << desc.position.z << ")"
                    << " halfExtents=(" << desc.halfExtents.x << ", " << desc.halfExtents.y << ", "
                    << desc.halfExtents.z << ")";
            }
            else
            {
                oss << "Cylinder at (" << desc.position.x << ", " << desc.position.y << ", " << desc.position.z << ")"
                    << " r=" << desc.radius << " h=" << desc.height;
            }

            oss << " affected=" << record.affectedTriangles.size() << " tris";
            if (record.dirty)
            {
                oss << " [DIRTY]";
            }
            oss << "\n";
        }

        return oss.str();
    }

    // =========================================================================
    // Carving implementation
    // =========================================================================

    void NavMeshObstacleManager::CarveObstacle(ObstacleRecord& record)
    {
        if (m_navMesh == nullptr)
        {
            return;
        }

        const auto triangleCount = static_cast<uint32_t>(m_navMesh->triangles.size());

        for (uint32_t i = 0; i < triangleCount; ++i)
        {
            NavTriangle& tri = m_navMesh->triangles[i];

            if (TriangleOverlapsObstacle(tri, record.desc, *m_navMesh))
            {
                record.affectedTriangles.insert(i);
                // Clear walkable flag (bit 0)
                tri.flags &= static_cast<uint16_t>(~WalkableFlag);
            }
        }
    }

    void NavMeshObstacleManager::RestoreObstacle(const ObstacleRecord& record)
    {
        if (m_navMesh == nullptr)
        {
            return;
        }

        // Determine the handle of this record so we can exclude it from overlap checks.
        // Find the handle by scanning the map (the record address is unique).
        ObstacleHandle thisHandle = InvalidObstacleHandle;
        for (const auto& [h, r] : m_obstacles)
        {
            if (&r == &record)
            {
                thisHandle = h;
                break;
            }
        }

        for (uint32_t triIdx : record.affectedTriangles)
        {
            if (triIdx >= static_cast<uint32_t>(m_navMesh->triangles.size()))
            {
                continue;
            }

            // Only restore if no other obstacle still covers this triangle
            if (!IsTriangleCoveredByOther(triIdx, thisHandle))
            {
                m_navMesh->triangles[triIdx].flags |= WalkableFlag;
            }
        }
    }

    // =========================================================================
    // Geometry tests
    // =========================================================================

    bool NavMeshObstacleManager::PointInBox(const XMFLOAT3& point, const ObstacleDesc& desc)
    {
        const float mx = desc.halfExtents.x + desc.margin;
        const float my = desc.halfExtents.y + desc.margin;
        const float mz = desc.halfExtents.z + desc.margin;

        const float dx = point.x - desc.position.x;
        const float dy = point.y - desc.position.y;
        const float dz = point.z - desc.position.z;

        return (std::abs(dx) <= mx) && (std::abs(dy) <= my) && (std::abs(dz) <= mz);
    }

    bool NavMeshObstacleManager::PointInCylinder(const XMFLOAT3& point, const ObstacleDesc& desc)
    {
        const float effectiveRadius = desc.radius + desc.margin;
        const float effectiveHalfHeight = (desc.height * 0.5f) + desc.margin;

        // Vertical check
        const float dy = point.y - desc.position.y;
        if (std::abs(dy) > effectiveHalfHeight)
        {
            return false;
        }

        // Horizontal distance check (XZ plane)
        const float dx = point.x - desc.position.x;
        const float dz = point.z - desc.position.z;
        const float distSq = dx * dx + dz * dz;

        return distSq <= (effectiveRadius * effectiveRadius);
    }

    bool NavMeshObstacleManager::TriangleOverlapsObstacle(const NavTriangle& tri, const ObstacleDesc& desc,
                                                          const NavMeshData& data)
    {
        // Test centroid first (fast rejection for most triangles)
        const bool centroidInside =
            (desc.shape == ObstacleShape::Box) ? PointInBox(tri.centroid, desc) : PointInCylinder(tri.centroid, desc);
        if (centroidInside)
        {
            return true;
        }

        // Test all three vertices
        for (int v = 0; v < 3; ++v)
        {
            const uint32_t vertIdx = tri.indices[v];
            if (vertIdx >= static_cast<uint32_t>(data.vertices.size()))
            {
                continue;
            }

            const XMFLOAT3& vertPos = data.vertices[vertIdx].position;
            const bool inside =
                (desc.shape == ObstacleShape::Box) ? PointInBox(vertPos, desc) : PointInCylinder(vertPos, desc);
            if (inside)
            {
                return true;
            }
        }

        // Test edge midpoints for better coverage on large triangles
        for (int e = 0; e < 3; ++e)
        {
            const uint32_t idx0 = tri.indices[e];
            const uint32_t idx1 = tri.indices[(e + 1) % 3];

            if (idx0 >= static_cast<uint32_t>(data.vertices.size()) ||
                idx1 >= static_cast<uint32_t>(data.vertices.size()))
            {
                continue;
            }

            const XMFLOAT3& p0 = data.vertices[idx0].position;
            const XMFLOAT3& p1 = data.vertices[idx1].position;

            XMFLOAT3 midpoint;
            midpoint.x = (p0.x + p1.x) * 0.5f;
            midpoint.y = (p0.y + p1.y) * 0.5f;
            midpoint.z = (p0.z + p1.z) * 0.5f;

            const bool inside =
                (desc.shape == ObstacleShape::Box) ? PointInBox(midpoint, desc) : PointInCylinder(midpoint, desc);
            if (inside)
            {
                return true;
            }
        }

        return false;
    }

    bool NavMeshObstacleManager::IsTriangleCoveredByOther(uint32_t triangleIndex, ObstacleHandle excludeHandle) const
    {
        for (const auto& [handle, record] : m_obstacles)
        {
            if (handle == excludeHandle)
            {
                continue;
            }

            if (record.affectedTriangles.count(triangleIndex) > 0)
            {
                return true;
            }
        }

        return false;
    }

} // namespace Spark::AI
