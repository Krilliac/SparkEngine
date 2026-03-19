/**
 * @file CancellationToken.h
 * @brief Cancellation token for coroutines tied to entity lifetime
 *
 * Inspired by S&box's Component.EnabledToken pattern. Coroutines started by
 * an entity carry a token that auto-cancels when the entity is destroyed,
 * preventing dangling access to destroyed entities.
 */

#pragma once

#include <atomic>
#include <memory>

namespace Spark
{

    /// @brief Cooperative cancellation token (thread-safe, shared ownership)
    class CancellationToken
    {
      public:
        CancellationToken() : m_cancelled(std::make_shared<std::atomic<bool>>(false)) {}

        /// @brief Check if cancellation was requested
        bool IsCancelled() const { return m_cancelled->load(std::memory_order_acquire); }

        /// @brief Request cancellation
        void Cancel() { m_cancelled->store(true, std::memory_order_release); }

        /// @brief Reset to non-cancelled state
        void Reset() { m_cancelled->store(false, std::memory_order_release); }

        /// @brief Create a linked token that cancels when either this or parent cancels
        CancellationToken CreateLinked() const
        {
            CancellationToken linked;
            linked.m_parent = m_cancelled;
            return linked;
        }

        /// @brief Check if cancelled (including parent chain)
        bool IsCancelledOrParent() const
        {
            if (m_cancelled->load(std::memory_order_acquire))
                return true;
            if (m_parent && m_parent->load(std::memory_order_acquire))
                return true;
            return false;
        }

      private:
        std::shared_ptr<std::atomic<bool>> m_cancelled;
        std::shared_ptr<std::atomic<bool>> m_parent; ///< Optional parent token
    };

    /// @brief ECS component that provides a cancellation token per entity
    struct EntityCancellationComponent
    {
        CancellationToken token;

        /// @brief Call when entity is about to be destroyed
        void CancelAll() { token.Cancel(); }
    };

} // namespace Spark
