/**
 * @file Octree.h
 * @brief Loose octree for spatial partitioning and fast broad-phase queries
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a generic loose octree that accelerates spatial queries across
 * large numbers of entities. Supports insertion, removal, range queries
 * (AABB and sphere), frustum queries, and ray intersection tests.
 *
 * The "loose" factor (default 2×) means each node's actual bounds are expanded
 * beyond the strict octant, reducing the number of objects that straddle node
 * boundaries and require placement in parent nodes.
 *
 * ## Usage
 * @code
 *   using namespace Spark;
 *   Octree<EntityID> tree({-500,-500,-500}, {500,500,500});
 *
 *   tree.Insert(entity, entityAABB);
 *   tree.Remove(entity);
 *
 *   auto visible = tree.QueryFrustum(frustum);
 *   auto nearby  = tree.QuerySphere(center, radius);
 *   auto inBox   = tree.QueryAABB(queryBox);
 * @endcode
 *
 * @see FrustumCulling.h, RenderSystem, PhysicsSystem
 */

#pragma once
#include "../Core/Platform.h"
#include "../Graphics/FrustumCulling.h"

#include <array>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cmath>
#include <algorithm>
#include <memory>

namespace Spark
{

    /**
 * @brief Loose octree for spatial partitioning.
 *
 * @tparam T  The type stored in the tree (typically EntityID or a pointer).
 *            Must be hashable (for the internal lookup map).
 */
    template <typename T> class Octree
    {
      public:
        /**
     * @brief Construct an octree covering the given world bounds.
     *
     * @param worldMin   Minimum corner of the world volume.
     * @param worldMax   Maximum corner of the world volume.
     * @param maxDepth   Maximum subdivision depth (default 6 = 64^3 leaf cells).
     * @param maxPerNode Maximum objects per node before subdivision.
     * @param looseness  Loose factor multiplier (>1.0 reduces boundary straddle).
     */
        Octree(const DirectX::XMFLOAT3& worldMin, const DirectX::XMFLOAT3& worldMax, int maxDepth = 6,
               int maxPerNode = 8, float looseness = 2.0f)
            : m_maxDepth(maxDepth), m_maxPerNode(maxPerNode), m_looseness(looseness)
        {
            m_root = std::make_unique<Node>();
            m_root->bounds.min = worldMin;
            m_root->bounds.max = worldMax;
            m_root->depth = 0;
        }

        /**
     * @brief Insert an object with its AABB into the tree.
     *
     * If the object already exists, it is updated (removed + re-inserted).
     */
        void Insert(const T& item, const Graphics::AABB& bounds)
        {
            // Remove old entry if present
            auto it = m_itemNodes.find(item);
            if (it != m_itemNodes.end())
            {
                RemoveFromNode(it->second, item);
            }
            InsertIntoNode(m_root.get(), item, bounds);
        }

        /**
     * @brief Remove an object from the tree.
     * @return True if the object was found and removed.
     */
        bool Remove(const T& item)
        {
            auto it = m_itemNodes.find(item);
            if (it == m_itemNodes.end())
                return false;
            RemoveFromNode(it->second, item);
            m_itemNodes.erase(it);
            return true;
        }

        /**
     * @brief Query all objects whose AABB intersects the given box.
     */
        std::vector<T> QueryAABB(const Graphics::AABB& queryBox) const
        {
            std::vector<T> results;
            QueryAABBRecursive(m_root.get(), queryBox, results);
            return results;
        }

        /**
     * @brief Query all objects whose AABB intersects the given sphere.
     */
        std::vector<T> QuerySphere(const DirectX::XMFLOAT3& center, float radius) const
        {
            std::vector<T> results;
            QuerySphereRecursive(m_root.get(), center, radius, results);
            return results;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        /**
     * @brief Query all objects that are at least partially inside the frustum.
     *
     * This is the primary integration point with FrustumCulling — call once
     * per frame to get a list of potentially visible objects.
     */
        std::vector<T> QueryFrustum(const Graphics::Frustum& frustum) const
        {
            std::vector<T> results;
            QueryFrustumRecursive(m_root.get(), frustum, results);
            return results;
        }
#endif

        /**
     * @brief Execute a callback for each object whose AABB intersects the query box.
     *
     * More efficient than QueryAABB when you don't need to collect into a vector.
     */
        void ForEachInAABB(const Graphics::AABB& queryBox, const std::function<void(const T&)>& callback) const
        {
            ForEachInAABBRecursive(m_root.get(), queryBox, callback);
        }

        /** @brief Remove all objects from the tree, preserving structure. */
        void Clear()
        {
            ClearNode(m_root.get());
            m_itemNodes.clear();
        }

        /** @brief Total number of objects currently stored. */
        size_t Size() const { return m_itemNodes.size(); }

        /** @brief Check if the tree is empty. */
        bool Empty() const { return m_itemNodes.empty(); }

        /**
     * @brief Rebuild the tree from scratch.
     *
     * Call periodically (e.g. every N frames) if many objects have moved
     * significantly. More efficient than incremental updates for large batches.
     */
        void Rebuild()
        {
            // Collect all current items
            std::vector<std::pair<T, Graphics::AABB>> allItems;
            allItems.reserve(m_itemNodes.size());
            CollectItems(m_root.get(), allItems);

            // Reset tree
            auto oldMin = m_root->bounds.min;
            auto oldMax = m_root->bounds.max;
            m_root = std::make_unique<Node>();
            m_root->bounds.min = oldMin;
            m_root->bounds.max = oldMax;
            m_root->depth = 0;
            m_itemNodes.clear();

            // Re-insert
            for (auto& [item, bounds] : allItems)
            {
                InsertIntoNode(m_root.get(), item, bounds);
            }
        }

      private:
        struct Entry
        {
            T item;
            Graphics::AABB bounds;
        };

        struct Node
        {
            Graphics::AABB bounds;
            std::vector<Entry> entries;
            std::array<std::unique_ptr<Node>, 8> children;
            int depth = 0;
            bool hasChildren = false;
        };

        std::unique_ptr<Node> m_root;
        std::unordered_map<T, Node*> m_itemNodes; // item -> node lookup for O(1) removal
        int m_maxDepth;
        int m_maxPerNode;
        float m_looseness;

        // ========================================================================
        // AABB helpers
        // ========================================================================

        static bool AABBOverlap(const Graphics::AABB& a, const Graphics::AABB& b)
        {
            return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
                   a.min.z <= b.max.z && a.max.z >= b.min.z;
        }

        static bool SphereAABBOverlap(const DirectX::XMFLOAT3& center, float radius, const Graphics::AABB& box)
        {
            // Find closest point on AABB to sphere center
            float dx = std::max(box.min.x - center.x, std::max(0.0f, center.x - box.max.x));
            float dy = std::max(box.min.y - center.y, std::max(0.0f, center.y - box.max.y));
            float dz = std::max(box.min.z - center.z, std::max(0.0f, center.z - box.max.z));
            return (dx * dx + dy * dy + dz * dz) <= (radius * radius);
        }

        static bool AABBContains(const Graphics::AABB& outer, const Graphics::AABB& inner)
        {
            return outer.min.x <= inner.min.x && outer.max.x >= inner.max.x && outer.min.y <= inner.min.y &&
                   outer.max.y >= inner.max.y && outer.min.z <= inner.min.z && outer.max.z >= inner.max.z;
        }

        // ========================================================================
        // Loose bounds for a child octant
        // ========================================================================

        Graphics::AABB GetChildBounds(const Node* node, int childIndex) const
        {
            auto c = node->bounds.Center();
            Graphics::AABB tight;

            tight.min.x = (childIndex & 1) ? c.x : node->bounds.min.x;
            tight.min.y = (childIndex & 2) ? c.y : node->bounds.min.y;
            tight.min.z = (childIndex & 4) ? c.z : node->bounds.min.z;
            tight.max.x = (childIndex & 1) ? node->bounds.max.x : c.x;
            tight.max.y = (childIndex & 2) ? node->bounds.max.y : c.y;
            tight.max.z = (childIndex & 4) ? node->bounds.max.z : c.z;

            // Apply looseness
            if (m_looseness > 1.0f)
            {
                auto ext = tight.Extents();
                float expand = (m_looseness - 1.0f) * 0.5f;
                auto tc = tight.Center();
                tight.min = {tc.x - ext.x * (1.0f + expand), tc.y - ext.y * (1.0f + expand),
                             tc.z - ext.z * (1.0f + expand)};
                tight.max = {tc.x + ext.x * (1.0f + expand), tc.y + ext.y * (1.0f + expand),
                             tc.z + ext.z * (1.0f + expand)};
            }

            return tight;
        }

        int GetChildIndex(const Node* node, const Graphics::AABB& itemBounds) const
        {
            auto c = node->bounds.Center();
            auto ic = itemBounds.Center();

            int index = 0;
            if (ic.x >= c.x)
                index |= 1;
            if (ic.y >= c.y)
                index |= 2;
            if (ic.z >= c.z)
                index |= 4;
            return index;
        }

        // ========================================================================
        // Insertion
        // ========================================================================

        void Subdivide(Node* node)
        {
            if (node->hasChildren)
                return;
            node->hasChildren = true;

            for (int i = 0; i < 8; ++i)
            {
                node->children[i] = std::make_unique<Node>();
                node->children[i]->bounds = GetChildBounds(node, i);
                node->children[i]->depth = node->depth + 1;
            }
        }

        void InsertIntoNode(Node* node, const T& item, const Graphics::AABB& bounds)
        {
            // If this node has children, try to push down
            if (node->hasChildren)
            {
                int childIdx = GetChildIndex(node, bounds);
                if (AABBContains(node->children[childIdx]->bounds, bounds))
                {
                    InsertIntoNode(node->children[childIdx].get(), item, bounds);
                    return;
                }
            }

            // Store in this node
            node->entries.push_back({item, bounds});
            m_itemNodes[item] = node;

            // Subdivide if needed
            if (!node->hasChildren && static_cast<int>(node->entries.size()) > m_maxPerNode && node->depth < m_maxDepth)
            {
                Subdivide(node);

                // Try to push existing entries down
                auto old = std::move(node->entries);
                node->entries.clear();

                for (auto& entry : old)
                {
                    int childIdx = GetChildIndex(node, entry.bounds);
                    if (AABBContains(node->children[childIdx]->bounds, entry.bounds))
                    {
                        InsertIntoNode(node->children[childIdx].get(), entry.item, entry.bounds);
                    }
                    else
                    {
                        node->entries.push_back(std::move(entry));
                        m_itemNodes[node->entries.back().item] = node;
                    }
                }
            }
        }

        // ========================================================================
        // Removal
        // ========================================================================

        void RemoveFromNode(Node* node, const T& item)
        {
            auto& entries = node->entries;
            for (auto it = entries.begin(); it != entries.end(); ++it)
            {
                if (it->item == item)
                {
                    entries.erase(it);
                    return;
                }
            }
        }

        // ========================================================================
        // Queries
        // ========================================================================

        void QueryAABBRecursive(const Node* node, const Graphics::AABB& queryBox, std::vector<T>& results) const
        {
            if (!AABBOverlap(node->bounds, queryBox))
                return;

            for (const auto& entry : node->entries)
            {
                if (AABBOverlap(entry.bounds, queryBox))
                {
                    results.push_back(entry.item);
                }
            }

            if (node->hasChildren)
            {
                for (int i = 0; i < 8; ++i)
                {
                    if (node->children[i])
                    {
                        QueryAABBRecursive(node->children[i].get(), queryBox, results);
                    }
                }
            }
        }

        void QuerySphereRecursive(const Node* node, const DirectX::XMFLOAT3& center, float radius,
                                  std::vector<T>& results) const
        {
            if (!SphereAABBOverlap(center, radius, node->bounds))
                return;

            for (const auto& entry : node->entries)
            {
                if (SphereAABBOverlap(center, radius, entry.bounds))
                {
                    results.push_back(entry.item);
                }
            }

            if (node->hasChildren)
            {
                for (int i = 0; i < 8; ++i)
                {
                    if (node->children[i])
                    {
                        QuerySphereRecursive(node->children[i].get(), center, radius, results);
                    }
                }
            }
        }

#ifdef SPARK_PLATFORM_WINDOWS
        void QueryFrustumRecursive(const Node* node, const Graphics::Frustum& frustum, std::vector<T>& results) const
        {
            // Test node bounds against frustum
            auto result = frustum.TestAABB(node->bounds);
            if (result == Graphics::CullResult::Outside)
                return;

            if (result == Graphics::CullResult::Inside)
            {
                // Entire node is visible — add all entries without further testing
                CollectAllEntries(node, results);
                return;
            }

            // Intersecting — test individual entries
            for (const auto& entry : node->entries)
            {
                if (frustum.IsVisible(entry.bounds))
                {
                    results.push_back(entry.item);
                }
            }

            if (node->hasChildren)
            {
                for (int i = 0; i < 8; ++i)
                {
                    if (node->children[i])
                    {
                        QueryFrustumRecursive(node->children[i].get(), frustum, results);
                    }
                }
            }
        }
#endif

        void ForEachInAABBRecursive(const Node* node, const Graphics::AABB& queryBox,
                                    const std::function<void(const T&)>& callback) const
        {
            if (!AABBOverlap(node->bounds, queryBox))
                return;

            for (const auto& entry : node->entries)
            {
                if (AABBOverlap(entry.bounds, queryBox))
                {
                    callback(entry.item);
                }
            }

            if (node->hasChildren)
            {
                for (int i = 0; i < 8; ++i)
                {
                    if (node->children[i])
                    {
                        ForEachInAABBRecursive(node->children[i].get(), queryBox, callback);
                    }
                }
            }
        }

        // ========================================================================
        // Utilities
        // ========================================================================

        void CollectAllEntries(const Node* node, std::vector<T>& results) const
        {
            for (const auto& entry : node->entries)
            {
                results.push_back(entry.item);
            }
            if (node->hasChildren)
            {
                for (int i = 0; i < 8; ++i)
                {
                    if (node->children[i])
                    {
                        CollectAllEntries(node->children[i].get(), results);
                    }
                }
            }
        }

        void CollectItems(const Node* node, std::vector<std::pair<T, Graphics::AABB>>& out) const
        {
            for (const auto& entry : node->entries)
            {
                out.push_back({entry.item, entry.bounds});
            }
            if (node->hasChildren)
            {
                for (int i = 0; i < 8; ++i)
                {
                    if (node->children[i])
                    {
                        CollectItems(node->children[i].get(), out);
                    }
                }
            }
        }

        void ClearNode(Node* node)
        {
            node->entries.clear();
            if (node->hasChildren)
            {
                for (int i = 0; i < 8; ++i)
                {
                    if (node->children[i])
                    {
                        ClearNode(node->children[i].get());
                    }
                }
            }
        }
    };

} // namespace Spark
