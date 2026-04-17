/**
 * @file JobSystem.h
 * @brief Simple thread pool for parallel work dispatch
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a lightweight job system built on a fixed-size thread pool.
 * Supports submitting callable tasks and waiting on futures.
 *
 * ## Usage
 * @code
 *   auto& jobs = Spark::JobSystem::Get();
 *   jobs.Initialize(4); // 4 worker threads
 *
 *   // Fire-and-forget
 *   jobs.Submit([]{ DoExpensiveWork(); });
 *
 *   // With future
 *   auto future = jobs.Submit([]{ return ComputeResult(); });
 *   auto result = future.get();
 *
 *   // Parallel for
 *   jobs.ParallelFor(0, 1000, [&](int i){ ProcessItem(i); });
 *
 *   jobs.Shutdown();
 * @endcode
 */

#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <new>
#include <exception>

#include "LogMacros.h"

namespace Spark
{

    /**
     * @brief Simple thread pool job system
     *
     * Thread-safe. Workers pull jobs from a shared queue and execute them.
     * Supports std::future-based result retrieval.
     */
    class JobSystem
    {
      public:
        static JobSystem& Get()
        {
            static JobSystem instance;
            return instance;
        }

        /**
         * @brief Initialize the thread pool with a given number of worker threads
         * @param numThreads Number of worker threads. 0 = hardware_concurrency - 1
         */
        void Initialize(uint32_t numThreads = 0)
        {
            std::call_once(m_initFlag,
                           [&, numThreads]() mutable
                           {
                               if (numThreads == 0)
                               {
                                   numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);
                               }

                               m_stop.store(false, std::memory_order_relaxed);

                               m_workers.reserve(numThreads);
                               for (uint32_t i = 0; i < numThreads; ++i)
                               {
                                   m_workers.emplace_back(&JobSystem::WorkerThread, this);
                               }

                               m_initialized.store(true, std::memory_order_release);
                           });
        }

        /**
         * @brief Shutdown all worker threads. Blocks until all pending jobs complete.
         */
        void Shutdown()
        {
            if (!m_initialized.exchange(false, std::memory_order_acq_rel))
            {
                return;
            }

            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_stop.store(true, std::memory_order_release);
            }
            m_condition.notify_all();

            for (auto& worker : m_workers)
            {
                if (worker.joinable())
                {
                    worker.join();
                }
            }
            m_workers.clear();
        }

        /**
         * @brief Submit a callable job and return a future for its result
         * @tparam F Callable type
         * @tparam Args Argument types
         * @return std::future holding the result of the callable
         */
        template <typename F, typename... Args>
        auto Submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
        {
            using ReturnType = std::invoke_result_t<F, Args...>;

            auto task = std::make_shared<std::packaged_task<ReturnType()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...));

            std::future<ReturnType> future = task->get_future();

            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_jobQueue.emplace([task]() { (*task)(); });
            }
            m_condition.notify_one();

            return future;
        }

        /**
         * @brief Execute a parallel for loop over [begin, end)
         *
         * Splits the range into chunks and submits each chunk as a job.
         * Blocks until all chunks complete.
         *
         * @param begin Start index (inclusive)
         * @param end End index (exclusive)
         * @param body Callable taking (int index)
         * @param minBatchSize Minimum items per batch
         */
        template <typename Func> void ParallelFor(int begin, int end, Func&& body, int minBatchSize = 1)
        {
            int totalItems = end - begin;
            if (totalItems <= 0)
            {
                return;
            }

            int numWorkers = static_cast<int>(m_workers.size());
            if (numWorkers == 0 || totalItems <= minBatchSize)
            {
                // Run inline if no workers or small range
                for (int i = begin; i < end; ++i)
                {
                    body(i);
                }
                return;
            }

            int batchSize = std::max(minBatchSize, totalItems / (numWorkers + 1));
            int batchCount = (totalItems + batchSize - 1) / batchSize;
            std::vector<std::future<void>> futures;
            futures.reserve(batchCount);

            for (int batchStart = begin; batchStart < end; batchStart += batchSize)
            {
                int batchEnd = std::min(batchStart + batchSize, end);
                futures.push_back(Submit(
                    [batchStart, batchEnd, body]()
                    {
                        for (int i = batchStart; i < batchEnd; ++i)
                        {
                            body(i);
                        }
                    }));
            }

            // Wait for all batches
            for (auto& f : futures)
            {
                f.get();
            }
        }

        /**
         * @brief Wait until all queued jobs are complete
         */
        void WaitForAll()
        {
            // Submit a barrier job and wait for it
            auto future = Submit([]() {});
            future.get();
        }

        /** @brief Get number of worker threads */
        uint32_t GetWorkerCount() const { return static_cast<uint32_t>(m_workers.size()); }

        /** @brief Check if the job system is initialized */
        bool IsInitialized() const { return m_initialized.load(std::memory_order_acquire); }

        /** @brief Get the number of pending jobs in the queue */
        size_t GetPendingJobCount() const
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            return m_jobQueue.size();
        }

      private:
        JobSystem() = default;
        ~JobSystem() { Shutdown(); }
        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;

        void WorkerThread()
        {
            while (true)
            {
                std::function<void()> job;

                {
                    std::unique_lock<std::mutex> lock(m_queueMutex);
                    m_condition.wait(lock,
                                     [this] { return m_stop.load(std::memory_order_acquire) || !m_jobQueue.empty(); });

                    if (m_stop.load(std::memory_order_acquire) && m_jobQueue.empty())
                    {
                        return;
                    }

                    job = std::move(m_jobQueue.front());
                    m_jobQueue.pop();
                }

                // Tasks that throw must not kill the worker; packaged_task captures
                // exceptions into the future, but fire-and-forget jobs need a catch.
                try
                {
                    job();
                }
                catch (const std::exception& e)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core, "JobSystem worker caught exception: %s", e.what());
                }
                catch (...)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core, "JobSystem worker caught unknown exception");
                }
            }
        }

        std::vector<std::thread> m_workers;
        std::queue<std::function<void()>> m_jobQueue;
        mutable std::mutex m_queueMutex;
        std::condition_variable m_condition;
        std::atomic<bool> m_stop{false};
        std::atomic<bool> m_initialized{false};
        std::once_flag m_initFlag;
    };

} // namespace Spark
