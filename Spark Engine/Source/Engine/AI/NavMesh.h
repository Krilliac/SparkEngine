/**
 * @file NavMesh.h
 * @brief Navigation mesh system for AI pathfinding
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides navigation mesh generation, storage, and querying for AI agents.
 * Designed for integration with Recast/Detour when available, with a
 * fallback grid-based implementation.
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <cstdint>
#include <queue>
#include <algorithm>

using namespace DirectX;

namespace Spark::AI {

// ============================================================================
// NavMesh Data Structures
// ============================================================================

struct NavVertex {
    XMFLOAT3 position;
};

struct NavTriangle {
    uint32_t indices[3];
    uint32_t neighborTriangles[3];  ///< Adjacent triangle indices (UINT32_MAX = no neighbor)
    XMFLOAT3 centroid;
    XMFLOAT3 normal;               ///< Triangle surface normal
    float area;
    uint16_t flags;                 ///< Walkability flags (terrain type, cost modifiers)
    std::vector<uint32_t> adjacency; ///< Dynamic adjacency list for pathfinding
};

struct NavMeshData {
    std::vector<NavVertex> vertices;
    std::vector<NavTriangle> triangles;
    XMFLOAT3 boundsMin{0, 0, 0};
    XMFLOAT3 boundsMax{0, 0, 0};
    float cellSize = 0.3f;
    float cellHeight = 0.2f;
    float agentHeight = 2.0f;
    float agentRadius = 0.6f;
    float agentMaxClimb = 0.9f;
    float agentMaxSlope = 45.0f;
};

// ============================================================================
// Pathfinding
// ============================================================================

struct PathPoint {
    XMFLOAT3 position;
    uint32_t triangleIndex;
};

struct PathRequest {
    XMFLOAT3 start;
    XMFLOAT3 end;
    float agentRadius = 0.6f;
    uint16_t includeFlags = 0xFFFF;  ///< Only traverse triangles with these flags
    uint16_t excludeFlags = 0x0000;  ///< Never traverse triangles with these flags
};

struct PathResult {
    bool found = false;
    std::vector<PathPoint> path;
    float totalCost = 0.0f;
};

// ============================================================================
// NavMesh Query
// ============================================================================

struct NavMeshHit {
    XMFLOAT3 position;
    XMFLOAT3 normal;
    uint32_t triangleIndex;
    float distance;
    bool hit = false;
};

class NavMeshQuery {
public:
    explicit NavMeshQuery(const NavMeshData* navMesh);

    /// Find the nearest point on the navmesh to a given position
    NavMeshHit FindNearestPoint(const XMFLOAT3& position, float searchRadius = 10.0f) const;

    /// Find a path from start to end using A*
    PathResult FindPath(const PathRequest& request) const;

    /// Raycast along the navmesh surface
    NavMeshHit Raycast(const XMFLOAT3& start, const XMFLOAT3& end) const;

    /// Check if a point is on the navmesh
    bool IsPointOnNavMesh(const XMFLOAT3& point, float tolerance = 0.5f) const;

    /// Get a random point on the navmesh
    XMFLOAT3 GetRandomPoint() const;

    /// Get a random point within radius of center
    XMFLOAT3 GetRandomPointInCircle(const XMFLOAT3& center, float radius) const;

private:
    const NavMeshData* m_navMesh;

    uint32_t FindContainingTriangle(const XMFLOAT3& point) const;
    float HeuristicCost(const XMFLOAT3& a, const XMFLOAT3& b) const;
    XMFLOAT3 ProjectPointToTriangle(const XMFLOAT3& point, uint32_t triIndex) const;
};

// ============================================================================
// NavMesh Builder
// ============================================================================

struct NavMeshBuildSettings {
    float cellSize = 0.3f;
    float cellHeight = 0.2f;
    float agentHeight = 2.0f;
    float agentRadius = 0.6f;
    float agentMaxClimb = 0.9f;
    float agentMaxSlope = 45.0f;
    float regionMinSize = 8.0f;
    float regionMergeSize = 20.0f;
    float edgeMaxLen = 12.0f;
    float edgeMaxError = 1.3f;
    int vertsPerPoly = 6;
    float detailSampleDist = 6.0f;
    float detailSampleMaxError = 1.0f;
};

class NavMeshBuilder {
public:
    /// Build navmesh from triangle soup (world geometry)
    static std::unique_ptr<NavMeshData> Build(
        const std::vector<XMFLOAT3>& vertices,
        const std::vector<uint32_t>& indices,
        const NavMeshBuildSettings& settings);

    /// Build navmesh from heightfield data
    static std::unique_ptr<NavMeshData> BuildFromHeightfield(
        const float* heightData, int width, int height,
        const XMFLOAT3& origin, float cellSize,
        const NavMeshBuildSettings& settings);
};

// ============================================================================
// NavMesh Manager
// ============================================================================

class NavMeshManager {
public:
    static NavMeshManager& GetInstance();

    /// Load a pre-built navmesh from file
    bool LoadNavMesh(const std::string& name, const std::string& filepath);

    /// Build and register a navmesh from geometry
    bool BuildNavMesh(const std::string& name,
                      const std::vector<XMFLOAT3>& vertices,
                      const std::vector<uint32_t>& indices,
                      const NavMeshBuildSettings& settings);

    /// Get a navmesh by name
    const NavMeshData* GetNavMesh(const std::string& name) const;

    /// Create a query object for a navmesh
    std::unique_ptr<NavMeshQuery> CreateQuery(const std::string& name) const;

    /// Remove a navmesh
    void RemoveNavMesh(const std::string& name);

    /// Clear all navmeshes
    void Clear();

    /// Console integration
    std::string Console_ListNavMeshes() const;
    std::string Console_GetNavMeshInfo(const std::string& name) const;

private:
    NavMeshManager() = default;
    std::unordered_map<std::string, std::unique_ptr<NavMeshData>> m_navMeshes;
};

} // namespace Spark::AI
