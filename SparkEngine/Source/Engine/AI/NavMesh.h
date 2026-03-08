/**
 * @file NavMesh.h
 * @brief Navigation mesh system for AI pathfinding
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * This file defines the navigation mesh (NavMesh) subsystem used by AI agents to
 * plan and execute paths through the game world. The system is architected in three
 * distinct layers:
 *
 * 1. **Data layer** – `NavMeshData`, `NavVertex`, `NavTriangle`: raw triangle mesh
 *    that represents the walkable surface of the level.
 * 2. **Query layer** – `NavMeshQuery`: per-agent pathfinding and point-location queries
 *    against a fixed NavMeshData (read-only).
 * 3. **Management layer** – `NavMeshBuilder` (offline bake) and `NavMeshManager`
 *    (runtime registry singleton).
 *
 * ## Architecture overview
 *
 * ```
 * NavMeshBuilder::Build()  →  NavMeshData  ←  NavMeshQuery::FindPath()
 *                                  ↑
 *                           NavMeshManager (registry)
 *                                  ↑
 *                           AISystem (creates NavMeshQuery per agent)
 * ```
 *
 * ## Pathfinding algorithm
 * `NavMeshQuery::FindPath()` uses an A* search over the triangle adjacency graph.
 * The heuristic is the Euclidean distance between triangle centroids. Paths are
 * returned as a sequence of `PathPoint` waypoints that agents walk between.
 *
 * ## Integration with AI
 * The `AISystem` creates a `NavMeshQuery` for each agent at initialization time and
 * stores the opaque handle in `AIComponent::navQueryHandle`. Action nodes in behavior
 * trees call the query API through the AISystem helper methods.
 *
 * ## File format
 * Pre-built nav meshes are stored as binary `.snav` files for fast runtime loading.
 * Use the offline NavMesh baking tool (SparkEditor → Navigation → Bake NavMesh) to
 * generate `.snav` files from level geometry.
 *
 * ## Typical usage
 * @code
 *   // --- Level load time ---
 *   NavMeshManager& mgr = NavMeshManager::GetInstance();
 *   mgr.LoadNavMesh("Level01", "Assets/NavMeshes/Level01.snav");
 *
 *   // --- Per-agent initialization ---
 *   auto query = mgr.CreateQuery("Level01");
 *
 *   // --- Per-frame pathfinding ---
 *   PathRequest req;
 *   req.start      = agent.GetPosition();
 *   req.end        = target.GetPosition();
 *   req.agentRadius = 0.6f;
 *   PathResult result = query->FindPath(req);
 *   if (result.found) agent.SetPath(result.path);
 * @endcode
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

    // =============================================================================
    // NavMesh Query
    // =============================================================================

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

    /**
 * @class NavMeshQuery
 * @brief Read-only, per-agent interface for NavMesh spatial queries and pathfinding.
 *
 * Each AI agent should hold its own NavMeshQuery instance (the AISystem creates
 * them and stores the pointer in `AIComponent::navQueryHandle`). Multiple query
 * objects may reference the same underlying `NavMeshData` concurrently because
 * all operations are read-only with respect to the NavMesh.
 *
 * ### Available operations
 * - **FindNearestPoint** – snap any world position to the closest NavMesh surface.
 * - **FindPath**         – compute an A* path between two positions.
 * - **Raycast**          – test line-of-sight along the NavMesh surface.
 * - **IsPointOnNavMesh** – check if a point lies on walkable ground.
 * - **GetRandomPoint**   – pick a uniformly random point on the NavMesh for patrol.
 *
 * @note NavMeshQuery stores a raw non-owning pointer to `NavMeshData`. The data
 *       must remain alive for the lifetime of the query object; the NavMeshManager
 *       guarantees this when queries are created via `NavMeshManager::CreateQuery()`.
 */
    class NavMeshQuery
    {
      public:
        /**
     * @brief Construct a query object bound to the given NavMesh data.
     * @param navMesh  Non-owning pointer to the NavMesh to query. Must not be null.
     */
        explicit NavMeshQuery(const NavMeshData* navMesh);

        /**
     * @brief Snap an arbitrary world position onto the nearest point on the NavMesh surface.
     *
     * Useful for correcting agent positions that have drifted off the NavMesh due to
     * physics interactions, or for validating a goal position before requesting a path.
     *
     * @param position      World-space position to project onto the NavMesh.
     * @param searchRadius  Radius (metres) within which to search for the nearest triangle.
     *                      Increase if the NavMesh is far below the query point. Default: 10 m.
     * @return              NavMeshHit with `hit = true` and the snapped position, or
     *                      `hit = false` if no triangle was found within `searchRadius`.
     */
        NavMeshHit FindNearestPoint(const XMFLOAT3& position, float searchRadius = 10.0f) const;

        /**
     * @brief Compute an A* path between two world positions.
     *
     * Both start and end positions are automatically snapped to the nearest NavMesh
     * triangle before the search begins. The returned path accounts for agent radius
     * and the include/exclude flags specified in the request.
     *
     * Time complexity: O(T log T) where T is the number of triangles in the NavMesh.
     * For large levels, consider restricting the search area with tight flag masks.
     *
     * @param request  Pathfinding parameters including start, end, agent radius, and flags.
     * @return         PathResult with `found = true` and a waypoint list, or `found = false`
     *                 if the start or end could not be mapped to the NavMesh or if no
     *                 connected path exists.
     */
        PathResult FindPath(const PathRequest& request) const;

        /**
     * @brief Cast a ray along the NavMesh surface and find the first obstacle edge.
     *
     * Useful for line-of-sight tests along the ground plane, or for checking whether
     * a direct path between two points is blocked by a wall without running a full A*.
     *
     * @param start  World-space start position of the ray.
     * @param end    World-space end position of the ray.
     * @return       NavMeshHit with the first point where the ray leaves the walkable
     *               surface, or `hit = false` if the entire segment lies on the NavMesh.
     */
        NavMeshHit Raycast(const XMFLOAT3& start, const XMFLOAT3& end) const;

        /**
     * @brief Test whether a world-space point lies on the walkable NavMesh surface.
     *
     * @param point      Position to test.
     * @param tolerance  Vertical tolerance (metres) for matching the NavMesh surface height.
     *                   Increase to catch points slightly above or below the floor. Default: 0.5 m.
     * @return           `true` if the point is on a walkable NavTriangle within tolerance.
     */
        bool IsPointOnNavMesh(const XMFLOAT3& point, float tolerance = 0.5f) const;

        /**
     * @brief Return a uniformly random point anywhere on the NavMesh.
     *
     * Useful for spawning patrol points, wandering destinations, or randomly placing
     * NPCs. The distribution is weighted by triangle area so larger triangles are
     * sampled more frequently, producing an approximately uniform spatial distribution.
     *
     * @return  A random world-space position on the NavMesh surface.
     */
        XMFLOAT3 GetRandomPoint() const;

        /**
     * @brief Return a random point on the NavMesh within a circle around `center`.
     *
     * Uses rejection sampling: generates random NavMesh points and rejects those
     * outside the circle until one is found within `radius`. For very dense NavMeshes
     * or small radii this may take several iterations.
     *
     * @param center  World-space center of the search circle.
     * @param radius  Search radius (metres).
     * @return        A random world-space position on the NavMesh within `radius` of `center`.
     *                Returns `center` if no valid point is found after a maximum number of tries.
     */
        XMFLOAT3 GetRandomPointInCircle(const XMFLOAT3& center, float radius) const;

      private:
        /** @brief Non-owning pointer to the NavMesh data this query operates on. */
        const NavMeshData* m_navMesh;

        /**
     * @brief Find the index of the NavTriangle containing the given world position.
     *
     * @param point  World-space position.
     * @return       Triangle index, or `UINT32_MAX` if not found.
     */
        uint32_t FindContainingTriangle(const XMFLOAT3& point) const;

        /**
     * @brief Compute the A* heuristic estimate between two world positions.
     *
     * Currently uses the straight-line Euclidean distance, which is admissible
     * (never overestimates) for a NavMesh graph where edge costs equal Euclidean length.
     *
     * @param a  First position.
     * @param b  Second position.
     * @return   Heuristic cost estimate.
     */
        float HeuristicCost(const XMFLOAT3& a, const XMFLOAT3& b) const;

        /**
     * @brief Project a 3D point onto a specific NavMesh triangle's surface.
     *
     * Used to snap a point that is slightly above or below a triangle down to its plane.
     *
     * @param point     World-space position to project.
     * @param triIndex  Index of the target triangle.
     * @return          The projected point on the triangle's plane, clamped to the triangle's bounds.
     */
        XMFLOAT3 ProjectPointToTriangle(const XMFLOAT3& point, uint32_t triIndex) const;
    };

    // =============================================================================
    // NavMesh Builder
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

    /**
 * @class NavMeshBuilder
 * @brief Offline NavMesh baking utility.
 *
 * NavMeshBuilder converts raw level geometry (triangle soup) or heightfield data
 * into a navigation mesh suitable for runtime pathfinding. Both methods are
 * computationally expensive and intended for use at level-design time in the editor
 * or in a standalone build pipeline — not at runtime during gameplay.
 *
 * @note The implementation uses Recast (if available) for voxel-based NavMesh
 *       generation. A simpler fallback grid-based builder is provided for platforms
 *       where Recast is not supported.
 */
    class NavMeshBuilder
    {
      public:
        /**
     * @brief Build a NavMesh from arbitrary triangle-soup world geometry.
     *
     * Voxelizes the input geometry, erodes walkable regions by agent dimensions,
     * extracts contours, and triangulates the result into a navigation mesh.
     *
     * @param vertices  World-space positions of the geometry vertices.
     * @param indices   Triangle indices into `vertices` (every 3 indices = one triangle).
     * @param settings  Build parameters controlling resolution and agent characteristics.
     * @return          Unique pointer to the generated `NavMeshData`, or `nullptr` on failure.
     *
     * @code
     *   auto data = NavMeshBuilder::Build(levelVerts, levelIndices, settings);
     *   if (data) NavMeshManager::GetInstance().RegisterNavMesh("Level01", std::move(data));
     * @endcode
     */
        static std::unique_ptr<NavMeshData> Build(const std::vector<XMFLOAT3>& vertices,
                                                  const std::vector<uint32_t>& indices,
                                                  const NavMeshBuildSettings& settings);

        /**
     * @brief Build a NavMesh from heightfield data (e.g. terrain).
     *
     * Optimized for height-map terrains. The heightfield is first voxelized
     * with the given cell size, then processed identically to the triangle-soup path.
     *
     * @param heightData  Flat array of height values (row-major, width × height elements).
     * @param width       Number of columns in the heightfield.
     * @param height      Number of rows in the heightfield.
     * @param origin      World-space position of the bottom-left corner of the heightfield.
     * @param cellSize    World-space size of each heightfield cell (metres).
     * @param settings    Build parameters.
     * @return            Unique pointer to the generated `NavMeshData`, or `nullptr` on failure.
     */
        static std::unique_ptr<NavMeshData> BuildFromHeightfield(const float* heightData, int width, int height,
                                                                 const XMFLOAT3& origin, float cellSize,
                                                                 const NavMeshBuildSettings& settings);
    };

    // =============================================================================
    // NavMesh Manager
    // =============================================================================

    /**
 * @class NavMeshManager
 * @brief Singleton registry for all navigation meshes in the current session.
 *
 * NavMeshManager provides a central store for named `NavMeshData` instances and
 * factory methods for creating `NavMeshQuery` objects. By centralizing storage the
 * manager ensures that multiple AI agents referencing the same level's NavMesh share
 * a single copy of the (potentially large) mesh data.
 *
 * ### Lifecycle
 * - At level load: call `LoadNavMesh()` or `BuildNavMesh()` to register the level's data.
 * - During agent initialization: call `CreateQuery()` to obtain a per-agent query object.
 * - At level unload: call `Clear()` or `RemoveNavMesh()` to free memory.
 *
 * ### Thread safety
 * All mutation methods (`LoadNavMesh`, `BuildNavMesh`, `RemoveNavMesh`, `Clear`) must
 * be called from the main thread. `GetNavMesh()` and `CreateQuery()` are read-only and
 * safe to call concurrently from worker threads once all mutations are complete.
 *
 * @code
 *   auto& mgr = NavMeshManager::GetInstance();
 *   mgr.LoadNavMesh("Level_01", "Assets/NavMeshes/Level_01.snav");
 *
 *   // Per agent at spawn
 *   auto query = mgr.CreateQuery("Level_01");
 * @endcode
 */
    class NavMeshManager
    {
      public:
        /**
     * @brief Access the global singleton instance.
     * @return  Reference to the single NavMeshManager.
     */
        static NavMeshManager& GetInstance();

        /**
     * @brief Load a pre-baked NavMesh from a `.snav` binary file.
     *
     * Deserializes the binary file into a `NavMeshData` and registers it under
     * `name`. If a NavMesh with the same name already exists it is replaced.
     *
     * @param name      Unique name to register this NavMesh under (e.g. "Level_01").
     * @param filepath  Path to the `.snav` file. Supports both absolute and project-relative paths.
     * @return          `true` on successful load; `false` if the file is missing or corrupt.
     */
        bool LoadNavMesh(const std::string& name, const std::string& filepath);

        /**
     * @brief Build a NavMesh from geometry and register it with the given name.
     *
     * A convenience wrapper that calls `NavMeshBuilder::Build()` and registers the
     * result. Intended for procedurally generated levels where a pre-baked file is
     * not available.
     *
     * @param name      Unique name to register the result under.
     * @param vertices  World-space vertex positions.
     * @param indices   Triangle indices.
     * @param settings  Build parameters.
     * @return          `true` if the build succeeded and the result was registered.
     */
        bool BuildNavMesh(const std::string& name, const std::vector<XMFLOAT3>& vertices,
                          const std::vector<uint32_t>& indices, const NavMeshBuildSettings& settings);

        /**
     * @brief Look up a NavMesh by name.
     *
     * @param name  Name of the NavMesh to retrieve.
     * @return      Const pointer to the NavMeshData, or `nullptr` if not found.
     */
        const NavMeshData* GetNavMesh(const std::string& name) const;

        /**
     * @brief Create a NavMeshQuery object for the named NavMesh.
     *
     * The returned query references the stored NavMeshData by pointer. The
     * NavMeshManager retains ownership of the data; do not call `Clear()` or
     * `RemoveNavMesh()` while active queries exist.
     *
     * @param name  Name of the NavMesh to create a query for.
     * @return      Unique pointer to a new NavMeshQuery, or `nullptr` if the name is unknown.
     */
        std::unique_ptr<NavMeshQuery> CreateQuery(const std::string& name) const;

        /**
     * @brief Unregister and free a single NavMesh by name.
     *
     * All existing NavMeshQuery objects referencing this NavMesh become dangling
     * after this call. Ensure no active queries exist before removing.
     *
     * @param name  Name of the NavMesh to remove.
     */
        void RemoveNavMesh(const std::string& name);

        /**
     * @brief Unregister and free all stored NavMeshes.
     *
     * Typically called during level unload. Ensure all NavMeshQuery objects have
     * been destroyed before calling Clear().
     */
        void Clear();

        // =========================================================================
        // Console integration
        // =========================================================================

        /**
     * @brief List all registered NavMesh names (console integration).
     * @return  Newline-separated string listing all NavMesh names and triangle counts.
     */
        std::string Console_ListNavMeshes() const;

        /**
     * @brief Get detailed information about a specific NavMesh (console integration).
     *
     * @param name  Name of the NavMesh to describe.
     * @return      Multi-line string with vertex count, triangle count, bounds, and build settings.
     */
        std::string Console_GetNavMeshInfo(const std::string& name) const;

      private:
        /** @brief Singleton constructor — use GetInstance(). */
        NavMeshManager() = default;

        /** @brief Map from registered name to owned NavMeshData. */
        std::unordered_map<std::string, std::unique_ptr<NavMeshData>> m_navMeshes;
    };

} // namespace Spark::AI
