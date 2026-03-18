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
#include "NavMeshTypes.h"

#include <unordered_map>
#include <memory>
#include <functional>
#include <queue>
#include <algorithm>


namespace Spark::AI
{

    // =============================================================================
    // NavMesh Query
    // =============================================================================

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

        // ====================================================================
        // Path Cache (Redot/ReX-inspired optimization)
        // ====================================================================

        /// Hash a start+goal position pair for path cache lookup.
        /// Quantizes positions to a grid to allow cache hits for nearby queries.
        static uint64_t HashPathKey(const XMFLOAT3& start, const XMFLOAT3& goal);

        /// Cached path entry with expiry tracking.
        struct CachedPath
        {
            PathResult result;
            float timestamp = 0.0f;    ///< Time when cached
            uint32_t navMeshVersion{}; ///< NavMesh version at cache time
        };

        /// Cache of recent path queries. Key = hashed (start, goal).
        mutable std::unordered_map<uint64_t, CachedPath> m_pathCache;

        /// Maximum number of cached paths before LRU eviction.
        static constexpr size_t MAX_CACHED_PATHS = 64;

        /// Cache validity duration in seconds.
        static constexpr float PATH_CACHE_EXPIRY = 5.0f;

      public:
        /**
         * @brief Find a path with caching (Redot-inspired optimization).
         *
         * Checks the cache first. If a valid cached path exists for the
         * same quantized start+goal, returns it immediately. Otherwise
         * runs FindPath() and caches the result.
         *
         * @param request  Path query parameters.
         * @param currentTime  Current game time for cache expiry.
         * @return PathResult (may be from cache).
         */
        PathResult FindPathCached(const PathRequest& request, float currentTime) const;

        /** @brief Clear all cached paths (call when NavMesh changes). */
        void ClearPathCache() { m_pathCache.clear(); }

        /** @brief Get current cache size. */
        size_t GetPathCacheSize() const { return m_pathCache.size(); }
    };

    // =============================================================================
    // NavMesh Builder
    // =============================================================================

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
