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

#include <algorithm>
#include <charconv>
#include <string_view>
#include <system_error>
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

        /// Upper bound on worker threads. More workers than this add contention, not
        /// throughput, and an unbounded request (`-threads 4000000000`,
        /// SPARK_MAX_WORKER_THREADS) would otherwise exhaust memory or thread handles.
        static constexpr uint32_t kMaxWorkerThreads = 256;

        /**
         * @brief Resolve a requested worker count against the host and the policy cap.
         * @param requested        Requested workers; 0 selects hardwareThreads - 1.
         * @param hardwareThreads  std::thread::hardware_concurrency(), which may legally be 0.
         * @return A count in [1, kMaxWorkerThreads]. Never underflows when the host reports 0 or 1.
         */
        [[nodiscard]] static constexpr uint32_t ResolveWorkerCount(uint32_t requested,
                                                                   uint32_t hardwareThreads) noexcept
        {
            uint32_t count = requested;
            if (count == 0)
            {
                count = hardwareThreads > 1 ? hardwareThreads - 1 : 1;
            }
            return count < kMaxWorkerThreads ? count : kMaxWorkerThreads;
        }

        /**
         * @brief Parse a worker count from a command-line or environment value.
         *
         * Accepts only an unsigned decimal number (surrounding spaces allowed). Anything
         * else, including a negative number, yields 0 ("use the default"). A number too
         * large for uint32_t saturates to UINT32_MAX, which ResolveWorkerCount then caps.
         * Unlike std::atoi this has no undefined behaviour on overflow.
         */
        [[nodiscard]] static uint32_t ParseWorkerCount(std::string_view text) noexcept
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            {
                text.remove_suffix(1);
            }

            uint32_t value = 0;
            const char* const end = text.data() + text.size();
            const auto [next, error] = std::from_chars(text.data(), end, value);
            if (text.empty() || next != end)
            {
                return 0;
            }
            if (error == std::errc::result_out_of_range)
            {
                return UINT32_MAX;
            }
            return error == std::errc{} ? value : 0;
        }

        /**
         * @brief Start @p count threads, or none at all.
         *
         * If reserving or starting any thread throws, @p requestStop is called so the
         * threads already started can exit, every one of them is joined, @p threads is
         * cleared and the exception is rethrown. A joinable std::thread is never left
         * behind, so the failure cannot turn into std::terminate at destruction.
         *
         * @param threads      Destination; must be empty.
         * @param count        Number of threads to start.
         * @param makeThread   Callable returning a started std::thread.
         * @param requestStop  Callable that makes every started thread return.
         */
        template <typename MakeThread, typename RequestStop>
        static void StartThreadsOrRollback(std::vector<std::thread>& threads, uint32_t count, MakeThread&& makeThread,
                                           RequestStop&& requestStop)
        {
            try
            {
                threads.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    // Capacity is reserved, so push_back cannot throw and drop a started thread.
                    threads.push_back(makeThread());
                }
            }
            catch (...)
            {
                requestStop();
                for (auto& thread : threads)
                {
                    if (thread.joinable())
                    {
                        thread.join();
                    }
                }
                threads.clear();
                throw;
            }
        }

        /**
         * @brief Initialize the thread pool with a given number of worker threads
         * @param numThreads Number of worker threads. 0 = hardware_concurrency - 1.
         *                   Values above kMaxWorkerThreads are clamped (and logged).
         * @throws std::system_error / std::bad_alloc if the workers cannot be started.
         *         No worker is left running in that case and Initialize may be retried.
         */
        void Initialize(uint32_t numThreads = 0)
        {
            std::call_once(
                m_initFlag,
                [this, numThreads]()
                {
                    const uint32_t workerCount = ResolveWorkerCount(numThreads, std::thread::hardware_concurrency());
                    if (numThreads > workerCount)
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Core, "JobSystem: requested %u worker threads, capped at %u",
                                       numThreads, workerCount);
                    }

                    m_stop.store(false, std::memory_order_relaxed);
                    StartThreadsOrRollback(
                        m_workers, workerCount, [this] { return std::thread(&JobSystem::WorkerThread, this); },
                        [this]
                        {
                            {
                                std::lock_guard<std::mutex> lock(m_queueMutex);
                                m_stop.store(true, std::memory_order_release);
                            }
                            m_condition.notify_all();
                        });

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
            // A FIFO barrier job is insufficient: it can complete on one worker while
            // earlier-dequeued jobs still run on others. Wait for the queue to drain
            // AND every in-flight job to finish.
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_idleCondition.wait(lock, [this] { return m_jobQueue.empty() && m_activeJobs == 0; });
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

        /**
         * @brief Block until a job is queued or the pool is stopping.
         * @return false once the pool is stopping and the queue has drained.
         *
         * The queue lock lives for this whole function. The wait predicate
         * guarantees a job is queued unless we are stopping, so an empty queue
         * after the wait means "stopping and drained".
         */
        bool WaitForJob(std::function<void()>& job)
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_condition.wait(lock, [this] { return m_stop.load(std::memory_order_acquire) || !m_jobQueue.empty(); });
            if (m_jobQueue.empty())
            {
                return false;
            }
            job = std::move(m_jobQueue.front());
            m_jobQueue.pop();
            ++m_activeJobs;
            return true;
        }

        void WorkerThread()
        {
            while (true)
            {
                std::function<void()> job;
                if (!WaitForJob(job))
                {
                    return;
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

                {
                    std::lock_guard<std::mutex> lock(m_queueMutex);
                    --m_activeJobs;
                    if (m_activeJobs == 0 && m_jobQueue.empty())
                    {
                        m_idleCondition.notify_all();
                    }
                }
            }
        }

        std::vector<std::thread> m_workers;
        std::queue<std::function<void()>> m_jobQueue;
        mutable std::mutex m_queueMutex;
        std::condition_variable m_condition;
        std::condition_variable m_idleCondition;
        size_t m_activeJobs{0}; // guarded by m_queueMutex
        std::atomic<bool> m_stop{false};
        std::atomic<bool> m_initialized{false};
        std::once_flag m_initFlag;
    };

} // namespace Spark
