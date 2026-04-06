/**
 * @file AtomicSharedPtr.h
 * @brief Lock-free atomic shared pointer wrapper
 *
 * Provides thread-safe shared ownership without mutex overhead, inspired by
 * RPCS3's atomic_ptr<T> with borrowed reference counting. Uses C++20
 * std::atomic<std::shared_ptr<T>> where available, with a mutex fallback.
 *
 * This wrapper allows swapping the implementation later if profiling shows
 * std::atomic<shared_ptr> is insufficient (e.g., custom fat-pointer approach).
 *
 * @see WorkSema.h for other threading primitives
 */

#pragma once

#include <atomic>
#include <memory>
#include <mutex>

namespace Spark
{

    /**
     * @brief Thread-safe atomic shared pointer.
     *
     * Supports lock-free load/store/exchange/compare_exchange on shared_ptr
     * without external synchronization. Ideal for resources shared between
     * IO, render, and streaming threads.
     *
     * @tparam T  Pointed-to type.
     */
    template <typename T> class AtomicSharedPtr
    {
      public:
        AtomicSharedPtr() = default;

        explicit AtomicSharedPtr(std::shared_ptr<T> ptr) { Store(std::move(ptr)); }

        ~AtomicSharedPtr() = default;

        AtomicSharedPtr(const AtomicSharedPtr&) = delete;
        AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

        /// @brief Atomically load the current pointer.
        std::shared_ptr<T> Load() const
        {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 202011L
            return m_atomic.load(std::memory_order_acquire);
#else
            std::lock_guard lock(m_mutex);
            return m_ptr;
#endif
        }

        /// @brief Atomically store a new pointer.
        void Store(std::shared_ptr<T> desired)
        {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 202011L
            m_atomic.store(std::move(desired), std::memory_order_release);
#else
            std::lock_guard lock(m_mutex);
            m_ptr = std::move(desired);
#endif
        }

        /// @brief Atomically exchange the pointer.
        std::shared_ptr<T> Exchange(std::shared_ptr<T> desired)
        {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 202011L
            return m_atomic.exchange(std::move(desired), std::memory_order_acq_rel);
#else
            std::lock_guard lock(m_mutex);
            auto old = std::move(m_ptr);
            m_ptr = std::move(desired);
            return old;
#endif
        }

        /// @brief Atomically compare and exchange.
        bool CompareExchange(std::shared_ptr<T>& expected, std::shared_ptr<T> desired)
        {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 202011L
            return m_atomic.compare_exchange_strong(expected, std::move(desired), std::memory_order_acq_rel,
                                                    std::memory_order_acquire);
#else
            std::lock_guard lock(m_mutex);
            if (m_ptr == expected)
            {
                m_ptr = std::move(desired);
                return true;
            }
            expected = m_ptr;
            return false;
#endif
        }

        /// @brief Convenience: check if the pointer is null.
        bool IsNull() const { return Load() == nullptr; }

        /// @brief Convenience: implicit load via operator->.
        std::shared_ptr<T> operator->() const { return Load(); }

      private:
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 202011L
        std::atomic<std::shared_ptr<T>> m_atomic;
#else
        mutable std::mutex m_mutex;
        std::shared_ptr<T> m_ptr;
#endif
    };

} // namespace Spark
