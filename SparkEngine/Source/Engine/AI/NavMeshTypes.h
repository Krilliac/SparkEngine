/**
 * @file NavMeshTypes.h
 * @brief Data types, enums, and structures for the NavMesh subsystem.
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Extracted from NavMesh.h to keep type definitions separate from class
 * declarations. This file contains the value types used by NavMeshQuery,
 * NavMeshBuilder, and NavMeshManager — including mesh data structures,
 * pathfinding request/result types, and build settings.
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <cstdint>


namespace Spark::AI
{

    // =============================================================================
    // NavMesh Data Structures
    // =============================================================================

    /**
 * @brief A single vertex in the navigation mesh triangle soup.
 *
 * Vertices are shared between triangles; each NavTriangle references three
 * vertex indices into the `NavMeshData::vertices` array.
 */
    struct NavVertex
    {
        /** @brief World-space position of the vertex (metres). */
        XMFLOAT3 position;
    };

    /**
 * @brief A single walkable triangle in the navigation mesh.
 *
 * NavTriangles form the fundamental unit of the NavMesh. Each triangle
 * represents a small region of walkable surface. A* pathfinding navigates
 * the graph formed by triangle adjacency.
 *
 * ### Adjacency
 * Each triangle can be adjacent to up to three other triangles (one per edge).
 * `neighborTriangles[i]` stores the index of the triangle that shares edge i,
 * or `UINT32_MAX` if there is no neighbor (boundary edge).
 *
 * ### Flags
 * The `flags` bitmask allows marking triangles with terrain-type information
 * (e.g. "water", "shallow", "steep") so pathfinding queries can include or
 * exclude specific surface types via `PathRequest::includeFlags` and
 * `PathRequest::excludeFlags`.
 */
    struct NavTriangle
    {
        /** @brief Indices into `NavMeshData::vertices` defining this triangle's three corners. */
        uint32_t indices[3];

        /**
     * @brief Indices of adjacent triangles sharing each edge.
     *
     * Index mapping: [0] = edge opposite vertex 0, [1] = edge opposite vertex 1,
     * [2] = edge opposite vertex 2. `UINT32_MAX` indicates no neighbor (boundary).
     */
        uint32_t neighborTriangles[3];

        /**
     * @brief World-space centroid of the triangle.
     *
     * Pre-computed and stored to avoid repeated recalculation during pathfinding.
     * Used as the representative "node position" for the A* graph.
     */
        XMFLOAT3 centroid;

        /**
     * @brief Surface normal vector of the triangle.
     *
     * Upward-facing normals indicate flat walkable surfaces. Normals that deviate
     * significantly from {0, 1, 0} indicate slopes that may be flagged as non-walkable
     * depending on `NavMeshBuildSettings::agentMaxSlope`.
     */
        XMFLOAT3 normal;

        /**
     * @brief Surface area of the triangle (square metres).
     *
     * Larger areas allow greater sampling granularity for `NavMeshQuery::GetRandomPoint()`.
     * Also used as a cost modifier: very small triangles are given a higher traversal cost.
     */
        float area;

        /**
     * @brief Bitmask encoding surface type and walkability modifiers.
     *
     * Standard flag bits (application-defined):
     * - Bit 0: Walkable surface.
     * - Bit 1: Water surface (slower movement).
     * - Bit 2: Hazard zone (avoided unless no other path).
     *
     * Use `PathRequest::includeFlags` and `excludeFlags` to filter navigation
     * based on these flags.
     */
        uint16_t flags;

        /**
     * @brief Dynamic adjacency list for runtime pathfinding.
     *
     * Supplement to the fixed `neighborTriangles[3]`. Used for non-planar adjacency
     * such as jump links and off-mesh connections added at runtime.
     */
        std::vector<uint32_t> adjacency;
    };

    /**
 * @brief Complete navigation mesh dataset for a single level or region.
 *
 * NavMeshData is the serializable, immutable product of a NavMesh bake. At runtime
 * it is loaded from a `.snav` file and held in the `NavMeshManager`. Multiple
 * `NavMeshQuery` objects can reference the same `NavMeshData` safely from different
 * threads as long as the data is not modified.
 *
 * ### Build parameters
 * The parameters at the bottom of the struct mirror the settings used during the
 * bake. They are stored for documentation purposes and for use by the editor's
 * NavMesh preview rendering.
 */
    struct NavMeshData
    {
        /** @brief All vertices of the navigation mesh. */
        std::vector<NavVertex> vertices;

        /** @brief All triangles of the navigation mesh. */
        std::vector<NavTriangle> triangles;

        /**
     * @brief World-space axis-aligned bounding box minimum corner.
     *
     * Used by spatial queries to quickly reject points outside the NavMesh bounds
     * without iterating all triangles.
     */
        XMFLOAT3 boundsMin{0, 0, 0};

        /**
     * @brief World-space axis-aligned bounding box maximum corner.
     */
        XMFLOAT3 boundsMax{0, 0, 0};

        /**
     * @brief Horizontal voxel cell size used during the bake (metres).
     *
     * Smaller values produce a more detailed NavMesh but increase bake time and
     * triangle count. Typical range: 0.1–0.5 m.
     */
        float cellSize = 0.3f;

        /**
     * @brief Vertical voxel cell height used during the bake (metres).
     *
     * Controls step-height resolution. Must be small enough to capture floor-level
     * geometry variation. Typical range: 0.1–0.3 m.
     */
        float cellHeight = 0.2f;

        /**
     * @brief Minimum walkable clearance height for agents (metres).
     *
     * Regions with ceiling clearance less than this value are marked non-walkable.
     * Set to the tallest agent type that should navigate this mesh.
     */
        float agentHeight = 2.0f;

        /**
     * @brief Agent capsule radius used for obstacle erosion (metres).
     *
     * NavMesh boundaries are inset by this radius so agents do not clip into walls.
     * Must be consistent with the `ColliderComponent::radius` of the player/NPCs.
     */
        float agentRadius = 0.6f;

        /**
     * @brief Maximum climbable step height (metres).
     *
     * Steps and kerbs shorter than this value are treated as walkable surface.
     * Typical value: 0.3–0.9 m.
     */
        float agentMaxClimb = 0.9f;

        /**
     * @brief Maximum walkable slope angle (degrees from horizontal).
     *
     * Surfaces steeper than this are marked non-walkable. Typical range: 30–60°.
     */
        float agentMaxSlope = 45.0f;
    };

    // =============================================================================
    // Pathfinding
    // =============================================================================

    /**
 * @brief A single point along a computed path.
 *
 * Paths are returned as an ordered sequence of PathPoints from start to goal.
 * Agents advance along the sequence, moving towards each point in turn.
 */
    struct PathPoint
    {
        /**
     * @brief World-space position of the waypoint (metres).
     *
     * Agents should navigate to within a small tolerance (e.g. 0.3 m) of each
     * point before advancing to the next.
     */
        XMFLOAT3 position;

        /**
     * @brief Index of the NavMesh triangle containing this waypoint.
     *
     * Stored to accelerate subsequent NavMesh snapping operations if the path
     * needs to be re-evaluated partway through (e.g. the target moved).
     */
        uint32_t triangleIndex;
    };

    /**
 * @brief Input parameters for a single pathfinding query.
 *
 * Fill this struct and pass it to `NavMeshQuery::FindPath()`. The agent radius
 * is used to compute a clearance-adjusted path that avoids narrow gaps narrower
 * than the agent.
 */
    struct PathRequest
    {
        /** @brief World-space start position of the path. Does not need to be exactly on the NavMesh. */
        XMFLOAT3 start;

        /** @brief World-space end/goal position of the path. Does not need to be exactly on the NavMesh. */
        XMFLOAT3 end;

        /**
     * @brief Radius of the agent requesting the path (metres).
     *
     * Used for clearance checks — narrow passages smaller than `2 * agentRadius`
     * will be avoided. Should match `NavMeshData::agentRadius` or be smaller.
     */
        float agentRadius = 0.6f;

        /**
     * @brief Bitmask of NavTriangle flags that the path is allowed to traverse.
     *
     * Default: 0xFFFF (all flags allowed). Set specific bits to restrict the agent
     * to certain surface types (e.g. only walkable + road surfaces).
     */
        uint16_t includeFlags = 0xFFFF;

        /**
     * @brief Bitmask of NavTriangle flags that the path must avoid.
     *
     * Default: 0x0000 (no exclusions). Set bits corresponding to hazard zones or
     * impassable terrain types the agent should not enter.
     */
        uint16_t excludeFlags = 0x0000;
    };

    /**
 * @brief Result returned by `NavMeshQuery::FindPath()`.
 *
 * Check `found` before using the `path` vector. If `found` is false, no path
 * exists between start and end (e.g. the points are in disconnected NavMesh
 * islands) and `path` will be empty.
 */
    struct PathResult
    {
        /**
     * @brief Whether a valid path was found connecting start to end.
     *
     * If false, the AI agent should fall back to a default behavior
     * (stand still, alert player unreachable, etc.).
     */
        bool found = false;

        /**
     * @brief Ordered list of world-space waypoints from start to goal.
     *
     * The first element is nearest to `PathRequest::start`; the last is nearest
     * to `PathRequest::end`. Agents walk these in index order.
     */
        std::vector<PathPoint> path;

        /**
     * @brief Total estimated path cost (roughly equivalent to path length in metres).
     *
     * Useful for comparing multiple candidate paths or determining whether the
     * goal is too far to reach within a time budget.
     */
        float totalCost = 0.0f;
    };

    /**
 * @brief Intermediate result from a NavMesh spatial query (hit test or raycast).
 *
 * Used by `NavMeshQuery::FindNearestPoint()` and `NavMeshQuery::Raycast()` to
 * return the point on the NavMesh surface closest to the queried position,
 * along with the triangle and surface normal.
 */
    struct NavMeshHit
    {
        /** @brief World-space point on the NavMesh surface. */
        XMFLOAT3 position;

        /** @brief Surface normal at the hit point. */
        XMFLOAT3 normal;

        /** @brief Index of the NavTriangle containing the hit point. */
        uint32_t triangleIndex;

        /** @brief Distance from the query origin to the hit point (metres). */
        float distance;

        /** @brief True if the query produced a valid hit; false if nothing was found. */
        bool hit = false;
    };

    // =============================================================================
    // NavMesh Build Settings
    // =============================================================================

    /**
 * @brief Build settings for offline NavMesh generation.
 *
 * These parameters are passed to `NavMeshBuilder::Build()` to control the trade-off
 * between NavMesh detail, walkable surface coverage, and triangle count.
 * The defaults represent sensible values for a standard FPS level.
 *
 * After a bake the resulting values are embedded in `NavMeshData` for reference.
 * See Recast/Detour documentation for detailed parameter descriptions.
 */
    struct NavMeshBuildSettings
    {
        /** @brief Horizontal voxelization cell size (metres). Smaller = more detail. Default: 0.3 m. */
        float cellSize = 0.3f;

        /** @brief Vertical voxelization cell height (metres). Should be ≤ agentMaxClimb. Default: 0.2 m. */
        float cellHeight = 0.2f;

        /** @brief Minimum vertical clearance (metres) for a region to be considered walkable. Default: 2.0 m. */
        float agentHeight = 2.0f;

        /** @brief Agent capsule radius (metres); NavMesh edges are eroded by this amount. Default: 0.6 m. */
        float agentRadius = 0.6f;

        /** @brief Maximum step height the agent can climb (metres). Default: 0.9 m. */
        float agentMaxClimb = 0.9f;

        /** @brief Maximum slope angle in degrees from horizontal. Default: 45°. */
        float agentMaxSlope = 45.0f;

        /** @brief Minimum walkable region size (cell units). Small regions are discarded. Default: 8. */
        float regionMinSize = 8.0f;

        /** @brief Adjacent regions smaller than this are merged into neighbors (cell units). Default: 20. */
        float regionMergeSize = 20.0f;

        /** @brief Maximum NavMesh edge length (metres). Longer edges are subdivided. Default: 12 m. */
        float edgeMaxLen = 12.0f;

        /** @brief Maximum deviation between simplified edge and original contour (metres). Default: 1.3 m. */
        float edgeMaxError = 1.3f;

        /** @brief Maximum vertices per polygon in the final NavMesh. 3 = triangles. Default: 6. */
        int vertsPerPoly = 6;

        /** @brief Sampling distance for detail mesh height adjustment. Default: 6.0. */
        float detailSampleDist = 6.0f;

        /** @brief Maximum error between detail mesh and source geometry (metres). Default: 1.0 m. */
        float detailSampleMaxError = 1.0f;
    };

} // namespace Spark::AI
