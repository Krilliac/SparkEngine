/**
 * @file NavMesh.cpp
 * @brief NavMesh implementation — query, builder, and manager
 */

#include "NavMesh.h"
#include "../../Utils/Validate.h"
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

    NavMeshQuery::NavMeshQuery(const NavMeshData* navMesh) : m_navMesh(navMesh)
    {
        SPARK_WARN_IF_NULL(Spark::LogCategory::AI, navMesh);
    }

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

            // Helper lambda to process a neighbor triangle
            auto processNeighbor = [&](uint32_t neighbor)
            {
                if (neighbor == UINT32_MAX || neighbor >= triCount || closed[neighbor])
                    return;

                // Check flags
                if ((m_navMesh->triangles[neighbor].flags & request.includeFlags) == 0)
                    return;
                if ((m_navMesh->triangles[neighbor].flags & request.excludeFlags) != 0)
                    return;

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
            };

            // Traverse fixed adjacency (shared edges)
            for (int e = 0; e < 3; ++e)
            {
                processNeighbor(tri.neighborTriangles[e]);
            }

            // Traverse dynamic adjacency (off-mesh connections, jump links)
            for (uint32_t dynNeighbor : tri.adjacency)
            {
                processNeighbor(dynNeighbor);
            }
        }

        return result;
    }

    NavMeshHit NavMeshQuery::Raycast(const XMFLOAT3& start, const XMFLOAT3& end) const
    {
        NavMeshHit hit{};
        if (!m_navMesh || m_navMesh->triangles.empty())
            return hit;

        uint32_t currentTri = FindContainingTriangle(start);
        if (currentTri == UINT32_MAX)
            return hit;

        // Ray direction on XZ plane
        float rayDirX = end.x - start.x;
        float rayDirZ = end.z - start.z;
        float rayLength = std::sqrt(rayDirX * rayDirX + rayDirZ * rayDirZ);

        if (rayLength < 1e-6f)
        {
            // Start and end are the same point; trivially on the navmesh
            hit.position = start;
            hit.normal = m_navMesh->triangles[currentTri].normal;
            hit.triangleIndex = currentTri;
            hit.distance = 0.0f;
            hit.hit = false; // No obstacle hit; entire segment is on navmesh
            return hit;
        }

        // Walk through triangles along the ray using edge crossing detection.
        // We parametrize the ray as P(t) = start + t * (end - start), t in [0, 1].
        // At each triangle, find which edge the ray exits through and step to the neighbor.
        constexpr int kMaxIterations = 512;
        std::vector<bool> visited(m_navMesh->triangles.size(), false);

        float tProgress = 0.0f;

        for (int iter = 0; iter < kMaxIterations; ++iter)
        {
            if (currentTri == UINT32_MAX || currentTri >= m_navMesh->triangles.size())
                break;

            if (visited[currentTri])
                break;
            visited[currentTri] = true;

            const auto& tri = m_navMesh->triangles[currentTri];

            // Check if end point is within this triangle
            const auto& v0 = m_navMesh->vertices[tri.indices[0]].position;
            const auto& v1 = m_navMesh->vertices[tri.indices[1]].position;
            const auto& v2 = m_navMesh->vertices[tri.indices[2]].position;

            // Test if end point is inside this triangle (XZ plane)
            {
                float dx0 = v1.x - v0.x, dz0 = v1.z - v0.z;
                float dx1 = v2.x - v0.x, dz1 = v2.z - v0.z;
                float dxP = end.x - v0.x, dzP = end.z - v0.z;
                float denom = dx0 * dz1 - dx1 * dz0;
                if (std::abs(denom) > 1e-10f)
                {
                    float invD = 1.0f / denom;
                    float u = (dxP * dz1 - dx1 * dzP) * invD;
                    float v = (dx0 * dzP - dxP * dz0) * invD;
                    constexpr float kEps = -0.01f;
                    if (u >= kEps && v >= kEps && (u + v) <= (1.0f - kEps))
                    {
                        // End point is in this triangle; entire segment is on navmesh
                        hit.position = end;
                        hit.normal = tri.normal;
                        hit.triangleIndex = currentTri;
                        hit.distance = rayLength;
                        hit.hit = false; // No obstacle; ray stays on navmesh
                        return hit;
                    }
                }
            }

            // Find which edge the ray exits through.
            // Edges: (v0,v1), (v1,v2), (v2,v0)
            const XMFLOAT3* edgeVerts[3][2] = {{&v0, &v1}, {&v1, &v2}, {&v2, &v0}};
            float bestT = 2.0f; // parameter along the ray, looking for smallest t > tProgress
            int bestEdge = -1;

            for (int e = 0; e < 3; ++e)
            {
                const XMFLOAT3& ea = *edgeVerts[e][0];
                const XMFLOAT3& eb = *edgeVerts[e][1];

                // Intersect ray (start -> end) with edge (ea -> eb) in 2D (XZ plane)
                float edgeDirX = eb.x - ea.x;
                float edgeDirZ = eb.z - ea.z;

                float det = rayDirX * edgeDirZ - rayDirZ * edgeDirX;
                if (std::abs(det) < 1e-10f)
                    continue; // Parallel

                float invDet = 1.0f / det;
                float diffX = ea.x - start.x;
                float diffZ = ea.z - start.z;

                float tRay = (diffX * edgeDirZ - diffZ * edgeDirX) * invDet;
                float tEdge = (diffX * rayDirZ - diffZ * rayDirX) * invDet;

                // tRay must be ahead of current progress, tEdge must be within [0,1]
                constexpr float kEdgeEps = -1e-4f;
                if (tRay > (tProgress - 1e-4f) && tEdge >= kEdgeEps && tEdge <= (1.0f - kEdgeEps))
                {
                    if (tRay < bestT)
                    {
                        bestT = tRay;
                        bestEdge = e;
                    }
                }
            }

            if (bestEdge < 0)
                break; // Could not find exit edge

            // Check if the ray exits the navmesh at this edge (no neighbor)
            uint32_t neighborTri = tri.neighborTriangles[bestEdge];
            if (neighborTri == UINT32_MAX)
            {
                // Ray hit the navmesh boundary
                float hitX = start.x + bestT * rayDirX;
                float hitZ = start.z + bestT * rayDirZ;
                // Interpolate Y from the edge vertices
                const XMFLOAT3& ea = *edgeVerts[bestEdge][0];
                const XMFLOAT3& eb = *edgeVerts[bestEdge][1];
                float edgeDirX = eb.x - ea.x;
                float edgeDirZ = eb.z - ea.z;
                float edgeLen = std::sqrt(edgeDirX * edgeDirX + edgeDirZ * edgeDirZ);
                float projDist = 0.0f;
                if (edgeLen > 1e-6f)
                {
                    projDist = ((hitX - ea.x) * edgeDirX + (hitZ - ea.z) * edgeDirZ) / (edgeLen * edgeLen);
                    projDist = (std::max)(0.0f, (std::min)(1.0f, projDist));
                }
                float hitY = ea.y + projDist * (eb.y - ea.y);

                hit.position = {hitX, hitY, hitZ};
                hit.normal = tri.normal;
                hit.triangleIndex = currentTri;
                float dx = hitX - start.x;
                float dz = hitZ - start.z;
                hit.distance = std::sqrt(dx * dx + dz * dz);
                hit.hit = true; // Ray left the navmesh
                return hit;
            }

            // Move to the neighbor triangle
            tProgress = bestT;
            currentTri = neighborTri;
        }

        // Fallback: could not resolve ray traversal within iteration limit
        // Report the boundary of the last known triangle
        hit.position = m_navMesh->triangles[currentTri].centroid;
        hit.normal = m_navMesh->triangles[currentTri].normal;
        hit.triangleIndex = currentTri;
        float dx = hit.position.x - start.x;
        float dz = hit.position.z - start.z;
        hit.distance = std::sqrt(dx * dx + dz * dz);
        hit.hit = true;
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

        // Build a cumulative area distribution for weighted triangle selection
        const auto& triangles = m_navMesh->triangles;
        float totalArea = 0.0f;
        for (const auto& tri : triangles)
        {
            totalArea += tri.area;
        }

        if (totalArea < 1e-10f)
        {
            // Degenerate mesh; fall back to uniform selection
            static thread_local std::mt19937 fallbackRng{std::random_device{}()};
            std::uniform_int_distribution<uint32_t> dist(0, static_cast<uint32_t>(triangles.size()) - 1);
            return triangles[dist(fallbackRng)].centroid;
        }

        // Select a triangle weighted by area
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<float> areaDist(0.0f, totalArea);
        float sample = areaDist(rng);

        uint32_t selectedTri = 0;
        float cumulative = 0.0f;
        for (uint32_t i = 0; i < static_cast<uint32_t>(triangles.size()); ++i)
        {
            cumulative += triangles[i].area;
            if (sample <= cumulative)
            {
                selectedTri = i;
                break;
            }
        }

        // Generate a uniformly random point within the selected triangle
        // using the square-root parameterization: P = (1-sqrt(r1))*A + sqrt(r1)*(1-r2)*B + sqrt(r1)*r2*C
        const auto& tri = triangles[selectedTri];
        const auto& a = m_navMesh->vertices[tri.indices[0]].position;
        const auto& b = m_navMesh->vertices[tri.indices[1]].position;
        const auto& c = m_navMesh->vertices[tri.indices[2]].position;

        std::uniform_real_distribution<float> unitDist(0.0f, 1.0f);
        float r1 = unitDist(rng);
        float r2 = unitDist(rng);
        float sqrtR1 = std::sqrt(r1);

        float u = 1.0f - sqrtR1;
        float v = sqrtR1 * (1.0f - r2);
        float w = sqrtR1 * r2;

        XMFLOAT3 result;
        result.x = u * a.x + v * b.x + w * c.x;
        result.y = u * a.y + v * b.y + w * c.y;
        result.z = u * a.z + v * b.z + w * c.z;
        return result;
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
        if (!m_navMesh || m_navMesh->triangles.empty())
            return UINT32_MAX;

        // First pass: exact point-in-triangle test using barycentric coordinates
        // projected onto the XZ plane, with a vertical tolerance check.
        constexpr float kVerticalTolerance = 2.0f; // metres
        uint32_t bestTri = UINT32_MAX;
        float bestVerticalDist = kVerticalTolerance;

        for (uint32_t i = 0; i < static_cast<uint32_t>(m_navMesh->triangles.size()); ++i)
        {
            const auto& tri = m_navMesh->triangles[i];
            const auto& v0 = m_navMesh->vertices[tri.indices[0]].position;
            const auto& v1 = m_navMesh->vertices[tri.indices[1]].position;
            const auto& v2 = m_navMesh->vertices[tri.indices[2]].position;

            // 2D barycentric test on XZ plane
            float dx0 = v1.x - v0.x;
            float dz0 = v1.z - v0.z;
            float dx1 = v2.x - v0.x;
            float dz1 = v2.z - v0.z;
            float dxP = point.x - v0.x;
            float dzP = point.z - v0.z;

            float denom = dx0 * dz1 - dx1 * dz0;
            if (std::abs(denom) < 1e-10f)
                continue;

            float invDenom = 1.0f / denom;
            float u = (dxP * dz1 - dx1 * dzP) * invDenom;
            float v = (dx0 * dzP - dxP * dz0) * invDenom;

            // Point is inside the triangle if u >= 0, v >= 0, u + v <= 1
            constexpr float kEpsilon = -0.01f; // small tolerance for edge cases
            if (u >= kEpsilon && v >= kEpsilon && (u + v) <= (1.0f - kEpsilon))
            {
                // Compute the Y coordinate on the triangle surface at this XZ position
                float surfaceY = v0.y + u * (v1.y - v0.y) + v * (v2.y - v0.y);
                float vertDist = std::abs(point.y - surfaceY);

                if (vertDist < bestVerticalDist)
                {
                    bestVerticalDist = vertDist;
                    bestTri = i;
                }
            }
        }

        if (bestTri != UINT32_MAX)
            return bestTri;

        // Fallback: find nearest triangle centroid within a reasonable range
        float bestDistSq = m_navMesh->cellSize * m_navMesh->cellSize * 16.0f;
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

        return bestTri;
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
        const auto& v0 = m_navMesh->vertices[tri.indices[0]].position;
        const auto& v1 = m_navMesh->vertices[tri.indices[1]].position;
        const auto& v2 = m_navMesh->vertices[tri.indices[2]].position;

        // Compute vectors from v0
        XMVECTOR a = XMLoadFloat3(&v0);
        XMVECTOR b = XMLoadFloat3(&v1);
        XMVECTOR c = XMLoadFloat3(&v2);
        XMVECTOR p = XMLoadFloat3(&point);

        XMVECTOR ab = XMVectorSubtract(b, a);
        XMVECTOR ac = XMVectorSubtract(c, a);
        XMVECTOR ap = XMVectorSubtract(p, a);

        // Compute barycentric coordinates via dot products
        float d00 = XMVectorGetX(XMVector3Dot(ab, ab));
        float d01 = XMVectorGetX(XMVector3Dot(ab, ac));
        float d11 = XMVectorGetX(XMVector3Dot(ac, ac));
        float d20 = XMVectorGetX(XMVector3Dot(ap, ab));
        float d21 = XMVectorGetX(XMVector3Dot(ap, ac));

        float denom = d00 * d11 - d01 * d01;
        if (std::abs(denom) < 1e-10f)
        {
            return tri.centroid;
        }

        float invDenom = 1.0f / denom;
        float baryV = (d11 * d20 - d01 * d21) * invDenom;
        float baryW = (d00 * d21 - d01 * d20) * invDenom;
        float baryU = 1.0f - baryV - baryW;

        // Clamp barycentric coordinates to triangle bounds
        baryU = (std::max)(0.0f, (std::min)(1.0f, baryU));
        baryV = (std::max)(0.0f, (std::min)(1.0f, baryV));
        baryW = (std::max)(0.0f, (std::min)(1.0f, baryW));

        // Re-normalize after clamping
        float sum = baryU + baryV + baryW;
        if (sum > 1e-6f)
        {
            float invSum = 1.0f / sum;
            baryU *= invSum;
            baryV *= invSum;
            baryW *= invSum;
        }

        // Compute projected point on triangle plane
        XMFLOAT3 result;
        result.x = v0.x * baryU + v1.x * baryV + v2.x * baryW;
        result.y = v0.y * baryU + v1.y * baryV + v2.y * baryW;
        result.z = v0.z * baryU + v1.z * baryV + v2.z * baryW;
        return result;
    }

    // ============================================================================
    // NavMeshBuilder
    // ============================================================================

    std::unique_ptr<NavMeshData> NavMeshBuilder::Build(const std::vector<XMFLOAT3>& vertices,
                                                       const std::vector<uint32_t>& indices,
                                                       const NavMeshBuildSettings& settings)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);
        SPARK_WARN_IF(Spark::LogCategory::AI, vertices.empty(), "NavMeshBuilder::Build called with empty vertices");
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
        const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            // Bounds-check indices to prevent out-of-range access on corrupted data
            if (indices[i] >= vertexCount || indices[i + 1] >= vertexCount || indices[i + 2] >= vertexCount)
                continue;

            const auto& v0 = vertices[indices[i]];
            const auto& v1 = vertices[indices[i + 1]];
            const auto& v2 = vertices[indices[i + 2]];

            // Check slope — reject triangles too steep for the agent
            XMVECTOR e1 = XMVectorSubtract(XMLoadFloat3(&v1), XMLoadFloat3(&v0));
            XMVECTOR e2 = XMVectorSubtract(XMLoadFloat3(&v2), XMLoadFloat3(&v0));
            XMVECTOR normal = XMVector3Normalize(XMVector3Cross(e1, e2));

            XMFLOAT3 n;
            XMStoreFloat3(&n, normal);
            float slopeAngle = std::acos((std::max)(-1.0f, (std::min)(1.0f, std::abs(n.y)))) * (180.0f / 3.14159265f);

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
            XMStoreFloat3(&tri.normal, normal);
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
                // Count shared vertices between triangles i and j
                int shared = 0;
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
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);
        if (!heightData || width <= 0 || height <= 0)
            return std::make_unique<NavMeshData>();

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
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);
        SPARK_VALIDATE_RET(Spark::LogCategory::AI, !name.empty(), false);
        SPARK_VALIDATE_RET(Spark::LogCategory::AI, !filepath.empty(), false);
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

        // Read vertices with safety limits to prevent excessive allocation from corrupted files
        uint32_t vertexCount;
        file.read(reinterpret_cast<char*>(&vertexCount), 4);
        constexpr uint32_t kMaxVertices = 10'000'000; // 10M vertices max
        if (!file.good() || vertexCount > kMaxVertices)
            return false;
        navMesh->vertices.resize(vertexCount);
        file.read(reinterpret_cast<char*>(navMesh->vertices.data()), vertexCount * sizeof(XMFLOAT3));

        // Read triangles with safety limits
        uint32_t triangleCount;
        file.read(reinterpret_cast<char*>(&triangleCount), 4);
        constexpr uint32_t kMaxTriangles = 10'000'000; // 10M triangles max
        if (!file.good() || triangleCount > kMaxTriangles)
            return false;
        navMesh->triangles.resize(triangleCount);
        for (uint32_t i = 0; i < triangleCount; ++i)
        {
            file.read(reinterpret_cast<char*>(&navMesh->triangles[i].indices), sizeof(uint32_t) * 3);
            file.read(reinterpret_cast<char*>(&navMesh->triangles[i].centroid), sizeof(XMFLOAT3));
            file.read(reinterpret_cast<char*>(&navMesh->triangles[i].normal), sizeof(XMFLOAT3));
            file.read(reinterpret_cast<char*>(&navMesh->triangles[i].area), sizeof(float));
            uint32_t adjCount;
            file.read(reinterpret_cast<char*>(&adjCount), 4);
            constexpr uint32_t kMaxAdjacency = 10'000; // Reasonable adjacency limit per triangle
            if (!file.good() || adjCount > kMaxAdjacency)
                return false;
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
        SPARK_VALIDATE_RET(Spark::LogCategory::AI, !name.empty(), false);
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
        SPARK_VALIDATE_RET(Spark::LogCategory::AI, !name.empty(), nullptr);
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
        SPARK_LOG_INFO(Spark::LogCategory::AI, "NavMeshManager::Clear — removing all %zu navmeshes.",
                       m_navMeshes.size());
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

    // ========================================================================
    // Path Cache (Redot-inspired optimization)
    // ========================================================================

    uint64_t NavMeshQuery::HashPathKey(const XMFLOAT3& start, const XMFLOAT3& goal)
    {
        // Quantize positions to 0.5m grid to allow cache hits for nearby queries
        constexpr float quantGrid = 0.5f;
        auto quantize = [quantGrid](float v) -> int32_t { return static_cast<int32_t>(std::floor(v / quantGrid)); };

        uint64_t hash = 14695981039346656037ULL; // FNV-1a offset basis
        constexpr uint64_t prime = 1099511628211ULL;

        auto mix = [&](int32_t val)
        {
            hash ^= static_cast<uint64_t>(static_cast<uint32_t>(val));
            hash *= prime;
        };

        mix(quantize(start.x));
        mix(quantize(start.y));
        mix(quantize(start.z));
        mix(quantize(goal.x));
        mix(quantize(goal.y));
        mix(quantize(goal.z));

        return hash;
    }

    PathResult NavMeshQuery::FindPathCached(const PathRequest& request, float currentTime) const
    {
        uint64_t key = HashPathKey(request.start, request.end);

        // Check cache
        auto it = m_pathCache.find(key);
        if (it != m_pathCache.end())
        {
            const auto& cached = it->second;
            if (currentTime - cached.timestamp < PATH_CACHE_EXPIRY)
            {
                return cached.result;
            }
            m_pathCache.erase(it);
        }

        // Cache miss — compute path
        PathResult result = FindPath(request);

        // Evict oldest entry if cache is full
        if (m_pathCache.size() >= MAX_CACHED_PATHS)
        {
            float oldestTime = currentTime;
            uint64_t oldestKey = 0;
            for (const auto& [k, v] : m_pathCache)
            {
                if (v.timestamp < oldestTime)
                {
                    oldestTime = v.timestamp;
                    oldestKey = k;
                }
            }
            m_pathCache.erase(oldestKey);
        }

        CachedPath entry;
        entry.result = result;
        entry.timestamp = currentTime;
        m_pathCache[key] = std::move(entry);

        return result;
    }

} // namespace Spark::AI
