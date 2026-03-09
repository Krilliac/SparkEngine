/**
 * @file NavMesh.cpp
 * @brief NavMesh implementation — query, builder, and manager
 */

#include "NavMesh.h"
#include <sstream>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <random>

using namespace DirectX;
namespace Spark::AI
{

    // ============================================================================
    // NavMeshQuery
    // ============================================================================

    NavMeshQuery::NavMeshQuery(const NavMeshData* navMesh) : m_navMesh(navMesh) {}

    NavMeshHit NavMeshQuery::FindNearestPoint(const XMFLOAT3& position, float searchRadius) const
    {
        NavMeshHit hit{};
        if (!m_navMesh || m_navMesh->triangles.empty())
            return hit;

        float bestDistSq = searchRadius * searchRadius;

        for (uint32_t i = 0; i < static_cast<uint32_t>(m_navMesh->triangles.size()); ++i)
        {
            const auto& tri = m_navMesh->triangles[i];
            XMFLOAT3 projected = ProjectPointToTriangle(position, i);

            float dx = projected.x - position.x;
            float dy = projected.y - position.y;
            float dz = projected.z - position.z;
            float distSq = dx * dx + dy * dy + dz * dz;

            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                hit.position = projected;
                hit.triangleIndex = i;
                hit.distance = std::sqrt(distSq);
                hit.hit = true;
            }
        }

        return hit;
    }

    PathResult NavMeshQuery::FindPath(const PathRequest& request) const
    {
        PathResult result;
        if (!m_navMesh || m_navMesh->triangles.empty())
            return result;

        uint32_t startTri = FindContainingTriangle(request.start);
        uint32_t endTri = FindContainingTriangle(request.end);

        if (startTri == UINT32_MAX || endTri == UINT32_MAX)
            return result;
        if (startTri == endTri)
        {
            result.found = true;
            result.path.push_back({request.start, startTri});
            result.path.push_back({request.end, endTri});
            return result;
        }

        // A* pathfinding on triangle graph
        struct AStarNode
        {
            uint32_t triIndex;
            float gCost;
            float fCost;
            uint32_t parent;
            bool operator>(const AStarNode& other) const { return fCost > other.fCost; }
        };

        size_t triCount = m_navMesh->triangles.size();
        std::vector<float> gCosts(triCount, 1e30f);
        std::vector<uint32_t> parents(triCount, UINT32_MAX);
        std::vector<bool> closed(triCount, false);

        std::priority_queue<AStarNode, std::vector<AStarNode>, std::greater<AStarNode>> openSet;

        gCosts[startTri] = 0.0f;
        float h = HeuristicCost(m_navMesh->triangles[startTri].centroid, m_navMesh->triangles[endTri].centroid);
        openSet.push({startTri, 0.0f, h, UINT32_MAX});

        while (!openSet.empty())
        {
            AStarNode current = openSet.top();
            openSet.pop();

            if (current.triIndex == endTri)
            {
                // Reconstruct path
                result.found = true;
                result.totalCost = gCosts[endTri];

                std::vector<uint32_t> triPath;
                uint32_t tri = endTri;
                while (tri != UINT32_MAX)
                {
                    triPath.push_back(tri);
                    tri = parents[tri];
                }
                std::reverse(triPath.begin(), triPath.end());

                result.path.push_back({request.start, startTri});
                for (size_t i = 1; i + 1 < triPath.size(); ++i)
                {
                    result.path.push_back({m_navMesh->triangles[triPath[i]].centroid, triPath[i]});
                }
                result.path.push_back({request.end, endTri});
                return result;
            }

            if (closed[current.triIndex])
                continue;
            closed[current.triIndex] = true;

            const auto& tri = m_navMesh->triangles[current.triIndex];
            for (int e = 0; e < 3; ++e)
            {
                uint32_t neighbor = tri.neighborTriangles[e];
                if (neighbor == UINT32_MAX || closed[neighbor])
                    continue;

                // Check flags
                if ((m_navMesh->triangles[neighbor].flags & request.includeFlags) == 0)
                    continue;
                if ((m_navMesh->triangles[neighbor].flags & request.excludeFlags) != 0)
                    continue;

                float edgeCost = HeuristicCost(tri.centroid, m_navMesh->triangles[neighbor].centroid);
                float newG = gCosts[current.triIndex] + edgeCost;

                if (newG < gCosts[neighbor])
                {
                    gCosts[neighbor] = newG;
                    parents[neighbor] = current.triIndex;
                    float newH =
                        HeuristicCost(m_navMesh->triangles[neighbor].centroid, m_navMesh->triangles[endTri].centroid);
                    openSet.push({neighbor, newG, newG + newH, current.triIndex});
                }
            }
        }

        return result;
    }

    NavMeshHit NavMeshQuery::Raycast(const XMFLOAT3& start, const XMFLOAT3& end) const
    {
        NavMeshHit hit{};
        if (!m_navMesh)
            return hit;

        // Simple raycast through triangle centroids
        uint32_t startTri = FindContainingTriangle(start);
        if (startTri == UINT32_MAX)
            return hit;

        hit.position = end;
        hit.hit = IsPointOnNavMesh(end);
        return hit;
    }

    bool NavMeshQuery::IsPointOnNavMesh(const XMFLOAT3& point, float tolerance) const
    {
        return FindContainingTriangle(point) != UINT32_MAX;
    }

    XMFLOAT3 NavMeshQuery::GetRandomPoint() const
    {
        if (!m_navMesh || m_navMesh->triangles.empty())
            return {0, 0, 0};

        uint32_t idx = static_cast<uint32_t>(rand() % m_navMesh->triangles.size());
        return m_navMesh->triangles[idx].centroid;
    }

    XMFLOAT3 NavMeshQuery::GetRandomPointInCircle(const XMFLOAT3& center, float radius) const
    {
        if (!m_navMesh || m_navMesh->triangles.empty())
            return center;

        float radiusSq = radius * radius;
        // Try random triangles until we find one within radius
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            XMFLOAT3 point = GetRandomPoint();
            float dx = point.x - center.x;
            float dz = point.z - center.z;
            if (dx * dx + dz * dz <= radiusSq)
                return point;
        }
        return center;
    }

    uint32_t NavMeshQuery::FindContainingTriangle(const XMFLOAT3& point) const
    {
        if (!m_navMesh)
            return UINT32_MAX;

        float bestDistSq = 1e30f;
        uint32_t bestTri = UINT32_MAX;

        for (uint32_t i = 0; i < static_cast<uint32_t>(m_navMesh->triangles.size()); ++i)
        {
            const auto& tri = m_navMesh->triangles[i];
            float dx = tri.centroid.x - point.x;
            float dz = tri.centroid.z - point.z;
            float distSq = dx * dx + dz * dz;

            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                bestTri = i;
            }
        }

        // Check if point is close enough to be considered "on" the navmesh
        if (bestDistSq < m_navMesh->cellSize * m_navMesh->cellSize * 4.0f)
            return bestTri;

        return UINT32_MAX;
    }

    float NavMeshQuery::HeuristicCost(const XMFLOAT3& a, const XMFLOAT3& b) const
    {
        float dx = b.x - a.x;
        float dy = b.y - a.y;
        float dz = b.z - a.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    XMFLOAT3 NavMeshQuery::ProjectPointToTriangle(const XMFLOAT3& point, uint32_t triIndex) const
    {
        if (!m_navMesh || triIndex >= static_cast<uint32_t>(m_navMesh->triangles.size()))
            return point;
        const auto& tri = m_navMesh->triangles[triIndex];
        // Simplified — project to centroid
        return tri.centroid;
    }

    // ============================================================================
    // NavMeshBuilder
    // ============================================================================

    std::unique_ptr<NavMeshData> NavMeshBuilder::Build(const std::vector<XMFLOAT3>& vertices,
                                                       const std::vector<uint32_t>& indices,
                                                       const NavMeshBuildSettings& settings)
    {

        auto navMesh = std::make_unique<NavMeshData>();
        navMesh->cellSize = settings.cellSize;
        navMesh->cellHeight = settings.cellHeight;
        navMesh->agentHeight = settings.agentHeight;
        navMesh->agentRadius = settings.agentRadius;
        navMesh->agentMaxClimb = settings.agentMaxClimb;
        navMesh->agentMaxSlope = settings.agentMaxSlope;

        if (indices.size() < 3 || vertices.empty())
            return navMesh;

        // Convert input geometry to navmesh triangles
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            const auto& v0 = vertices[indices[i]];
            const auto& v1 = vertices[indices[i + 1]];
            const auto& v2 = vertices[indices[i + 2]];

            // Check slope — reject triangles too steep for the agent
            XMVECTOR e1 = XMVectorSubtract(XMLoadFloat3(&v1), XMLoadFloat3(&v0));
            XMVECTOR e2 = XMVectorSubtract(XMLoadFloat3(&v2), XMLoadFloat3(&v0));
            XMVECTOR normal = XMVector3Normalize(XMVector3Cross(e1, e2));

            XMFLOAT3 n;
            XMStoreFloat3(&n, normal);
            float slopeAngle = std::acos(std::abs(n.y)) * (180.0f / 3.14159265f);

            if (slopeAngle > settings.agentMaxSlope)
                continue;

            NavVertex nv0{v0}, nv1{v1}, nv2{v2};
            uint32_t baseIdx = static_cast<uint32_t>(navMesh->vertices.size());
            navMesh->vertices.push_back(nv0);
            navMesh->vertices.push_back(nv1);
            navMesh->vertices.push_back(nv2);

            NavTriangle tri{};
            tri.indices[0] = baseIdx;
            tri.indices[1] = baseIdx + 1;
            tri.indices[2] = baseIdx + 2;
            tri.neighborTriangles[0] = UINT32_MAX;
            tri.neighborTriangles[1] = UINT32_MAX;
            tri.neighborTriangles[2] = UINT32_MAX;
            tri.centroid.x = (v0.x + v1.x + v2.x) / 3.0f;
            tri.centroid.y = (v0.y + v1.y + v2.y) / 3.0f;
            tri.centroid.z = (v0.z + v1.z + v2.z) / 3.0f;
            tri.flags = 0xFFFF; // All walkable by default

            // Compute area via cross product
            XMFLOAT3 cross;
            XMStoreFloat3(&cross, XMVector3Cross(e1, e2));
            tri.area = 0.5f * std::sqrt(cross.x * cross.x + cross.y * cross.y + cross.z * cross.z);

            navMesh->triangles.push_back(tri);
        }

        // Build adjacency (simple O(n^2) — fine for offline bake)
        for (size_t i = 0; i < navMesh->triangles.size(); ++i)
        {
            for (size_t j = i + 1; j < navMesh->triangles.size(); ++j)
            {
                // Count shared vertices
                int shared = 0;
                int sharedEdgeI = -1, sharedEdgeJ = -1;
                for (int ei = 0; ei < 3; ++ei)
                {
                    for (int ej = 0; ej < 3; ++ej)
                    {
                        const auto& vi = navMesh->vertices[navMesh->triangles[i].indices[ei]].position;
                        const auto& vj = navMesh->vertices[navMesh->triangles[j].indices[ej]].position;
                        float dx = vi.x - vj.x, dy = vi.y - vj.y, dz = vi.z - vj.z;
                        if (dx * dx + dy * dy + dz * dz < 0.001f)
                        {
                            shared++;
                            sharedEdgeI = ei;
                            sharedEdgeJ = ej;
                        }
                    }
                }
                if (shared >= 2)
                {
                    // Find first unset neighbor slot
                    for (int e = 0; e < 3; ++e)
                    {
                        if (navMesh->triangles[i].neighborTriangles[e] == UINT32_MAX)
                        {
                            navMesh->triangles[i].neighborTriangles[e] = static_cast<uint32_t>(j);
                            break;
                        }
                    }
                    for (int e = 0; e < 3; ++e)
                    {
                        if (navMesh->triangles[j].neighborTriangles[e] == UINT32_MAX)
                        {
                            navMesh->triangles[j].neighborTriangles[e] = static_cast<uint32_t>(i);
                            break;
                        }
                    }
                }
            }
        }

        // Compute bounds
        if (!navMesh->vertices.empty())
        {
            navMesh->boundsMin = navMesh->vertices[0].position;
            navMesh->boundsMax = navMesh->vertices[0].position;
            for (const auto& v : navMesh->vertices)
            {
                navMesh->boundsMin.x = (std::min)(navMesh->boundsMin.x, v.position.x);
                navMesh->boundsMin.y = (std::min)(navMesh->boundsMin.y, v.position.y);
                navMesh->boundsMin.z = (std::min)(navMesh->boundsMin.z, v.position.z);
                navMesh->boundsMax.x = (std::max)(navMesh->boundsMax.x, v.position.x);
                navMesh->boundsMax.y = (std::max)(navMesh->boundsMax.y, v.position.y);
                navMesh->boundsMax.z = (std::max)(navMesh->boundsMax.z, v.position.z);
            }
        }

        return navMesh;
    }

    std::unique_ptr<NavMeshData> NavMeshBuilder::BuildFromHeightfield(const float* heightData, int width, int height,
                                                                      const XMFLOAT3& origin, float cellSize,
                                                                      const NavMeshBuildSettings& settings)
    {

        // Convert heightfield to triangle soup, then build
        std::vector<XMFLOAT3> vertices;
        std::vector<uint32_t> indices;

        vertices.reserve(static_cast<size_t>(width) * height);
        for (int z = 0; z < height; ++z)
        {
            for (int x = 0; x < width; ++x)
            {
                float h = heightData[z * width + x];
                vertices.push_back({origin.x + x * cellSize, origin.y + h, origin.z + z * cellSize});
            }
        }

        for (int z = 0; z < height - 1; ++z)
        {
            for (int x = 0; x < width - 1; ++x)
            {
                uint32_t tl = static_cast<uint32_t>(z * width + x);
                uint32_t tr = tl + 1;
                uint32_t bl = static_cast<uint32_t>((z + 1) * width + x);
                uint32_t br = bl + 1;

                indices.push_back(tl);
                indices.push_back(bl);
                indices.push_back(tr);
                indices.push_back(tr);
                indices.push_back(bl);
                indices.push_back(br);
            }
        }

        return Build(vertices, indices, settings);
    }

    // ============================================================================
    // NavMeshManager
    // ============================================================================

    NavMeshManager& NavMeshManager::GetInstance()
    {
        static NavMeshManager instance;
        return instance;
    }

    bool NavMeshManager::LoadNavMesh(const std::string& name, const std::string& filepath)
    {
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open())
            return false;

        // Binary format: magic(4) + version(4) + settings + vertex count + vertices + triangle count + triangles + adjacency
        char magic[4];
        file.read(magic, 4);
        if (std::string(magic, 4) != "SNAV")
        {
            // Not a SparkEngine navmesh file — return false
            return false;
        }

        uint32_t version;
        file.read(reinterpret_cast<char*>(&version), 4);
        if (version > 1)
            return false;

        auto navMesh = std::make_unique<NavMeshData>();

        // Read settings
        file.read(reinterpret_cast<char*>(&navMesh->cellSize), sizeof(float));
        file.read(reinterpret_cast<char*>(&navMesh->agentHeight), sizeof(float));
        file.read(reinterpret_cast<char*>(&navMesh->agentRadius), sizeof(float));
        file.read(reinterpret_cast<char*>(&navMesh->boundsMin), sizeof(XMFLOAT3));
        file.read(reinterpret_cast<char*>(&navMesh->boundsMax), sizeof(XMFLOAT3));

        // Read vertices
        uint32_t vertexCount;
        file.read(reinterpret_cast<char*>(&vertexCount), 4);
        navMesh->vertices.resize(vertexCount);
        file.read(reinterpret_cast<char*>(navMesh->vertices.data()), vertexCount * sizeof(XMFLOAT3));

        // Read triangles
        uint32_t triangleCount;
        file.read(reinterpret_cast<char*>(&triangleCount), 4);
        navMesh->triangles.resize(triangleCount);
        for (uint32_t i = 0; i < triangleCount; ++i)
        {
            file.read(reinterpret_cast<char*>(&navMesh->triangles[i].indices), sizeof(uint32_t) * 3);
            file.read(reinterpret_cast<char*>(&navMesh->triangles[i].centroid), sizeof(XMFLOAT3));
            file.read(reinterpret_cast<char*>(&navMesh->triangles[i].normal), sizeof(XMFLOAT3));
            file.read(reinterpret_cast<char*>(&navMesh->triangles[i].area), sizeof(float));
            uint32_t adjCount;
            file.read(reinterpret_cast<char*>(&adjCount), 4);
            navMesh->triangles[i].adjacency.resize(adjCount);
            if (adjCount > 0)
            {
                file.read(reinterpret_cast<char*>(navMesh->triangles[i].adjacency.data()), adjCount * sizeof(uint32_t));
            }
        }

        if (!file.good())
            return false;

        m_navMeshes[name] = std::move(navMesh);
        return true;
    }

    bool NavMeshManager::BuildNavMesh(const std::string& name, const std::vector<XMFLOAT3>& vertices,
                                      const std::vector<uint32_t>& indices, const NavMeshBuildSettings& settings)
    {
        auto navMesh = NavMeshBuilder::Build(vertices, indices, settings);
        if (!navMesh)
            return false;
        m_navMeshes[name] = std::move(navMesh);
        return true;
    }

    const NavMeshData* NavMeshManager::GetNavMesh(const std::string& name) const
    {
        auto it = m_navMeshes.find(name);
        return (it != m_navMeshes.end()) ? it->second.get() : nullptr;
    }

    std::unique_ptr<NavMeshQuery> NavMeshManager::CreateQuery(const std::string& name) const
    {
        const NavMeshData* nm = GetNavMesh(name);
        if (!nm)
            return nullptr;
        return std::make_unique<NavMeshQuery>(nm);
    }

    void NavMeshManager::RemoveNavMesh(const std::string& name)
    {
        m_navMeshes.erase(name);
    }

    void NavMeshManager::Clear()
    {
        m_navMeshes.clear();
    }

    std::string NavMeshManager::Console_ListNavMeshes() const
    {
        std::ostringstream ss;
        ss << "=== NavMeshes (" << m_navMeshes.size() << ") ===\n";
        for (const auto& [name, nm] : m_navMeshes)
        {
            ss << "  " << name << " [" << nm->triangles.size() << " triangles, " << nm->vertices.size()
               << " vertices]\n";
        }
        return ss.str();
    }

    std::string NavMeshManager::Console_GetNavMeshInfo(const std::string& name) const
    {
        auto it = m_navMeshes.find(name);
        if (it == m_navMeshes.end())
            return "NavMesh '" + name + "' not found\n";

        const auto& nm = it->second;
        std::ostringstream ss;
        ss << "=== NavMesh: " << name << " ===\n";
        ss << "Triangles: " << nm->triangles.size() << "\n";
        ss << "Vertices: " << nm->vertices.size() << "\n";
        ss << "Cell Size: " << nm->cellSize << "\n";
        ss << "Agent Height: " << nm->agentHeight << "\n";
        ss << "Agent Radius: " << nm->agentRadius << "\n";
        ss << "Bounds: (" << nm->boundsMin.x << "," << nm->boundsMin.y << "," << nm->boundsMin.z << ") to ("
           << nm->boundsMax.x << "," << nm->boundsMax.y << "," << nm->boundsMax.z << ")\n";
        return ss.str();
    }

} // namespace Spark::AI
