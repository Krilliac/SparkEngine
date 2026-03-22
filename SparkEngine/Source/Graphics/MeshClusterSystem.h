/**
 * @file MeshClusterSystem.h
 * @brief Nanite-inspired mesh cluster hierarchy with GPU-driven rendering
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a cluster-based mesh representation for GPU-driven rendering.
 * Each mesh is decomposed into clusters of 64-128 triangles, organized
 * in a DAG (directed acyclic graph) where each level is a progressively
 * coarser simplification.
 *
 * Features:
 * - MeshCluster: 64-128 triangles with bounding sphere and normal cone
 * - ClusterGroup: set of clusters sharing a parent in the DAG
 * - ClusterDAG: multi-level hierarchy for LOD selection
 * - LODSelection: screen-space error threshold traversal
 * - ClusterCuller: frustum + normal cone + HiZ occlusion tests
 * - VisibilityBuffer output for deferred material evaluation
 * - Software rasterizer for sub-pixel clusters
 *
 * @see MeshSystem.h, RenderGraph.h, HiZSystem.h, GPUDrivenRendering.h
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Spark::Graphics
{

    static constexpr uint32_t CLUSTER_MIN_TRIANGLES = 64;
    static constexpr uint32_t CLUSTER_MAX_TRIANGLES = 128;
    static constexpr uint32_t CLUSTER_MAX_VERTICES = 256;

    // =========================================================================
    // Bounding Sphere
    // =========================================================================

    struct BoundingSphere
    {
        float centerX = 0.0f;
        float centerY = 0.0f;
        float centerZ = 0.0f;
        float radius = 0.0f;

        /// @brief Test if a point is inside the sphere
        bool Contains(float px, float py, float pz) const
        {
            float dx = px - centerX;
            float dy = py - centerY;
            float dz = pz - centerZ;
            return (dx * dx + dy * dy + dz * dz) <= (radius * radius);
        }
    };

    // =========================================================================
    // Normal Cone (for backface cluster culling)
    // =========================================================================

    /**
     * @brief Cone bounding all triangle normals within a cluster
     *
     * If the viewing direction falls outside this cone, all triangles
     * in the cluster are guaranteed to be backfacing.
     */
    struct NormalCone
    {
        float axisX = 0.0f; ///< Cone axis direction (normalized)
        float axisY = 0.0f;
        float axisZ = 1.0f;
        float halfAngleCos = -1.0f; ///< Cosine of half-angle (-1 = full sphere)

        /// @brief Test if a view direction is outside the cone (all backfacing)
        bool IsBackfacing(float viewDirX, float viewDirY, float viewDirZ) const
        {
            // If half angle covers full sphere, never backface-cull
            if (halfAngleCos <= -1.0f)
                return false;

            float dot = viewDirX * axisX + viewDirY * axisY + viewDirZ * axisZ;
            return dot > halfAngleCos;
        }
    };

    // =========================================================================
    // Visibility Buffer Entry
    // =========================================================================

    /// @brief Per-pixel visibility buffer output for deferred shading
    struct VisibilityEntry
    {
        uint32_t clusterID = 0;
        uint32_t triangleID = 0;
        float baryU = 0.0f;
        float baryV = 0.0f;
    };

    // =========================================================================
    // Mesh Cluster
    // =========================================================================

    /**
     * @brief A cluster of 64-128 triangles from a mesh
     *
     * Self-contained geometry unit with spatial and normal bounds
     * for efficient culling.
     */
    struct MeshCluster
    {
        uint32_t clusterID = 0;
        uint32_t triangleCount = 0;
        uint32_t vertexCount = 0;

        /// @brief Triangle indices (3 per triangle, into local vertex buffer)
        std::vector<uint32_t> indices;

        /// @brief Local vertex positions (interleaved XYZ)
        std::vector<float> positions;

        BoundingSphere bounds;
        NormalCone normalCone;

        /// @brief Simplification error metric for this cluster's LOD level
        float simplificationError = 0.0f;

        /// @brief LOD level (0 = finest detail)
        uint32_t lodLevel = 0;

        /// @brief Index of the parent cluster group in the DAG (-1 if root)
        int32_t parentGroupIndex = -1;
    };

    // =========================================================================
    // Cluster Group
    // =========================================================================

    /**
     * @brief A group of clusters that share a common parent in the DAG
     *
     * During LOD selection, an entire group is either rendered or
     * replaced by its parent group at a coarser level.
     */
    struct ClusterGroup
    {
        uint32_t groupID = 0;
        std::vector<uint32_t> clusterIndices; ///< Indices into ClusterDAG's cluster array
        BoundingSphere bounds;
        float maxError = 0.0f; ///< Maximum simplification error among children

        /// @brief Index of the parent group (-1 if top level)
        int32_t parentGroupIndex = -1;

        /// @brief LOD level of this group
        uint32_t lodLevel = 0;
    };

    // =========================================================================
    // Frustum Planes
    // =========================================================================

    /// @brief Six frustum planes for culling (ax + by + cz + d = 0)
    struct FrustumPlanes
    {
        float planes[6][4] = {};

        /// @brief Test if a bounding sphere is outside the frustum
        bool IsSphereOutside(const BoundingSphere& sphere) const
        {
            for (int i = 0; i < 6; ++i)
            {
                float dist = planes[i][0] * sphere.centerX + planes[i][1] * sphere.centerY +
                             planes[i][2] * sphere.centerZ + planes[i][3];
                if (dist < -sphere.radius)
                    return true;
            }
            return false;
        }
    };

    // =========================================================================
    // Cluster DAG (Directed Acyclic Graph)
    // =========================================================================

    /**
     * @brief Multi-level cluster hierarchy for a mesh
     *
     * Level 0 contains the original full-detail clusters. Each
     * subsequent level merges and simplifies clusters from the
     * previous level. LOD selection traverses this DAG.
     */
    class ClusterDAG
    {
      public:
        ClusterDAG() = default;
        ~ClusterDAG() = default;

        /// @brief Add a cluster to the DAG
        uint32_t AddCluster(MeshCluster cluster)
        {
            uint32_t idx = static_cast<uint32_t>(m_clusters.size());
            cluster.clusterID = idx;
            m_clusters.push_back(std::move(cluster));
            return idx;
        }

        /// @brief Add a cluster group
        uint32_t AddGroup(ClusterGroup group)
        {
            uint32_t idx = static_cast<uint32_t>(m_groups.size());
            group.groupID = idx;
            m_groups.push_back(std::move(group));
            return idx;
        }

        /// @brief Build the hierarchy bounds (bottom-up)
        void BuildBounds()
        {
            // Compute group bounds from constituent clusters
            for (auto& group : m_groups)
            {
                if (group.clusterIndices.empty())
                    continue;

                float minX = 1e30f, minY = 1e30f, minZ = 1e30f;
                float maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;
                float maxErr = 0.0f;

                for (uint32_t ci : group.clusterIndices)
                {
                    const MeshCluster& c = m_clusters[ci];
                    float r = c.bounds.radius;
                    minX = std::min(minX, c.bounds.centerX - r);
                    minY = std::min(minY, c.bounds.centerY - r);
                    minZ = std::min(minZ, c.bounds.centerZ - r);
                    maxX = std::max(maxX, c.bounds.centerX + r);
                    maxY = std::max(maxY, c.bounds.centerY + r);
                    maxZ = std::max(maxZ, c.bounds.centerZ + r);
                    maxErr = std::max(maxErr, c.simplificationError);
                }

                group.bounds.centerX = (minX + maxX) * 0.5f;
                group.bounds.centerY = (minY + maxY) * 0.5f;
                group.bounds.centerZ = (minZ + maxZ) * 0.5f;

                float dx = maxX - minX;
                float dy = maxY - minY;
                float dz = maxZ - minZ;
                group.bounds.radius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
                group.maxError = maxErr;
            }
        }

        /// @brief Get all clusters
        const std::vector<MeshCluster>& GetClusters() const { return m_clusters; }

        /// @brief Get all groups
        const std::vector<ClusterGroup>& GetGroups() const { return m_groups; }

        /// @brief Get the total number of LOD levels
        uint32_t GetLODLevelCount() const { return m_lodLevelCount; }

        /// @brief Set the total number of LOD levels
        void SetLODLevelCount(uint32_t count) { m_lodLevelCount = count; }

        /**
         * @brief Build the DAG from raw mesh data
         *
         * Creates level-0 clusters by splitting the mesh into groups of
         * 64-128 triangles, then computes bounding spheres and normal cones.
         *
         * @param indices      Triangle index buffer (3 per triangle)
         * @param positions    Vertex positions (interleaved XYZ)
         * @param indexCount   Number of indices
         * @param vertexCount  Number of vertices
         */
        void Build(const uint32_t* indices, const float* positions,
                   uint32_t indexCount, uint32_t vertexCount);

      private:
        std::vector<MeshCluster> m_clusters;
        std::vector<ClusterGroup> m_groups;
        uint32_t m_lodLevelCount = 1;
    };

    // =========================================================================
    // Cluster Builder
    // =========================================================================

    /**
     * @brief Build mesh clusters from raw index and position data
     *
     * Splits the mesh into clusters of 64-128 triangles, computes bounding
     * spheres and normal cones for each cluster.
     */
    class ClusterBuilder
    {
      public:
        /**
         * @brief Build level-0 clusters from a triangle mesh
         *
         * @param indices      Triangle index buffer (3 indices per triangle)
         * @param positions    Vertex positions (interleaved XYZ)
         * @param indexCount   Number of indices
         * @param vertexCount  Number of vertices
         * @return Vector of clusters covering the entire mesh
         */
        static std::vector<MeshCluster> BuildClusters(const uint32_t* indices, const float* positions,
                                                       uint32_t indexCount, uint32_t vertexCount)
        {
            std::vector<MeshCluster> clusters;
            uint32_t triangleCount = indexCount / 3;
            uint32_t clusterTriCount = CLUSTER_MAX_TRIANGLES;

            for (uint32_t triStart = 0; triStart < triangleCount; triStart += clusterTriCount)
            {
                uint32_t triEnd = std::min(triStart + clusterTriCount, triangleCount);
                uint32_t numTris = triEnd - triStart;

                MeshCluster cluster;
                cluster.triangleCount = numTris;
                cluster.lodLevel = 0;
                cluster.simplificationError = 0.0f;

                // Gather unique vertices and build local index buffer
                std::vector<uint32_t> vertexMap(vertexCount, UINT32_MAX);
                uint32_t localVertCount = 0;

                for (uint32_t t = triStart; t < triEnd; ++t)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        uint32_t globalIdx = indices[t * 3 + k];
                        if (vertexMap[globalIdx] == UINT32_MAX)
                        {
                            vertexMap[globalIdx] = localVertCount;
                            cluster.positions.push_back(positions[globalIdx * 3 + 0]);
                            cluster.positions.push_back(positions[globalIdx * 3 + 1]);
                            cluster.positions.push_back(positions[globalIdx * 3 + 2]);
                            localVertCount++;
                        }
                        cluster.indices.push_back(vertexMap[globalIdx]);
                    }
                }
                cluster.vertexCount = localVertCount;

                // Compute bounding sphere (centroid + max radius)
                ComputeBoundingSphere(cluster);

                // Compute normal cone enclosing all triangle normals
                ComputeNormalCone(cluster);

                clusters.push_back(std::move(cluster));
            }

            return clusters;
        }

      private:
        /// @brief Compute a bounding sphere for a cluster from its positions
        static void ComputeBoundingSphere(MeshCluster& cluster)
        {
            float cx = 0.0f, cy = 0.0f, cz = 0.0f;
            for (uint32_t v = 0; v < cluster.vertexCount; ++v)
            {
                cx += cluster.positions[v * 3 + 0];
                cy += cluster.positions[v * 3 + 1];
                cz += cluster.positions[v * 3 + 2];
            }
            float inv = 1.0f / std::max(1u, cluster.vertexCount);
            cx *= inv;
            cy *= inv;
            cz *= inv;

            float maxDist2 = 0.0f;
            for (uint32_t v = 0; v < cluster.vertexCount; ++v)
            {
                float dx = cluster.positions[v * 3 + 0] - cx;
                float dy = cluster.positions[v * 3 + 1] - cy;
                float dz = cluster.positions[v * 3 + 2] - cz;
                maxDist2 = std::max(maxDist2, dx * dx + dy * dy + dz * dz);
            }

            cluster.bounds.centerX = cx;
            cluster.bounds.centerY = cy;
            cluster.bounds.centerZ = cz;
            cluster.bounds.radius = std::sqrt(maxDist2);
        }

        /**
         * @brief Compute normal cone enclosing all triangle normals
         *
         * Averages all triangle normals to find the cone axis, then
         * finds the maximum deviation angle from that axis.
         */
        static void ComputeNormalCone(MeshCluster& cluster)
        {
            float avgNX = 0.0f, avgNY = 0.0f, avgNZ = 0.0f;
            uint32_t triCount = cluster.triangleCount;

            for (uint32_t t = 0; t < triCount; ++t)
            {
                uint32_t i0 = cluster.indices[t * 3 + 0];
                uint32_t i1 = cluster.indices[t * 3 + 1];
                uint32_t i2 = cluster.indices[t * 3 + 2];

                float ax = cluster.positions[i1 * 3 + 0] - cluster.positions[i0 * 3 + 0];
                float ay = cluster.positions[i1 * 3 + 1] - cluster.positions[i0 * 3 + 1];
                float az = cluster.positions[i1 * 3 + 2] - cluster.positions[i0 * 3 + 2];

                float bx = cluster.positions[i2 * 3 + 0] - cluster.positions[i0 * 3 + 0];
                float by = cluster.positions[i2 * 3 + 1] - cluster.positions[i0 * 3 + 1];
                float bz = cluster.positions[i2 * 3 + 2] - cluster.positions[i0 * 3 + 2];

                // Cross product for face normal
                float nx = ay * bz - az * by;
                float ny = az * bx - ax * bz;
                float nz = ax * by - ay * bx;

                float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (len > 0.0001f)
                {
                    avgNX += nx / len;
                    avgNY += ny / len;
                    avgNZ += nz / len;
                }
            }

            // Normalize average normal to get cone axis
            float avgLen = std::sqrt(avgNX * avgNX + avgNY * avgNY + avgNZ * avgNZ);
            if (avgLen < 0.0001f)
            {
                // Normals cancel out - cone spans full sphere
                cluster.normalCone = {0.0f, 0.0f, 1.0f, -1.0f};
                return;
            }

            float axisX = avgNX / avgLen;
            float axisY = avgNY / avgLen;
            float axisZ = avgNZ / avgLen;

            // Find maximum deviation from the axis
            float minDot = 1.0f;
            for (uint32_t t = 0; t < triCount; ++t)
            {
                uint32_t i0 = cluster.indices[t * 3 + 0];
                uint32_t i1 = cluster.indices[t * 3 + 1];
                uint32_t i2 = cluster.indices[t * 3 + 2];

                float eax = cluster.positions[i1 * 3 + 0] - cluster.positions[i0 * 3 + 0];
                float eay = cluster.positions[i1 * 3 + 1] - cluster.positions[i0 * 3 + 1];
                float eaz = cluster.positions[i1 * 3 + 2] - cluster.positions[i0 * 3 + 2];

                float ebx = cluster.positions[i2 * 3 + 0] - cluster.positions[i0 * 3 + 0];
                float eby = cluster.positions[i2 * 3 + 1] - cluster.positions[i0 * 3 + 1];
                float ebz = cluster.positions[i2 * 3 + 2] - cluster.positions[i0 * 3 + 2];

                float nx = eay * ebz - eaz * eby;
                float ny = eaz * ebx - eax * ebz;
                float nz = eax * eby - eay * ebx;

                float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (len > 0.0001f)
                {
                    float dot = (nx * axisX + ny * axisY + nz * axisZ) / len;
                    minDot = std::min(minDot, dot);
                }
            }

            cluster.normalCone.axisX = axisX;
            cluster.normalCone.axisY = axisY;
            cluster.normalCone.axisZ = axisZ;
            cluster.normalCone.halfAngleCos = minDot;
        }
    };

    // =========================================================================
    // LOD Selection
    // =========================================================================

    /**
     * @brief Select which clusters to render based on screen-space error
     *
     * Traverses the ClusterDAG from coarsest to finest. At each level,
     * if the projected error exceeds the threshold, refine to children.
     * The result is the "cut" through the DAG.
     */
    class LODSelector
    {
      public:
        /**
         * @brief Select visible clusters at the appropriate LOD
         *
         * @param dag              The cluster hierarchy
         * @param errorThreshold   Maximum screen-space error in pixels
         * @param cameraX/Y/Z     Camera world position
         * @param screenHeight     Viewport height in pixels
         * @param fovY             Vertical field of view in radians
         * @return Indices of selected clusters to render
         */
        static std::vector<uint32_t> SelectClusters(const ClusterDAG& dag, float errorThreshold, float cameraX,
                                                    float cameraY, float cameraZ, float screenHeight, float fovY)
        {
            std::vector<uint32_t> selected;
            float projFactor = screenHeight / (2.0f * std::tan(fovY * 0.5f));

            const auto& clusters = dag.GetClusters();

            for (const auto& cluster : clusters)
            {
                // Compute distance from camera to cluster center
                float dx = cluster.bounds.centerX - cameraX;
                float dy = cluster.bounds.centerY - cameraY;
                float dz = cluster.bounds.centerZ - cameraZ;
                float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                distance = std::max(distance, 0.001f);

                // Project simplification error to screen space
                float screenError = (cluster.simplificationError * projFactor) / distance;

                // If this cluster's error is acceptable and its parent's error
                // would be too large, include it in the cut
                bool thisLevelOK = (screenError <= errorThreshold);

                // Check parent level error
                bool parentTooCoarse = true;
                if (cluster.parentGroupIndex >= 0)
                {
                    const auto& groups = dag.GetGroups();
                    const ClusterGroup& parentGroup = groups[cluster.parentGroupIndex];
                    float parentScreenError = (parentGroup.maxError * projFactor) / distance;
                    parentTooCoarse = (parentScreenError > errorThreshold);
                }

                if (thisLevelOK && parentTooCoarse)
                {
                    selected.push_back(cluster.clusterID);
                }
                // If this is the finest level and still too detailed, include anyway
                else if (cluster.lodLevel == 0 && !thisLevelOK)
                {
                    selected.push_back(cluster.clusterID);
                }
            }

            return selected;
        }
    };

    // =========================================================================
    // Cluster Culler
    // =========================================================================

    /**
     * @brief Multi-test cluster culling pipeline
     *
     * Tests each cluster against frustum planes, normal cone backface
     * culling, and hierarchical Z-buffer occlusion.
     */
    class ClusterCuller
    {
      public:
        /**
         * @brief Cull a set of clusters and return visible ones
         *
         * @param clusters       Array of cluster indices to test
         * @param dag            The cluster hierarchy (for bounds/normals)
         * @param frustum        Camera frustum planes
         * @param cameraDirX/Y/Z Camera forward direction
         * @param hizBuffer      Hierarchical Z-buffer (mip 0 = full res)
         * @param hizWidth       Width of mip 0
         * @param hizHeight      Height of mip 0
         * @param projMatrix     4x4 projection matrix (row-major)
         * @return Indices of visible clusters
         */
        static std::vector<uint32_t> CullClusters(const std::vector<uint32_t>& clusters, const ClusterDAG& dag,
                                                  const FrustumPlanes& frustum, float cameraDirX, float cameraDirY,
                                                  float cameraDirZ, const float* hizBuffer, uint32_t hizWidth,
                                                  uint32_t hizHeight, const float projMatrix[16])
        {
            std::vector<uint32_t> visible;
            visible.reserve(clusters.size());

            const auto& allClusters = dag.GetClusters();

            for (uint32_t ci : clusters)
            {
                const MeshCluster& cluster = allClusters[ci];

                // Test 1: Frustum culling
                if (frustum.IsSphereOutside(cluster.bounds))
                    continue;

                // Test 2: Normal cone backface culling
                if (cluster.normalCone.IsBackfacing(cameraDirX, cameraDirY, cameraDirZ))
                    continue;

                // Test 3: HiZ occlusion test
                if (hizBuffer && !PassesHiZTest(cluster.bounds, hizBuffer, hizWidth, hizHeight, projMatrix))
                    continue;

                visible.push_back(ci);
            }

            return visible;
        }

      private:
        /**
         * @brief Hierarchical Z-buffer occlusion test
         *
         * Projects the cluster's bounding sphere to screen space,
         * selects the appropriate HiZ mip level, and tests against
         * the conservative depth.
         */
        static bool PassesHiZTest(const BoundingSphere& bounds, const float* hizBuffer, uint32_t hizWidth,
                                  uint32_t hizHeight, const float projMatrix[16])
        {
            // Project bounding sphere center to clip space
            float cx = bounds.centerX;
            float cy = bounds.centerY;
            float cz = bounds.centerZ;

            float clipX = projMatrix[0] * cx + projMatrix[4] * cy + projMatrix[8] * cz + projMatrix[12];
            float clipY = projMatrix[1] * cx + projMatrix[5] * cy + projMatrix[9] * cz + projMatrix[13];
            float clipW = projMatrix[3] * cx + projMatrix[7] * cy + projMatrix[11] * cz + projMatrix[15];

            if (clipW <= 0.0f)
                return true; // Behind camera, let frustum test handle it

            float ndcX = clipX / clipW;
            float ndcY = clipY / clipW;

            // Screen-space radius
            float screenRadius = bounds.radius * projMatrix[0] / clipW;

            // Select HiZ mip level based on projected size
            float projectedPixels = screenRadius * hizWidth;
            int mipLevel = std::max(0, static_cast<int>(std::log2(projectedPixels + 1.0f)));
            uint32_t mipWidth = std::max(1u, hizWidth >> mipLevel);
            uint32_t mipHeight = std::max(1u, hizHeight >> mipLevel);

            // Convert NDC to texel coordinates at the selected mip
            float u = (ndcX * 0.5f + 0.5f) * mipWidth;
            float v = (ndcY * -0.5f + 0.5f) * mipHeight;

            int px = std::clamp(static_cast<int>(u), 0, static_cast<int>(mipWidth - 1));
            int py = std::clamp(static_cast<int>(v), 0, static_cast<int>(mipHeight - 1));

            // Compute mip offset in the flattened HiZ buffer
            uint32_t mipOffset = 0;
            for (int m = 0; m < mipLevel; ++m)
            {
                uint32_t mw = std::max(1u, hizWidth >> m);
                uint32_t mh = std::max(1u, hizHeight >> m);
                mipOffset += mw * mh;
            }

            float hizDepth = hizBuffer[mipOffset + py * mipWidth + px];

            // Compute conservative near depth of the bounding sphere
            float sphereNearZ = (cz - bounds.radius);
            float sphereClipZ = projMatrix[10] * sphereNearZ + projMatrix[14];
            float sphereClipW = projMatrix[11] * sphereNearZ + projMatrix[15];
            float sphereNDCZ = (sphereClipW > 0.0f) ? sphereClipZ / sphereClipW : 0.0f;

            // If sphere's near depth is behind the HiZ depth, it is occluded
            return sphereNDCZ <= hizDepth;
        }
    };

    // =========================================================================
    // Software Rasterizer (for sub-pixel clusters)
    // =========================================================================

    /**
     * @brief Software rasterizer for clusters too small for hardware raster
     *
     * When a cluster projects to very few pixels on screen, hardware
     * rasterization is inefficient. This software path rasterizes directly
     * into the visibility buffer.
     */
    class SoftwareRasterizer
    {
      public:
        /**
         * @brief Rasterize a cluster into the visibility buffer
         *
         * @param cluster      The cluster to rasterize
         * @param projMatrix   4x4 projection matrix (row-major)
         * @param viewMatrix   4x4 view matrix (row-major)
         * @param visBuffer    Output visibility buffer (width * height)
         * @param depthBuffer  Depth buffer for depth test (width * height)
         * @param width        Framebuffer width
         * @param height       Framebuffer height
         */
        static void RasterizeCluster(const MeshCluster& cluster, const float projMatrix[16], const float viewMatrix[16],
                                     VisibilityEntry* visBuffer, float* depthBuffer, uint32_t width, uint32_t height)
        {
            if (cluster.indices.empty() || cluster.positions.empty())
                return;

            // Transform and project all vertices
            std::vector<float> screenX(cluster.vertexCount);
            std::vector<float> screenY(cluster.vertexCount);
            std::vector<float> screenZ(cluster.vertexCount);

            for (uint32_t v = 0; v < cluster.vertexCount; ++v)
            {
                float vx = cluster.positions[v * 3 + 0];
                float vy = cluster.positions[v * 3 + 1];
                float vz = cluster.positions[v * 3 + 2];

                // View transform
                float ex = viewMatrix[0] * vx + viewMatrix[4] * vy + viewMatrix[8] * vz + viewMatrix[12];
                float ey = viewMatrix[1] * vx + viewMatrix[5] * vy + viewMatrix[9] * vz + viewMatrix[13];
                float ez = viewMatrix[2] * vx + viewMatrix[6] * vy + viewMatrix[10] * vz + viewMatrix[14];

                // Projection
                float cx = projMatrix[0] * ex + projMatrix[4] * ey + projMatrix[8] * ez + projMatrix[12];
                float cy = projMatrix[1] * ex + projMatrix[5] * ey + projMatrix[9] * ez + projMatrix[13];
                float cz = projMatrix[2] * ex + projMatrix[6] * ey + projMatrix[10] * ez + projMatrix[14];
                float cw = projMatrix[3] * ex + projMatrix[7] * ey + projMatrix[11] * ez + projMatrix[15];

                if (cw > 0.0001f)
                {
                    float invW = 1.0f / cw;
                    screenX[v] = (cx * invW * 0.5f + 0.5f) * width;
                    screenY[v] = (cy * invW * -0.5f + 0.5f) * height;
                    screenZ[v] = cz * invW;
                }
                else
                {
                    screenX[v] = -1.0f;
                    screenY[v] = -1.0f;
                    screenZ[v] = 1.0f;
                }
            }

            // Rasterize each triangle
            for (uint32_t t = 0; t < cluster.triangleCount; ++t)
            {
                uint32_t i0 = cluster.indices[t * 3 + 0];
                uint32_t i1 = cluster.indices[t * 3 + 1];
                uint32_t i2 = cluster.indices[t * 3 + 2];

                RasterizeTriangle(cluster.clusterID, t, screenX[i0], screenY[i0], screenZ[i0], screenX[i1], screenY[i1],
                                  screenZ[i1], screenX[i2], screenY[i2], screenZ[i2], visBuffer, depthBuffer, width,
                                  height);
            }
        }

      private:
        /// @brief Rasterize a single screen-space triangle with barycentric interpolation
        static void RasterizeTriangle(uint32_t clusterID, uint32_t triangleID, float x0, float y0, float z0, float x1,
                                      float y1, float z1, float x2, float y2, float z2, VisibilityEntry* visBuffer,
                                      float* depthBuffer, uint32_t width, uint32_t height)
        {
            // Bounding box
            int minX = std::max(0, static_cast<int>(std::min({x0, x1, x2})));
            int minY = std::max(0, static_cast<int>(std::min({y0, y1, y2})));
            int maxX = std::min(static_cast<int>(width - 1), static_cast<int>(std::max({x0, x1, x2}) + 1.0f));
            int maxY = std::min(static_cast<int>(height - 1), static_cast<int>(std::max({y0, y1, y2}) + 1.0f));

            // Edge equation setup
            float dx01 = x1 - x0, dy01 = y1 - y0;
            float dx12 = x2 - x1, dy12 = y2 - y1;
            float dx20 = x0 - x2, dy20 = y0 - y2;

            // Triangle area (2x)
            float area = dx01 * (y2 - y0) - dy01 * (x2 - x0);
            if (std::abs(area) < 0.001f)
                return; // Degenerate

            float invArea = 1.0f / area;

            for (int py = minY; py <= maxY; ++py)
            {
                for (int px = minX; px <= maxX; ++px)
                {
                    float sx = px + 0.5f;
                    float sy = py + 0.5f;

                    // Barycentric coordinates via edge functions
                    float w0 = (sx - x1) * dy12 - (sy - y1) * dx12;
                    float w1 = (sx - x2) * dy20 - (sy - y2) * dx20;
                    float w2 = (sx - x0) * dy01 - (sy - y0) * dx01;

                    // Inside test (top-left fill rule approximation)
                    if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
                    {
                        float bary0 = w0 * invArea;
                        float bary1 = w1 * invArea;
                        float bary2 = w2 * invArea;

                        float depth = z0 * bary0 + z1 * bary1 + z2 * bary2;

                        uint32_t pixelIdx = py * width + px;

                        // Depth test (less-than)
                        if (depth < depthBuffer[pixelIdx])
                        {
                            depthBuffer[pixelIdx] = depth;
                            visBuffer[pixelIdx].clusterID = clusterID;
                            visBuffer[pixelIdx].triangleID = triangleID;
                            visBuffer[pixelIdx].baryU = bary1;
                            visBuffer[pixelIdx].baryV = bary2;
                        }
                    }
                }
            }
        }
    };

} // namespace Spark::Graphics
