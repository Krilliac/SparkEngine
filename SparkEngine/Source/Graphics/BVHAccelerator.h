/**
 * @file BVHAccelerator.h
 * @brief SAH-based Bounding Volume Hierarchy for hierarchical frustum culling
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a CPU-side binary BVH using Surface Area Heuristic (SAH) that
 * enables hierarchical frustum and
 * occlusion culling — entire subtrees are rejected if their bounding box
 * is outside the frustum, dramatically reducing per-object test count.
 *
 * SAH produces optimal split decisions that minimize the expected cost of
 * ray/frustum traversal, outperforming median-split or object-median
 * alternatives for spatially uneven distributions typical in FPS levels.
 *
 * ## Usage
 * @code
 *   std::vector<BVHPrimitive> prims;
 *   for (auto& mesh : sceneMeshes)
 *   {
 *       prims.push_back({mesh.id, mesh.bounds});
 *   }
 *
 *   BVHAccelerator bvh;
 *   bvh.Build(prims);
 *
 *   // Each frame
 *   Frustum frustum;
 *   frustum.ExtractPlanes(viewProj);
 *
 *   auto visible = bvh.FrustumQuery(frustum);
 *   // visible contains only object IDs that passed culling
 * @endcode
 *
 * @see FrustumCulling.h, Octree.h, RenderSystem
 *
 * @note **Lifecycle-only activation (Phase L)** — BVHAccelerator is referenced in
 * SceneRenderer comments but FrustumQuery/Cull are not called from any active render path.
 * The BVH is not populated with scene primitives. Full activation requires the scene renderer
 * to build the BVH from renderable AABBs each frame and use FrustumQuery for hierarchical culling.
 *
 */

#pragma once

#include "../Core/Platform.h"
#include "FrustumCulling.h"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS

namespace Spark::Graphics
{

    /**
     * @brief Input primitive for BVH construction.
     */
    struct BVHPrimitive
    {
        uint32_t objectId = 0;
        AABB bounds;
    };

    /**
     * @brief SAH-based BVH accelerator for hierarchical spatial queries.
     *
     * Uses a flattened array representation for cache-friendly traversal.
     * Each internal node stores the AABB of its children and an offset
     * to the second child (first child is always at index + 1).
     */
    class BVHAccelerator
    {
      public:
        BVHAccelerator() = default;
        ~BVHAccelerator() = default;

        /**
         * @brief Build the BVH from a list of primitives.
         *
         * Uses SAH to find optimal split planes. After construction, the
         * input vector may be reordered.
         *
         * @param primitives  List of objects with bounding boxes.
         * @param maxLeafSize Maximum primitives per leaf (default 4).
         */
        void Build(std::vector<BVHPrimitive>& primitives, uint32_t maxLeafSize = 4)
        {
            m_nodes.clear();
            m_leafPrimitives.clear();
            m_maxLeafSize = maxLeafSize;

            if (primitives.empty())
                return;

            // Reserve estimated node count (2N - 1 for N leaves)
            m_nodes.reserve(primitives.size() * 2);

            BuildRecursive(primitives, 0, static_cast<uint32_t>(primitives.size()));
        }

        /**
         * @brief Query all primitives whose bounding boxes intersect the frustum.
         *
         * Traverses the BVH, pruning entire subtrees when their bounding
         * box is outside the frustum.
         *
         * @param frustum  The camera frustum to test against.
         * @return Vector of object IDs that are potentially visible.
         */
        std::vector<uint32_t> FrustumQuery(const Frustum& frustum) const
        {
            std::vector<uint32_t> results;
            if (m_nodes.empty())
                return results;

            results.reserve(m_leafPrimitives.size());
            FrustumQueryRecursive(0, frustum, results);
            return results;
        }

        /**
         * @brief Query all primitives whose bounding boxes intersect an AABB.
         */
        std::vector<uint32_t> AABBQuery(const AABB& queryBox) const
        {
            std::vector<uint32_t> results;
            if (m_nodes.empty())
                return results;

            results.reserve(32);
            AABBQueryRecursive(0, queryBox, results);
            return results;
        }

        /**
         * @brief Get the total node count (for profiling).
         */
        uint32_t GetNodeCount() const { return static_cast<uint32_t>(m_nodes.size()); }

        /**
         * @brief Get the total primitive count stored in leaves.
         */
        uint32_t GetPrimitiveCount() const { return static_cast<uint32_t>(m_leafPrimitives.size()); }

        /**
         * @brief Get the root bounding box of the entire scene.
         */
        AABB GetRootBounds() const
        {
            if (m_nodes.empty())
                return {};
            return m_nodes[0].bounds;
        }

        /**
         * @brief Check if the BVH has been built.
         */
        bool IsBuilt() const { return !m_nodes.empty(); }

      private:
        struct BVHNode
        {
            AABB bounds;
            uint32_t primOffset = 0;  ///< Offset into m_leafPrimitives (leaf only)
            uint32_t primCount = 0;   ///< Number of primitives (0 = internal node)
            uint32_t secondChild = 0; ///< Index of second child (first child = this + 1)
            uint8_t splitAxis = 0;    ///< Axis used for split (0=X, 1=Y, 2=Z)
        };

        static constexpr uint32_t SAH_BUCKET_COUNT = 12;
        static constexpr float TRAVERSAL_COST = 1.0f;
        static constexpr float INTERSECTION_COST = 1.0f;

        struct SAHBucket
        {
            uint32_t count = 0;
            AABB bounds;
        };

        static AABB UnionAABB(const AABB& a, const AABB& b)
        {
            return {{std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)},
                    {std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)}};
        }

        static AABB UnionPoint(const AABB& a, const DirectX::XMFLOAT3& p)
        {
            return {{std::min(a.min.x, p.x), std::min(a.min.y, p.y), std::min(a.min.z, p.z)},
                    {std::max(a.max.x, p.x), std::max(a.max.y, p.y), std::max(a.max.z, p.z)}};
        }

        static float SurfaceArea(const AABB& b)
        {
            float dx = b.max.x - b.min.x;
            float dy = b.max.y - b.min.y;
            float dz = b.max.z - b.min.z;
            return 2.0f * (dx * dy + dx * dz + dy * dz);
        }

        static float GetAxis(const DirectX::XMFLOAT3& v, int axis)
        {
            switch (axis)
            {
            case 0:
                return v.x;
            case 1:
                return v.y;
            default:
                return v.z;
            }
        }

        static bool AABBIntersects(const AABB& a, const AABB& b)
        {
            return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
                   a.min.z <= b.max.z && a.max.z >= b.min.z;
        }

        uint32_t BuildRecursive(std::vector<BVHPrimitive>& prims, uint32_t start, uint32_t end)
        {
            uint32_t nodeIndex = static_cast<uint32_t>(m_nodes.size());
            m_nodes.emplace_back();
            uint32_t primCount = end - start;

            // Compute bounds of all primitives in this range
            AABB totalBounds = prims[start].bounds;
            for (uint32_t i = start + 1; i < end; ++i)
            {
                totalBounds = UnionAABB(totalBounds, prims[i].bounds);
            }
            m_nodes[nodeIndex].bounds = totalBounds;

            // Leaf node
            if (primCount <= m_maxLeafSize)
            {
                m_nodes[nodeIndex].primOffset = static_cast<uint32_t>(m_leafPrimitives.size());
                m_nodes[nodeIndex].primCount = primCount;
                for (uint32_t i = start; i < end; ++i)
                {
                    m_leafPrimitives.push_back(prims[i].objectId);
                }
                return nodeIndex;
            }

            // Compute centroid bounds for SAH
            AABB centroidBounds;
            DirectX::XMFLOAT3 firstCenter = prims[start].bounds.Center();
            centroidBounds.min = firstCenter;
            centroidBounds.max = firstCenter;
            for (uint32_t i = start + 1; i < end; ++i)
            {
                DirectX::XMFLOAT3 c = prims[i].bounds.Center();
                centroidBounds = UnionPoint(centroidBounds, c);
            }

            // Find best split axis and position using SAH
            int bestAxis = -1;
            float bestCost = std::numeric_limits<float>::max();
            uint32_t bestSplit = (start + end) / 2;

            float parentArea = SurfaceArea(totalBounds);
            if (parentArea <= 0.0f)
                parentArea = 1.0f;

            for (int axis = 0; axis < 3; ++axis)
            {
                float axisMin = GetAxis(centroidBounds.min, axis);
                float axisMax = GetAxis(centroidBounds.max, axis);
                float extent = axisMax - axisMin;

                if (extent <= 0.0f)
                    continue;

                // Initialize SAH buckets
                SAHBucket buckets[SAH_BUCKET_COUNT];
                for (auto& b : buckets)
                {
                    b.count = 0;
                    b.bounds = {{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                 std::numeric_limits<float>::max()},
                                {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                                 std::numeric_limits<float>::lowest()}};
                }

                for (uint32_t i = start; i < end; ++i)
                {
                    DirectX::XMFLOAT3 c = prims[i].bounds.Center();
                    float t = (GetAxis(c, axis) - axisMin) / extent;
                    int b = static_cast<int>(t * SAH_BUCKET_COUNT);
                    if (b >= static_cast<int>(SAH_BUCKET_COUNT))
                        b = SAH_BUCKET_COUNT - 1;
                    buckets[b].count++;
                    buckets[b].bounds = UnionAABB(buckets[b].bounds, prims[i].bounds);
                }

                // Evaluate SAH cost for each split position
                for (uint32_t split = 1; split < SAH_BUCKET_COUNT; ++split)
                {
                    AABB leftBounds = buckets[0].bounds;
                    uint32_t leftCount = buckets[0].count;
                    for (uint32_t j = 1; j < split; ++j)
                    {
                        if (buckets[j].count > 0)
                            leftBounds = UnionAABB(leftBounds, buckets[j].bounds);
                        leftCount += buckets[j].count;
                    }

                    AABB rightBounds = buckets[split].bounds;
                    uint32_t rightCount = buckets[split].count;
                    for (uint32_t j = split + 1; j < SAH_BUCKET_COUNT; ++j)
                    {
                        if (buckets[j].count > 0)
                            rightBounds = UnionAABB(rightBounds, buckets[j].bounds);
                        rightCount += buckets[j].count;
                    }

                    if (leftCount == 0 || rightCount == 0)
                        continue;

                    float cost = TRAVERSAL_COST +
                                 INTERSECTION_COST *
                                     (leftCount * SurfaceArea(leftBounds) + rightCount * SurfaceArea(rightBounds)) /
                                     parentArea;

                    if (cost < bestCost)
                    {
                        bestCost = cost;
                        bestAxis = axis;
                        // Find the partition point based on bucket split
                        float splitPos = axisMin + extent * (static_cast<float>(split) / SAH_BUCKET_COUNT);
                        auto midIt = std::partition(prims.begin() + start, prims.begin() + end,
                                                    [axis, splitPos](const BVHPrimitive& p)
                                                    { return GetAxis(p.bounds.Center(), axis) < splitPos; });
                        bestSplit = static_cast<uint32_t>(midIt - prims.begin());

                        // Ensure non-degenerate split
                        if (bestSplit == start || bestSplit == end)
                        {
                            bestSplit = (start + end) / 2;
                        }
                    }
                }
            }

            // If no good split found, fall back to median split on longest axis
            if (bestAxis == -1)
            {
                float dx = totalBounds.max.x - totalBounds.min.x;
                float dy = totalBounds.max.y - totalBounds.min.y;
                float dz = totalBounds.max.z - totalBounds.min.z;
                bestAxis = (dx >= dy && dx >= dz) ? 0 : (dy >= dz ? 1 : 2);
                bestSplit = (start + end) / 2;
                std::nth_element(prims.begin() + start, prims.begin() + bestSplit, prims.begin() + end,
                                 [bestAxis](const BVHPrimitive& a, const BVHPrimitive& b) {
                                     return GetAxis(a.bounds.Center(), bestAxis) < GetAxis(b.bounds.Center(), bestAxis);
                                 });
            }

            m_nodes[nodeIndex].splitAxis = static_cast<uint8_t>(bestAxis);

            // Build children (first child is always at nodeIndex + 1)
            BuildRecursive(prims, start, bestSplit);
            m_nodes[nodeIndex].secondChild = BuildRecursive(prims, bestSplit, end);

            return nodeIndex;
        }

        void FrustumQueryRecursive(uint32_t nodeIndex, const Frustum& frustum, std::vector<uint32_t>& results) const
        {
            const auto& node = m_nodes[nodeIndex];

            // Test node bounds against frustum
            CullResult result = frustum.TestAABB(node.bounds);
            if (result == CullResult::Outside)
                return;

            // Leaf node — add all primitives
            if (node.primCount > 0)
            {
                for (uint32_t i = 0; i < node.primCount; ++i)
                {
                    results.push_back(m_leafPrimitives[node.primOffset + i]);
                }
                return;
            }

            // If fully inside, collect all leaf primitives without further tests
            if (result == CullResult::Inside)
            {
                CollectAllPrimitives(nodeIndex, results);
                return;
            }

            // Partially visible — recurse into children
            FrustumQueryRecursive(nodeIndex + 1, frustum, results);
            FrustumQueryRecursive(node.secondChild, frustum, results);
        }

        void AABBQueryRecursive(uint32_t nodeIndex, const AABB& queryBox, std::vector<uint32_t>& results) const
        {
            const auto& node = m_nodes[nodeIndex];

            if (!AABBIntersects(node.bounds, queryBox))
                return;

            if (node.primCount > 0)
            {
                for (uint32_t i = 0; i < node.primCount; ++i)
                {
                    results.push_back(m_leafPrimitives[node.primOffset + i]);
                }
                return;
            }

            AABBQueryRecursive(nodeIndex + 1, queryBox, results);
            AABBQueryRecursive(node.secondChild, queryBox, results);
        }

        void CollectAllPrimitives(uint32_t nodeIndex, std::vector<uint32_t>& results) const
        {
            const auto& node = m_nodes[nodeIndex];

            if (node.primCount > 0)
            {
                for (uint32_t i = 0; i < node.primCount; ++i)
                {
                    results.push_back(m_leafPrimitives[node.primOffset + i]);
                }
                return;
            }

            CollectAllPrimitives(nodeIndex + 1, results);
            CollectAllPrimitives(node.secondChild, results);
        }

        std::vector<BVHNode> m_nodes;
        std::vector<uint32_t> m_leafPrimitives;
        uint32_t m_maxLeafSize = 4;
    };

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
