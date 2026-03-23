/**
 * @file DeferredDeletion.h
 * @brief Collect-now, process-later deferred queue for end-of-frame deletion
 * @author Spark Engine Team
 * @date 2026
 *
 * Many engine systems (entities, particles, network connections) need to mark
 * objects for deletion during iteration and process them later at a safe point.
 * This template replaces scattered `std::vector<T> m_pendingDeletion` patterns.
 *
 * ## Usage
 * @code
 *   Spark::DeferredQueue<entt::entity> deathQueue;
 *
 *   // During update — mark for deletion
 *   deathQueue.MarkForDeletion(entity);
 *
 *   // At end of frame — process all pending
 *   deathQueue.Flush([&](entt::entity& e) { registry.destroy(e); });
 * @endcode
 */

#pragma once

#include <functional>
#include <vector>

namespace Spark
{

    /**
     * @class DeferredQueue
     * @brief A queue that collects items and processes them in a deferred batch.
     * @tparam T The type of items to defer. Must be move-constructible.
     *
     * Not thread-safe. Callers must synchronize externally if used across threads.
     */
    template <typename T> class DeferredQueue
    {
      public:
        /**
         * @brief Add an item to the deferred queue.
         */
        void MarkForDeletion(T item) { m_pending.push_back(std::move(item)); }

        /**
         * @brief Process all pending items through a callback, then clear the queue.
         *
         * Safe to call MarkForDeletion from within the callback — new items will be
         * processed on the next Flush, not during the current one.
         */
        void Flush(std::function<void(T&)> callback)
        {
            // Move to local so callback can safely enqueue new items
            std::vector<T> local = std::move(m_pending);
            m_pending.clear();
            for (auto& item : local)
                callback(item);
        }

        /**
         * @brief Discard all pending items without processing.
         */
        void FlushAll() { m_pending.clear(); }

        /**
         * @brief Number of items pending deletion.
         */
        [[nodiscard]] size_t GetPendingCount() const { return m_pending.size(); }

        /**
         * @brief True if no items are pending.
         */
        [[nodiscard]] bool IsEmpty() const { return m_pending.empty(); }

      private:
        std::vector<T> m_pending;
    };

} // namespace Spark
