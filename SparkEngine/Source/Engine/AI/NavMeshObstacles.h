/**
 * @file NavMeshObstacles.h
 * @brief Dynamic obstacle management for runtime NavMesh carving
 * @author Spark Engine Team
 * @date 2026
 *
 * @details
 * Implements R4.4 (NavMesh Dynamic Obstacles) from the architecture analysis. Provides
 * a manager that tracks axis-aligned box and cylinder obstacles, carves them out of
 * the NavMesh by flagging affected triangles as non-walkable, and supports real-time
 * updates as obstacles move, spawn, or are destroyed.
 *
 * ## Algorithm
 * When an obstacle is added or moved, the manager iterates NavMesh triangles and tests
 * each triangle's centroid (and vertices) against the obstacle's world-space bounds.
 * Affected triangles have their walkability flag cleared. When an obstacle is removed,
 * affected triangles are restored unless another obstacle still overlaps them.
 *
 * ## Integration
 * The NavMeshObstacleManager operates on `NavMeshData` directly. It should be updated
 * each frame (or when obstacles change) before any pathfinding queries are issued.
 *
 * ## Usage
 * @code
 *   auto& obsMgr = NavMeshObstacleManager::GetInstance();
 *   obsMgr.SetNavMesh(&navMeshData);
 *
 *   // Add a box obstacle (e.g. a crate)
 *   ObstacleDesc desc;
 *   desc.shape    = ObstacleShape::Box;
 *   desc.position = {10.0f, 0.0f, 5.0f};
 *   desc.halfExtents = {1.0f, 1.0f, 1.0f};
 *   ObstacleHandle handle = obsMgr.AddObstacle(desc);
 *
 *   // Move the crate
 *   desc.position = {12.0f, 0.0f, 5.0f};
 *   obsMgr.UpdateObstacle(handle, desc);
 *
 *   // Remove it
 *   obsMgr.RemoveObstacle(handle);
 * @endcode
 */

#pragma once
#include "NavMesh.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>


namespace Spark::AI
{

    // =========================================================================
    // Obstacle types
    // =========================================================================

    /**
     * @brief Shape type for a dynamic NavMesh obstacle.
     */
    enum class ObstacleShape : uint8_t
    {
        Box,     ///< Axis-aligned box defined by half-extents.
        Cylinder ///< Upright cylinder defined by radius and height.
    };

    /**
     * @brief Opaque handle identifying a tracked obstacle.
     *
     * Returned by `AddObstacle()` and used to reference the obstacle in
     * `UpdateObstacle()` and `RemoveObstacle()`.
     */
    using ObstacleHandle = uint32_t;

    /** @brief Sentinel value indicating an invalid obstacle handle. */
    static constexpr ObstacleHandle InvalidObstacleHandle = 0;

    /**
     * @brief Descriptor for a dynamic obstacle that carves into the NavMesh.
     *
     * Describes the shape, position, and dimensions of an obstacle. Pass to
     * `AddObstacle()` or `UpdateObstacle()` to modify the NavMesh.
     */
    struct ObstacleDesc
    {
        /** @brief Shape of the obstacle volume. */
        ObstacleShape shape = ObstacleShape::Box;

        /** @brief World-space center position of the obstacle. */
        XMFLOAT3 position{0.0f, 0.0f, 0.0f};

        /**
         * @brief Half-extents for Box obstacles (X, Y, Z).
         *
         * Ignored for Cylinder shapes. The full box spans
         * [position - halfExtents, position + halfExtents].
         */
        XMFLOAT3 halfExtents{1.0f, 1.0f, 1.0f};

        /**
         * @brief Radius for Cylinder obstacles (metres).
         *
         * Ignored for Box shapes. The cylinder is centered at `position`
         * and extends vertically by `height / 2` above and below.
         */
        float radius = 1.0f;

        /**
         * @brief Height for Cylinder obstacles (metres).
         *
         * Ignored for Box shapes. Full vertical extent is
         * [position.y - height/2, position.y + height/2].
         */
        float height = 2.0f;

        /**
         * @brief Additional margin around the obstacle (metres).
         *
         * Expands the effective obstacle volume by this amount in all directions
         * to account for agent radius and provide a safety buffer.
         */
        float margin = 0.0f;
    };

    // =========================================================================
    // Internal obstacle record
    // =========================================================================

    /**
     * @brief Internal record tracking a live obstacle and the triangles it affects.
     */
    struct ObstacleRecord
    {
        /** @brief Current descriptor for this obstacle. */
        ObstacleDesc desc;

        /** @brief Set of NavMesh triangle indices affected (carved) by this obstacle. */
        std::unordered_set<uint32_t> affectedTriangles;

        /** @brief Whether the obstacle's descriptor has changed since last carve. */
        bool dirty = false;
    };

    // =========================================================================
    // NavMeshObstacleManager
    // =========================================================================

    /**
     * @class NavMeshObstacleManager
     * @brief Manages dynamic obstacles that carve into a NavMesh at runtime.
     *
     * The obstacle manager is a singleton that operates on a single NavMeshData
     * instance. It maintains a registry of active obstacles and tracks which
     * NavMesh triangles each obstacle affects. When obstacles are added, moved,
     * or removed, the affected triangles' walkability flags are updated.
     *
     * ### Thread safety
     * All methods must be called from the main thread. The manager modifies
     * NavMeshData triangle flags in place, so pathfinding queries should not
     * be executing concurrently with carve operations.
     *
     * ### Performance
     * Triangle overlap testing is O(T) per obstacle where T is the total number
     * of NavMesh triangles. For levels with many triangles (>50k), consider
     * calling `ApplyDirtyObstacles()` instead of re-carving every frame.
     */
    class NavMeshObstacleManager
    {
      public:
        /**
         * @brief Access the global singleton instance.
         * @return Reference to the NavMeshObstacleManager.
         */
        static NavMeshObstacleManager& GetInstance();

        /**
         * @brief Set the NavMesh that obstacles will carve into.
         *
         * Must be called before any Add/Update/Remove operations. The manager
         * stores a non-owning pointer; the NavMeshData must outlive the manager
         * or be reset via another `SetNavMesh()` call.
         *
         * Calling this clears all tracked obstacles and restores the previous
         * NavMesh's triangle flags.
         *
         * @param navMesh  Pointer to the NavMeshData to operate on, or nullptr to detach.
         */
        void SetNavMesh(NavMeshData* navMesh);

        /**
         * @brief Get the currently attached NavMesh.
         * @return Non-owning pointer to the active NavMeshData, or nullptr.
         */
        NavMeshData* GetNavMesh() const;

        // =====================================================================
        // Obstacle CRUD
        // =====================================================================

        /**
         * @brief Add a new dynamic obstacle and carve it into the NavMesh.
         *
         * The obstacle is immediately carved: affected triangles have their
         * walkability flag (bit 0) cleared.
         *
         * @param desc  Descriptor defining the obstacle's shape, position, and size.
         * @return      Handle to the new obstacle. Use this handle for Update/Remove.
         *              Returns `InvalidObstacleHandle` if no NavMesh is attached.
         */
        ObstacleHandle AddObstacle(const ObstacleDesc& desc);

        /**
         * @brief Update an existing obstacle's descriptor and re-carve.
         *
         * The previously affected triangles are restored (unless still covered
         * by other obstacles), and the obstacle is re-carved at its new position.
         *
         * @param handle  Handle returned by `AddObstacle()`.
         * @param desc    New descriptor for the obstacle.
         * @return        `true` if the obstacle was found and updated; `false` if
         *                the handle is invalid.
         */
        bool UpdateObstacle(ObstacleHandle handle, const ObstacleDesc& desc);

        /**
         * @brief Remove a dynamic obstacle and restore affected NavMesh triangles.
         *
         * Triangles that were carved by this obstacle are restored to walkable
         * unless another active obstacle still overlaps them.
         *
         * @param handle  Handle returned by `AddObstacle()`.
         * @return        `true` if the obstacle was found and removed; `false` if
         *                the handle is invalid.
         */
        bool RemoveObstacle(ObstacleHandle handle);

        /**
         * @brief Mark an obstacle as dirty without immediately re-carving.
         *
         * Use this for obstacles that move every frame to batch carve updates.
         * Call `ApplyDirtyObstacles()` once per frame after all moves are complete.
         *
         * @param handle  Handle returned by `AddObstacle()`.
         * @param desc    New descriptor for the obstacle.
         * @return        `true` if the handle is valid.
         */
        bool MarkObstacleDirty(ObstacleHandle handle, const ObstacleDesc& desc);

        /**
         * @brief Re-carve all obstacles marked dirty since the last apply.
         *
         * More efficient than calling `UpdateObstacle()` individually for
         * many obstacles that move each frame because triangle restoration
         * and re-carving can be batched.
         */
        void ApplyDirtyObstacles();

        // =====================================================================
        // Query
        // =====================================================================

        /**
         * @brief Check whether a specific triangle is currently carved by any obstacle.
         *
         * @param triangleIndex  Index into the NavMesh's triangle array.
         * @return               `true` if the triangle is carved (non-walkable due to obstacles).
         */
        bool IsTriangleCarved(uint32_t triangleIndex) const;

        /**
         * @brief Get the number of active obstacles.
         * @return  Count of tracked obstacles.
         */
        size_t GetObstacleCount() const;

        /**
         * @brief Get the descriptor for an active obstacle.
         *
         * @param handle  Handle returned by `AddObstacle()`.
         * @param outDesc Output descriptor. Only written if the handle is valid.
         * @return        `true` if the handle is valid.
         */
        bool GetObstacleDesc(ObstacleHandle handle, ObstacleDesc& outDesc) const;

        /**
         * @brief Remove all obstacles and restore all carved triangles.
         */
        void Clear();

        // =====================================================================
        // Console integration
        // =====================================================================

        /**
         * @brief List all active obstacles (console integration).
         * @return Multi-line string describing each obstacle.
         */
        std::string Console_ListObstacles() const;

      private:
        NavMeshObstacleManager() = default;

        // -- Carving helpers --

        /**
         * @brief Compute which triangles an obstacle overlaps and carve them.
         *
         * Iterates all NavMesh triangles, tests overlap, clears the walkable
         * flag on affected triangles, and records them in the obstacle's record.
         *
         * @param record  The obstacle record to populate and carve.
         */
        void CarveObstacle(ObstacleRecord& record);

        /**
         * @brief Restore triangles previously carved by an obstacle.
         *
         * Re-enables walkability on triangles that are no longer covered by
         * ANY active obstacle (checks all other obstacles before restoring).
         *
         * @param record  The obstacle record whose triangles should be restored.
         */
        void RestoreObstacle(const ObstacleRecord& record);

        /**
         * @brief Test whether a point overlaps a box obstacle (with margin).
         *
         * @param point  World-space point to test.
         * @param desc   Obstacle descriptor (must be Box shape).
         * @return       `true` if the point is inside the box volume.
         */
        static bool PointInBox(const XMFLOAT3& point, const ObstacleDesc& desc);

        /**
         * @brief Test whether a point overlaps a cylinder obstacle (with margin).
         *
         * @param point  World-space point to test.
         * @param desc   Obstacle descriptor (must be Cylinder shape).
         * @return       `true` if the point is inside the cylinder volume.
         */
        static bool PointInCylinder(const XMFLOAT3& point, const ObstacleDesc& desc);

        /**
         * @brief Test whether a triangle overlaps an obstacle volume.
         *
         * Tests all three vertices and the centroid; if any is inside the obstacle
         * the triangle is considered affected.
         *
         * @param tri   The NavMesh triangle to test.
         * @param desc  The obstacle descriptor.
         * @param data  The NavMeshData containing vertex positions.
         * @return      `true` if the triangle overlaps the obstacle.
         */
        static bool TriangleOverlapsObstacle(const NavTriangle& tri, const ObstacleDesc& desc, const NavMeshData& data);

        /**
         * @brief Check if a triangle is covered by any obstacle OTHER than the excluded one.
         *
         * Used during restoration to avoid un-carving triangles still overlapped
         * by a different obstacle.
         *
         * @param triangleIndex   Index of the triangle to check.
         * @param excludeHandle   Handle of the obstacle being removed/updated (excluded from check).
         * @return                `true` if another obstacle covers this triangle.
         */
        bool IsTriangleCoveredByOther(uint32_t triangleIndex, ObstacleHandle excludeHandle) const;

        /** @brief Non-owning pointer to the active NavMesh. */
        NavMeshData* m_navMesh = nullptr;

        /** @brief Map from obstacle handle to internal record. */
        std::unordered_map<ObstacleHandle, ObstacleRecord> m_obstacles;

        /** @brief Obstacles added before a NavMesh was attached. Carve when SetNavMesh() is called. */
        std::vector<std::pair<ObstacleHandle, ObstacleDesc>> m_pendingObstacles;

        /** @brief Next handle to assign. Monotonically increasing starting at 1. */
        ObstacleHandle m_nextHandle = 1;

        /** @brief Walkable flag bit in NavTriangle::flags. */
        static constexpr uint16_t WalkableFlag = 0x0001;
    };

} // namespace Spark::AI
