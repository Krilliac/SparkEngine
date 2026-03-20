/**
 * @file WorkSema.h
 * @brief Spin-before-sleep work queue semaphore
 *
 * Designed for high-frequency wake/sleep
 * patterns in job systems. Spins briefly before transitioning to kernel
 * sleep, reducing synchronization overhead for short-lived tasks.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace Spark
{

    /**
     * @brief Optimized semaphore for work-queue patterns.
     *
     * Three-phase wait: spin → yield → kernel sleep.
     * The spin phase catches quick producer-consumer handoffs without
     * kernel transitions. The yield phase is a compromise before the
     * expensive kernel sleep.
     */
    class WorkSema
    {
      public:
        /**
         * @param spinIterations Number of spin iterations before yielding.
         * @param yieldIterations Number of yield iterations before kernel sleep.
         */
        explicit WorkSema(uint32_t spinIterations = 1000, uint32_t yieldIterations = 10)
            : m_spinIterations(spinIterations), m_yieldIterations(yieldIterations)
        {
        }

        /**
         * @brief Signal that work is available (producer side).
         */
        void Post(uint32_t count = 1)
        {
            m_count.fetch_add(static_cast<int32_t>(count), std::memory_order_release);
            if (m_waiters.load(std::memory_order_acquire) > 0)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (count == 1)
                    m_cv.notify_one();
                else
                    m_cv.notify_all();
            }
        }

        /**
         * @brief Wait for work to become available (consumer side).
         *
         * Spin-waits first, then yields, then blocks on condition variable.
         */
        void Wait()
        {
            // Phase 1: Spin
            for (uint32_t i = 0; i < m_spinIterations; ++i)
            {
                if (TryAcquire())
                    return;
#if defined(_MSC_VER)
                _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#else
                std::this_thread::yield();
#endif
            }

            // Phase 2: Yield
            for (uint32_t i = 0; i < m_yieldIterations; ++i)
            {
                if (TryAcquire())
                    return;
                std::this_thread::yield();
            }

            // Phase 3: Kernel sleep
            m_waiters.fetch_add(1, std::memory_order_relaxed);
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_count.load(std::memory_order_acquire) > 0; });
            m_count.fetch_sub(1, std::memory_order_relaxed);
            m_waiters.fetch_sub(1, std::memory_order_relaxed);
        }

        /**
         * @brief Try to acquire without blocking.
         */
        bool TryAcquire()
        {
            int32_t expected = m_count.load(std::memory_order_relaxed);
            while (expected > 0)
            {
                if (m_count.compare_exchange_weak(expected, expected - 1, std::memory_order_acquire,
                                                  std::memory_order_relaxed))
                    return true;
            }
            return false;
        }

        int32_t GetCount() const { return m_count.load(std::memory_order_acquire); }
        int32_t GetWaiters() const { return m_waiters.load(std::memory_order_acquire); }

      private:
        std::atomic<int32_t> m_count{0};
        std::atomic<int32_t> m_waiters{0};
        std::mutex m_mutex;
        std::condition_variable m_cv;
        uint32_t m_spinIterations;
        uint32_t m_yieldIterations;
    };

} // namespace Spark
