/**
 * @file RecastDetourBackend.h
 * @brief Recast/Detour-powered NavMesh generation backend
 *
 * When SPARK_RECAST_AVAILABLE is defined, provides a high-quality voxel-based
 * NavMesh builder that replaces the engine's simple triangle-soup fallback.
 * The Recast pipeline (voxelization → region building → contour extraction →
 * polygon mesh) produces significantly better navigation meshes from complex
 * 3D geometry.
 *
 * @threadsafety Thread-safe — all state is local to the Build() call.
 */

#pragma once

#include "NavMeshTypes.h"
#include <memory>
#include <vector>

namespace Spark::AI
{

    /**
     * @brief Builds a NavMeshData using Recast's voxel-based pipeline.
     *
     * Full pipeline:
     * 1. Voxelize input geometry into heightfield
     * 2. Filter walkable surfaces (slope, height clearance, ledge detection)
     * 3. Build compact heightfield and erode by agent radius
     * 4. Build distance field and regions (watershed partitioning)
     * 5. Trace region contours and simplify
     * 6. Triangulate contours into polygon mesh
     * 7. Build detail mesh for precise height sampling
     * 8. Convert to SparkEngine NavMeshData format
     *
     * @param vertices  World-space triangle soup vertices.
     * @param indices   Triangle indices (every 3 = one triangle).
     * @param settings  Agent and voxelization parameters.
     * @return NavMeshData ready for pathfinding, or nullptr on failure.
     */
    std::unique_ptr<NavMeshData> BuildNavMeshWithRecast(const std::vector<XMFLOAT3>& vertices,
                                                        const std::vector<uint32_t>& indices,
                                                        const NavMeshBuildSettings& settings);

    /// @brief Returns true if the Recast backend is available at compile time.
    bool IsRecastAvailable();

} // namespace Spark::AI
