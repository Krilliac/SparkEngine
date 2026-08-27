// TestNavMesh.cpp - Tests for navigation mesh system
// Standalone implementations for CI testing (no DirectXMath dependency)

#include "TestFramework.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <queue>
#include <limits>

namespace TestNav
{

    // ============================================================================
    // Minimal math types for cross-platform testing
    // ============================================================================

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        Vec3() = default;
        Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }

        float Length() const { return std::sqrt(x * x + y * y + z * z); }
        float LengthSq() const { return x * x + y * y + z * z; }

        float DistanceTo(const Vec3& o) const { return (*this - o).Length(); }

        bool Near(const Vec3& o, float tol = 0.5f) const { return DistanceTo(o) <= tol; }
    };

    float Dot(const Vec3& a, const Vec3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    // ============================================================================
    // NavMesh Data Structures (mirrors Spark::AI)
    // ============================================================================

    struct NavVertex
    {
        Vec3 position;
    };

    struct NavTriangle
    {
        uint32_t indices[3];
        Vec3 centroid;
        Vec3 normal;
        float area;
        uint16_t flags;
        std::vector<uint32_t> adjacency; ///< Indices of adjacent triangles
    };

    struct NavMeshData
    {
        std::vector<NavVertex> vertices;
        std::vector<NavTriangle> triangles;
        Vec3 boundsMin{0, 0, 0};
        Vec3 boundsMax{0, 0, 0};
        float cellSize = 0.3f;
        float cellHeight = 0.2f;
        float agentHeight = 2.0f;
        float agentRadius = 0.6f;
        float agentMaxClimb = 0.9f;
        float agentMaxSlope = 45.0f;
    };

    struct PathPoint
    {
        Vec3 position;
        uint32_t triangleIndex;
    };

    struct PathResult
    {
        bool found = false;
        std::vector<PathPoint> path;
        float totalCost = 0.0f;
    };

    struct NavMeshHit
    {
        Vec3 position;
        Vec3 normal;
        uint32_t triangleIndex;
        float distance;
        bool hit = false;
    };

    // ============================================================================
    // NavMesh Build Settings
    // ============================================================================

    struct NavMeshBuildSettings
    {
        float cellSize = 0.3f;
        float cellHeight = 0.2f;
        float agentHeight = 2.0f;
        float agentRadius = 0.6f;
        float agentMaxClimb = 0.9f;
        float agentMaxSlope = 45.0f;
    };

    // ============================================================================
    // NavMeshBuilder (simplified for testing)
    // ============================================================================

    class NavMeshBuilder
    {
      public:
        /// Build navmesh from triangle soup
        /// For testing, this directly converts the input triangles into a navmesh
        static std::unique_ptr<NavMeshData> Build(const std::vector<Vec3>& vertices,
                                                  const std::vector<uint32_t>& indices,
                                                  const NavMeshBuildSettings& settings)
        {
            if (indices.size() % 3 != 0 || vertices.empty())
                return nullptr;

            auto navMesh = std::make_unique<NavMeshData>();
            navMesh->cellSize = settings.cellSize;
            navMesh->agentHeight = settings.agentHeight;
            navMesh->agentRadius = settings.agentRadius;

            // Add vertices
            for (const auto& v : vertices)
            {
                navMesh->vertices.push_back({v});
            }

            // Compute bounds
            navMesh->boundsMin = vertices[0];
            navMesh->boundsMax = vertices[0];
            for (const auto& v : vertices)
            {
                navMesh->boundsMin.x = std::min(navMesh->boundsMin.x, v.x);
                navMesh->boundsMin.y = std::min(navMesh->boundsMin.y, v.y);
                navMesh->boundsMin.z = std::min(navMesh->boundsMin.z, v.z);
                navMesh->boundsMax.x = std::max(navMesh->boundsMax.x, v.x);
                navMesh->boundsMax.y = std::max(navMesh->boundsMax.y, v.y);
                navMesh->boundsMax.z = std::max(navMesh->boundsMax.z, v.z);
            }

            // Build triangles
            size_t triCount = indices.size() / 3;
            for (size_t i = 0; i < triCount; ++i)
            {
                NavTriangle tri;
                tri.indices[0] = indices[i * 3 + 0];
                tri.indices[1] = indices[i * 3 + 1];
                tri.indices[2] = indices[i * 3 + 2];

                Vec3 v0 = vertices[tri.indices[0]];
                Vec3 v1 = vertices[tri.indices[1]];
                Vec3 v2 = vertices[tri.indices[2]];

                // Centroid
                tri.centroid = {(v0.x + v1.x + v2.x) / 3.0f, (v0.y + v1.y + v2.y) / 3.0f, (v0.z + v1.z + v2.z) / 3.0f};

                // Normal
                Vec3 edge1 = v1 - v0;
                Vec3 edge2 = v2 - v0;
                Vec3 n = Cross(edge1, edge2);
                float len = n.Length();
                tri.normal = (len > 0.0001f) ? Vec3{n.x / len, n.y / len, n.z / len} : Vec3{0, 1, 0};

                // Area
                tri.area = len * 0.5f;
                tri.flags = 0xFFFF; // All walkable by default

                navMesh->triangles.push_back(tri);
            }

            // Build adjacency: two triangles are adjacent if they share an edge (2 vertices)
            for (size_t i = 0; i < navMesh->triangles.size(); ++i)
            {
                for (size_t j = i + 1; j < navMesh->triangles.size(); ++j)
                {
                    int sharedCount = 0;
                    for (int a = 0; a < 3; ++a)
                    {
                        for (int b = 0; b < 3; ++b)
                        {
                            if (navMesh->triangles[i].indices[a] == navMesh->triangles[j].indices[b])
                                sharedCount++;
                        }
                    }
                    if (sharedCount >= 2)
                    {
                        navMesh->triangles[i].adjacency.push_back(static_cast<uint32_t>(j));
                        navMesh->triangles[j].adjacency.push_back(static_cast<uint32_t>(i));
                    }
                }
            }

            return navMesh;
        }
    };

    // ============================================================================
    // NavMeshQuery (simplified for testing)
    // ============================================================================

    class NavMeshQuery
    {
      public:
        explicit NavMeshQuery(const NavMeshData* navMesh) : m_navMesh(navMesh) {}

        /// Find the nearest point on the navmesh to a given position
        NavMeshHit FindNearestPoint(const Vec3& position, float searchRadius = 10.0f) const
        {
            NavMeshHit result;
            result.hit = false;
            float bestDist = searchRadius;

            for (size_t i = 0; i < m_navMesh->triangles.size(); ++i)
            {
                const NavTriangle& tri = m_navMesh->triangles[i];
                // Use centroid as the projected point (simplified)
                Vec3 projected = ProjectPointToTriangle(position, static_cast<uint32_t>(i));
                float dist = position.DistanceTo(projected);

                if (dist < bestDist)
                {
                    bestDist = dist;
                    result.position = projected;
                    result.normal = tri.normal;
                    result.triangleIndex = static_cast<uint32_t>(i);
                    result.distance = dist;
                    result.hit = true;
                }
            }

            return result;
        }

        /// Check if a point is on (or near) the navmesh
        bool IsPointOnNavMesh(const Vec3& point, float tolerance = 0.5f) const
        {
            for (size_t i = 0; i < m_navMesh->triangles.size(); ++i)
            {
                if (IsPointInTriangle(point, static_cast<uint32_t>(i), tolerance))
                    return true;
            }
            return false;
        }

        /// Find path using A* on triangle adjacency graph
        PathResult FindPath(const Vec3& start, const Vec3& end) const
        {
            PathResult result;
            result.found = false;

            if (m_navMesh->triangles.empty())
                return result;

            // Find start and end triangles
            uint32_t startTri = FindContainingTriangle(start);
            uint32_t endTri = FindContainingTriangle(end);

            if (startTri == UINT32_MAX || endTri == UINT32_MAX)
                return result;

            if (startTri == endTri)
            {
                result.found = true;
                result.path.push_back({start, startTri});
                result.path.push_back({end, endTri});
                result.totalCost = start.DistanceTo(end);
                return result;
            }

            // A* search on triangle graph
            size_t triCount = m_navMesh->triangles.size();
            std::vector<float> gScore(triCount, std::numeric_limits<float>::max());
            std::vector<uint32_t> cameFrom(triCount, UINT32_MAX);
            std::vector<bool> closed(triCount, false);

            // Priority queue: (f-score, triangle index)
            using PQEntry = std::pair<float, uint32_t>;
            std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<PQEntry>> openSet;

            gScore[startTri] = 0.0f;
            float hStart = m_navMesh->triangles[startTri].centroid.DistanceTo(m_navMesh->triangles[endTri].centroid);
            openSet.push({hStart, startTri});

            while (!openSet.empty())
            {
                auto [fScore, current] = openSet.top();
                openSet.pop();

                if (current == endTri)
                {
                    // Reconstruct path
                    result.found = true;
                    result.totalCost = gScore[endTri];

                    std::vector<uint32_t> triPath;
                    uint32_t node = endTri;
                    while (node != UINT32_MAX)
                    {
                        triPath.push_back(node);
                        node = cameFrom[node];
                    }
                    std::reverse(triPath.begin(), triPath.end());

                    // Build path points from centroids
                    result.path.push_back({start, startTri});
                    for (size_t i = 1; i + 1 < triPath.size(); ++i)
                    {
                        result.path.push_back({m_navMesh->triangles[triPath[i]].centroid, triPath[i]});
                    }
                    result.path.push_back({end, endTri});
                    return result;
                }

                if (closed[current])
                    continue;
                closed[current] = true;

                for (uint32_t neighbor : m_navMesh->triangles[current].adjacency)
                {
                    if (closed[neighbor])
                        continue;

                    float tentG = gScore[current] + m_navMesh->triangles[current].centroid.DistanceTo(
                                                        m_navMesh->triangles[neighbor].centroid);

                    if (tentG < gScore[neighbor])
                    {
                        gScore[neighbor] = tentG;
                        cameFrom[neighbor] = current;
                        float h =
                            m_navMesh->triangles[neighbor].centroid.DistanceTo(m_navMesh->triangles[endTri].centroid);
                        openSet.push({tentG + h, neighbor});
                    }
                }
            }

            return result; // Not found
        }

      private:
        const NavMeshData* m_navMesh;

        /// Find the triangle containing a point (2D XZ projection with tolerance)
        uint32_t FindContainingTriangle(const Vec3& point) const
        {
            float bestDist = std::numeric_limits<float>::max();
            uint32_t bestTri = UINT32_MAX;

            for (size_t i = 0; i < m_navMesh->triangles.size(); ++i)
            {
                float dist = point.DistanceTo(m_navMesh->triangles[i].centroid);
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestTri = static_cast<uint32_t>(i);
                }
            }
            return bestTri;
        }

        /// Project point onto nearest position on/near a triangle
        Vec3 ProjectPointToTriangle(const Vec3& point, uint32_t triIndex) const
        {
            const NavTriangle& tri = m_navMesh->triangles[triIndex];
            Vec3 v0 = m_navMesh->vertices[tri.indices[0]].position;
            Vec3 v1 = m_navMesh->vertices[tri.indices[1]].position;
            Vec3 v2 = m_navMesh->vertices[tri.indices[2]].position;

            // Use barycentric projection (simplified: closest point among centroid and vertices)
            Vec3 centroid = tri.centroid;
            float bestDist = point.DistanceTo(centroid);
            Vec3 best = centroid;

            for (const Vec3& v : {v0, v1, v2})
            {
                float d = point.DistanceTo(v);
                if (d < bestDist)
                {
                    bestDist = d;
                    best = v;
                }
            }
            return best;
        }

        /// Check if point is within or near a triangle (XZ plane, with Y tolerance)
        bool IsPointInTriangle(const Vec3& point, uint32_t triIndex, float tolerance) const
        {
            const NavTriangle& tri = m_navMesh->triangles[triIndex];
            Vec3 v0 = m_navMesh->vertices[tri.indices[0]].position;
            Vec3 v1 = m_navMesh->vertices[tri.indices[1]].position;
            Vec3 v2 = m_navMesh->vertices[tri.indices[2]].position;

            // Barycentric coordinate test in XZ plane
            float d00 = (v1.x - v0.x) * (v1.x - v0.x) + (v1.z - v0.z) * (v1.z - v0.z);
            float d01 = (v1.x - v0.x) * (v2.x - v0.x) + (v1.z - v0.z) * (v2.z - v0.z);
            float d11 = (v2.x - v0.x) * (v2.x - v0.x) + (v2.z - v0.z) * (v2.z - v0.z);
            float d20 = (point.x - v0.x) * (v1.x - v0.x) + (point.z - v0.z) * (v1.z - v0.z);
            float d21 = (point.x - v0.x) * (v2.x - v0.x) + (point.z - v0.z) * (v2.z - v0.z);

            float denom = d00 * d11 - d01 * d01;
            if (std::abs(denom) < 0.0001f)
                return false;

            float u = (d11 * d20 - d01 * d21) / denom;
            float v = (d00 * d21 - d01 * d20) / denom;

            // Inside triangle with tolerance
            return (u >= -tolerance) && (v >= -tolerance) && (u + v <= 1.0f + tolerance);
        }
    };

    // ============================================================================
    // NavMeshManager (simplified for testing)
    // ============================================================================

    class NavMeshManager
    {
      public:
        void RegisterNavMesh(const std::string& name, std::unique_ptr<NavMeshData> data)
        {
            m_navMeshes[name] = std::move(data);
        }

        const NavMeshData* GetNavMesh(const std::string& name) const
        {
            auto it = m_navMeshes.find(name);
            return (it != m_navMeshes.end()) ? it->second.get() : nullptr;
        }

        std::unique_ptr<NavMeshQuery> CreateQuery(const std::string& name) const
        {
            auto* data = GetNavMesh(name);
            if (!data)
                return nullptr;
            return std::make_unique<NavMeshQuery>(data);
        }

        void RemoveNavMesh(const std::string& name) { m_navMeshes.erase(name); }

        void Clear() { m_navMeshes.clear(); }

      private:
        std::unordered_map<std::string, std::unique_ptr<NavMeshData>> m_navMeshes;
    };

    // ============================================================================
    // Helper: Build a flat quad navmesh (2 triangles on XZ plane)
    // ============================================================================

    /// Creates a flat 10x10 quad on the XZ plane centered at origin:
    ///   v0(0,0,0)  v1(10,0,0)
    ///   v2(0,0,10) v3(10,0,10)
    ///   Triangle 0: v0,v1,v2    Triangle 1: v1,v3,v2
    std::unique_ptr<NavMeshData> MakeFlatQuadNavMesh()
    {
        std::vector<Vec3> verts = {{0, 0, 0}, {10, 0, 0}, {0, 0, 10}, {10, 0, 10}};
        std::vector<uint32_t> indices = {
            0, 2, 1, // triangle 0 (CCW from above for +Y normal)
            2, 3, 1  // triangle 1 (CCW from above for +Y normal)
        };
        NavMeshBuildSettings settings;
        return NavMeshBuilder::Build(verts, indices, settings);
    }

    /// Creates a 3x3 grid of 18 triangles for path testing
    ///   9 quads -> 18 triangles on the XZ plane
    std::unique_ptr<NavMeshData> MakeGridNavMesh()
    {
        const int gridSize = 4; // 4x4 vertices -> 3x3 quads -> 18 triangles
        const float spacing = 5.0f;

        std::vector<Vec3> verts;
        for (int z = 0; z < gridSize; ++z)
            for (int x = 0; x < gridSize; ++x)
                verts.push_back({x * spacing, 0.0f, z * spacing});

        std::vector<uint32_t> indices;
        for (int z = 0; z < gridSize - 1; ++z)
        {
            for (int x = 0; x < gridSize - 1; ++x)
            {
                uint32_t topLeft = z * gridSize + x;
                uint32_t topRight = topLeft + 1;
                uint32_t bottomLeft = (z + 1) * gridSize + x;
                uint32_t bottomRight = bottomLeft + 1;

                indices.push_back(topLeft);
                indices.push_back(topRight);
                indices.push_back(bottomLeft);

                indices.push_back(topRight);
                indices.push_back(bottomRight);
                indices.push_back(bottomLeft);
            }
        }

        NavMeshBuildSettings settings;
        return NavMeshBuilder::Build(verts, indices, settings);
    }

} // namespace TestNav

// ============================================================================
// Tests: NavMeshBuilder
// ============================================================================

TEST(NavMeshBuilder_BuildFromSimpleGeometry)
{
    auto navMesh = TestNav::MakeFlatQuadNavMesh();

    ASSERT_TRUE(navMesh != nullptr);
    EXPECT_EQ(navMesh->vertices.size(), 4u);
    ASSERT_EQ(navMesh->triangles.size(), 2u);

    // Check that triangles have valid indices
    for (const auto& tri : navMesh->triangles)
    {
        for (int i = 0; i < 3; ++i)
        {
            EXPECT_TRUE(tri.indices[i] < navMesh->vertices.size());
        }
    }

    // Check that normals point up (flat horizontal surface)
    for (const auto& tri : navMesh->triangles)
    {
        EXPECT_NEAR(tri.normal.y, 1.0f, 0.01f);
        EXPECT_NEAR(tri.normal.x, 0.0f, 0.01f);
        EXPECT_NEAR(tri.normal.z, 0.0f, 0.01f);
    }

    // Check that area is positive
    for (const auto& tri : navMesh->triangles)
    {
        EXPECT_TRUE(tri.area > 0.0f);
    }

    // Check that the two triangles are adjacent (they share an edge)
    EXPECT_FALSE(navMesh->triangles[0].adjacency.empty());
    EXPECT_FALSE(navMesh->triangles[1].adjacency.empty());

    // Check bounds
    EXPECT_NEAR(navMesh->boundsMin.x, 0.0f, 0.01f);
    EXPECT_NEAR(navMesh->boundsMin.z, 0.0f, 0.01f);
    EXPECT_NEAR(navMesh->boundsMax.x, 10.0f, 0.01f);
    EXPECT_NEAR(navMesh->boundsMax.z, 10.0f, 0.01f);
}

TEST(NavMeshBuilder_BuildReturnsNullForInvalidInput)
{
    TestNav::NavMeshBuildSettings settings;

    // Empty vertices
    std::vector<TestNav::Vec3> emptyVerts;
    std::vector<uint32_t> emptyIndices;
    auto result1 = TestNav::NavMeshBuilder::Build(emptyVerts, emptyIndices, settings);
    EXPECT_TRUE(result1 == nullptr);

    // Non-divisible-by-3 indices
    std::vector<TestNav::Vec3> verts = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
    std::vector<uint32_t> badIndices = {0, 1}; // Not divisible by 3
    auto result2 = TestNav::NavMeshBuilder::Build(verts, badIndices, settings);
    EXPECT_TRUE(result2 == nullptr);
}

// ============================================================================
// Tests: NavMeshQuery FindNearestPoint
// ============================================================================

TEST(NavMeshQuery_FindNearestPoint)
{
    auto navMesh = TestNav::MakeFlatQuadNavMesh();
    TestNav::NavMeshQuery query(navMesh.get());

    // Point directly on the mesh surface
    TestNav::NavMeshHit hit = query.FindNearestPoint({5, 0, 5});
    EXPECT_TRUE(hit.hit);
    EXPECT_NEAR(hit.position.y, 0.0f, 0.5f);

    // Point above the mesh
    TestNav::NavMeshHit hitAbove = query.FindNearestPoint({5, 10, 5}, 15.0f);
    EXPECT_TRUE(hitAbove.hit);
    EXPECT_TRUE(hitAbove.distance > 0.0f);

    // Point far away (beyond search radius)
    TestNav::NavMeshHit hitFar = query.FindNearestPoint({1000, 1000, 1000}, 5.0f);
    EXPECT_FALSE(hitFar.hit);
}

// ============================================================================
// Tests: NavMeshQuery FindPath
// ============================================================================

TEST(NavMeshQuery_FindPathOnSimpleGrid)
{
    auto navMesh = TestNav::MakeGridNavMesh();
    TestNav::NavMeshQuery query(navMesh.get());

    // Path from near bottom-left to near top-right of the 3x3 grid
    TestNav::Vec3 start{1, 0, 1};
    TestNav::Vec3 end{14, 0, 14};

    TestNav::PathResult result = query.FindPath(start, end);
    EXPECT_TRUE(result.found);
    EXPECT_TRUE(result.path.size() >= 2u);
    EXPECT_TRUE(result.totalCost > 0.0f);

    // Start and end of path should be near our requested points
    EXPECT_TRUE(result.path.front().position.Near(start, 2.0f));
    EXPECT_TRUE(result.path.back().position.Near(end, 2.0f));
}

TEST(NavMeshQuery_FindPathSameTriangle)
{
    auto navMesh = TestNav::MakeFlatQuadNavMesh();
    TestNav::NavMeshQuery query(navMesh.get());

    // Two points in the same triangle
    TestNav::Vec3 start{2, 0, 2};
    TestNav::Vec3 end{3, 0, 3};

    TestNav::PathResult result = query.FindPath(start, end);
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.path.size(), 2u);
}

// ============================================================================
// Tests: NavMeshQuery IsPointOnNavMesh
// ============================================================================

TEST(NavMeshQuery_IsPointOnNavMesh)
{
    auto navMesh = TestNav::MakeFlatQuadNavMesh();
    TestNav::NavMeshQuery query(navMesh.get());

    // Point on the mesh
    EXPECT_TRUE(query.IsPointOnNavMesh({5, 0, 5}));

    // Point at a corner
    EXPECT_TRUE(query.IsPointOnNavMesh({0, 0, 0}));

    // Point clearly off the mesh
    EXPECT_FALSE(query.IsPointOnNavMesh({-100, 0, -100}, 0.1f));
    EXPECT_FALSE(query.IsPointOnNavMesh({100, 0, 100}, 0.1f));
}

// ============================================================================
// Tests: NavMeshManager
// ============================================================================

TEST(NavMeshManager_RegisterAndRetrieve)
{
    TestNav::NavMeshManager mgr;

    auto navMesh = TestNav::MakeFlatQuadNavMesh();
    auto* rawPtr = navMesh.get();
    mgr.RegisterNavMesh("TestLevel", std::move(navMesh));

    const TestNav::NavMeshData* retrieved = mgr.GetNavMesh("TestLevel");
    EXPECT_TRUE(retrieved != nullptr);
    EXPECT_TRUE(retrieved == rawPtr);

    const TestNav::NavMeshData* missing = mgr.GetNavMesh("NonExistent");
    EXPECT_TRUE(missing == nullptr);
}

TEST(NavMeshManager_CreateQuery)
{
    TestNav::NavMeshManager mgr;
    mgr.RegisterNavMesh("Level1", TestNav::MakeFlatQuadNavMesh());

    auto query = mgr.CreateQuery("Level1");
    EXPECT_TRUE(query != nullptr);

    auto nullQuery = mgr.CreateQuery("MissingLevel");
    EXPECT_TRUE(nullQuery == nullptr);
}

TEST(NavMeshManager_RemoveAndClear)
{
    TestNav::NavMeshManager mgr;
    mgr.RegisterNavMesh("A", TestNav::MakeFlatQuadNavMesh());
    mgr.RegisterNavMesh("B", TestNav::MakeFlatQuadNavMesh());

    mgr.RemoveNavMesh("A");
    EXPECT_TRUE(mgr.GetNavMesh("A") == nullptr);
    EXPECT_TRUE(mgr.GetNavMesh("B") != nullptr);

    mgr.Clear();
    EXPECT_TRUE(mgr.GetNavMesh("B") == nullptr);
}

// ============================================================================
// Tests: PathResult
// ============================================================================

TEST(PathResult_DefaultIsNotFound)
{
    TestNav::PathResult result;
    EXPECT_FALSE(result.found);
    EXPECT_TRUE(result.path.empty());
    EXPECT_NEAR(result.totalCost, 0.0f, 0.001f);
}

TEST(PathResult_SimplePathProperties)
{
    TestNav::PathResult result;
    result.found = true;
    result.totalCost = 15.5f;
    result.path.push_back({{0, 0, 0}, 0});
    result.path.push_back({{5, 0, 5}, 1});
    result.path.push_back({{10, 0, 10}, 2});

    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.path.size(), 3u);
    EXPECT_NEAR(result.totalCost, 15.5f, 0.001f);
    EXPECT_NEAR(result.path[0].position.x, 0.0f, 0.001f);
    EXPECT_NEAR(result.path[2].position.x, 10.0f, 0.001f);
}
