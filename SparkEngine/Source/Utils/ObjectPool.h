/**
 * @file ObjectPool.h
 * @brief Fixed-capacity object pool with optional thread safety
 * @author Spark Engine Team
 * @date 2026
 *
 * Pre-allocates a pool of objects and recycles them via acquire/release without
 * per-frame heap allocations. Suitable for projectiles, particles, debris, and
 * other high-frequency temporary objects.
 *
 * ## Usage
 * @code
 *   Spark::ObjectPool<Bullet> pool(256);
 *
 *   Bullet* b = pool.Acquire();
 *   if (b) { b->position = spawnPos; Fire(b); }
 *
 *   // When bullet is done
 *   pool.Release(b);
 *
 *   // Thread-safe variant
 *   Spark::ObjectPool<Bullet, true> safePool(128);
 * @endcode
 */

#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace Spark
{

    /**
     * @class ObjectPool
     * @brief Fixed-capacity pool that recycles pre-allocated objects.
     * @tparam T          Object type. Must be default-constructible.
     * @tparam ThreadSafe If true, Acquire/Release are guarded by a mutex.
     *
     * Objects are recycled as-is — the caller is responsible for reinitializing
     * state after Acquire. No constructors/destructors are called on recycle.
     */
    template <typename T, bool ThreadSafe = false> class ObjectPool
    {
      public:
        /**
         * @brief Construct a pool with the given capacity.
         * @param capacity Number of objects to pre-allocate.
         */
        explicit ObjectPool(size_t capacity)
        {
            m_storage.reserve(capacity);
            m_freeList.reserve(capacity);
            for (size_t i = 0; i < capacity; ++i)
            {
                m_storage.push_back(std::make_unique<T>());
                m_freeList.push_back(m_storage.back().get());
            }
        }

        /**
         * @brief Acquire an object from the pool.
         * @return Pointer to an available object, or nullptr if exhausted.
         */
        [[nodiscard]] T* Acquire()
        {
            if constexpr (ThreadSafe)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                return AcquireImpl();
            }
            else
            {
                return AcquireImpl();
            }
        }

        /**
         * @brief Return an object to the pool for reuse.
         * @param obj Pointer previously obtained from Acquire(). nullptr is a no-op.
         */
        void Release(T* obj)
        {
            if (!obj)
                return;
            if constexpr (ThreadSafe)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_freeList.push_back(obj);
            }
            else
            {
                m_freeList.push_back(obj);
            }
        }

        /**
         * @brief Total capacity of the pool.
         */
        [[nodiscard]] size_t GetCapacity() const { return m_storage.size(); }

        /**
         * @brief Number of objects currently available for acquisition.
         */
        [[nodiscard]] size_t GetAvailableCount() const { return m_freeList.size(); }

        /**
         * @brief Number of objects currently in use (acquired, not released).
         */
        [[nodiscard]] size_t GetActiveCount() const { return m_storage.size() - m_freeList.size(); }

      private:
        T* AcquireImpl()
        {
            if (m_freeList.empty())
                return nullptr;
            T* obj = m_freeList.back();
            m_freeList.pop_back();
            return obj;
        }

        std::vector<std::unique_ptr<T>> m_storage;
        std::vector<T*> m_freeList;

        struct EmptyMutex
        {
        };

        [[no_unique_address]] std::conditional_t<ThreadSafe, std::mutex, EmptyMutex> m_mutex;
    };

} // namespace Spark
