// TestJobSystem.cpp - Tests for the thread pool job system

#include "TestFramework.h"
#include "Utils/JobSystem.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <system_error>
#include <thread>
#include <vector>

// Helper: stand-alone job system for test isolation (avoids the singleton's
// call_once, which prevents re-initialization within the same process)
namespace
{

    class TestJobPool
    {
      public:
        void Initialize(uint32_t numThreads)
        {
            m_stop.store(false);
            m_workers.reserve(numThreads);
            for (uint32_t i = 0; i < numThreads; ++i)
                m_workers.emplace_back(&TestJobPool::Worker, this);
        }

        void Shutdown()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop.store(true);
            }
            m_cv.notify_all();
            for (auto& w : m_workers)
            {
                if (w.joinable())
                    w.join();
            }
            m_workers.clear();
        }

        template <typename F> std::future<std::invoke_result_t<F>> Submit(F&& f)
        {
            using R = std::invoke_result_t<F>;
            auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
            auto future = task->get_future();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_queue.emplace([task]() { (*task)(); });
            }
            m_cv.notify_one();
            return future;
        }

        uint32_t GetWorkerCount() const { return static_cast<uint32_t>(m_workers.size()); }

      private:
        void Worker()
        {
            while (true)
            {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this] { return m_stop.load() || !m_queue.empty(); });
                    if (m_stop.load() && m_queue.empty())
                        return;
                    job = std::move(m_queue.front());
                    m_queue.pop();
                }
                job();
            }
        }

        std::vector<std::thread> m_workers;
        std::queue<std::function<void()>> m_queue;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::atomic<bool> m_stop{false};
    };

} // namespace

// =============================================================================
// Submit and Future
// =============================================================================

TEST(JobSystem_SubmitReturnsResult)
{
    TestJobPool pool;
    pool.Initialize(2);

    auto future = pool.Submit([]() { return 42; });
    int result = future.get();
    EXPECT_EQ(result, 42);

    pool.Shutdown();
}

TEST(JobSystem_SubmitVoidJob)
{
    TestJobPool pool;
    pool.Initialize(2);

    std::atomic<bool> executed{false};
    auto future = pool.Submit([&executed]() { executed.store(true); });
    future.get();
    EXPECT_TRUE(executed.load());

    pool.Shutdown();
}

TEST(JobSystem_SubmitMultipleJobs)
{
    TestJobPool pool;
    pool.Initialize(4);

    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;

    for (int i = 0; i < 100; ++i)
    {
        futures.push_back(pool.Submit([&counter]() { counter.fetch_add(1); }));
    }

    for (auto& f : futures)
        f.get();

    EXPECT_EQ(counter.load(), 100);

    pool.Shutdown();
}

// =============================================================================
// Parallel For (via singleton)
// =============================================================================

TEST(JobSystem_ParallelForEmptyRange)
{
    // ParallelFor on singleton - use it cautiously since it's a one-time init
    auto& js = Spark::JobSystem::Get();
    js.Initialize(2);

    std::atomic<int> count{0};
    js.ParallelFor(0, 0, [&count](int) { count.fetch_add(1); });
    EXPECT_EQ(count.load(), 0);
}

TEST(JobSystem_ParallelForProcessesAll)
{
    auto& js = Spark::JobSystem::Get();
    js.Initialize(2);

    const int N = 200;
    std::vector<std::atomic<int>> flags(N);
    for (int i = 0; i < N; ++i)
        flags[i].store(0);

    js.ParallelFor(0, N, [&flags](int i) { flags[i].store(1); });

    int set = 0;
    for (int i = 0; i < N; ++i)
    {
        if (flags[i].load())
            set++;
    }
    EXPECT_EQ(set, N);
}

TEST(JobSystem_ParallelForWithBatchSize)
{
    auto& js = Spark::JobSystem::Get();
    js.Initialize(2);

    std::atomic<int> sum{0};
    js.ParallelFor(0, 50, [&sum](int i) { sum.fetch_add(i); }, 10);

    // Sum of 0..49 = 1225
    EXPECT_EQ(sum.load(), 1225);
}

// =============================================================================
// Worker Count
// =============================================================================

TEST(JobSystem_WorkerCount)
{
    TestJobPool pool;
    pool.Initialize(3);
    EXPECT_EQ(pool.GetWorkerCount(), static_cast<uint32_t>(3));
    pool.Shutdown();
}

// =============================================================================
// Concurrent Stress
// =============================================================================

TEST(JobSystem_ConcurrentStress)
{
    TestJobPool pool;
    pool.Initialize(4);

    std::atomic<int> total{0};
    std::vector<std::future<int>> futures;

    for (int i = 0; i < 500; ++i)
    {
        futures.push_back(pool.Submit(
            [i, &total]()
            {
                total.fetch_add(1);
                return i * 2;
            }));
    }

    int resultSum = 0;
    for (auto& f : futures)
        resultSum += f.get();

    EXPECT_EQ(total.load(), 500);
    // Sum of (i*2) for i=0..499 = 2 * (499*500/2) = 249500
    EXPECT_EQ(resultSum, 249500);

    pool.Shutdown();
}

// =============================================================================
// Singleton Interface
// =============================================================================

TEST(JobSystem_SingletonInitialized)
{
    auto& js = Spark::JobSystem::Get();
    js.Initialize(2);
    EXPECT_TRUE(js.IsInitialized());
}

TEST(JobSystem_WaitForAll)
{
    auto& js = Spark::JobSystem::Get();
    js.Initialize(2);

    std::atomic<int> counter{0};
    for (int i = 0; i < 10; ++i)
    {
        js.Submit([&counter]() { counter.fetch_add(1); });
    }

    js.WaitForAll();
    EXPECT_EQ(counter.load(), 10);
}

TEST(JobSystem_GetPendingJobCount)
{
    auto& js = Spark::JobSystem::Get();
    js.Initialize(2);

    // After WaitForAll, pending should be 0
    js.WaitForAll();
    EXPECT_EQ(js.GetPendingJobCount(), static_cast<size_t>(0));
}

// =============================================================================
// Worker-count policy and partial-start rollback
// =============================================================================

TEST(JobSystemHardening_WorkerCountNeverUnderflowsAndIsCapped)
{
    using Spark::JobSystem;
    constexpr uint32_t cap = JobSystem::kMaxWorkerThreads;

    // hardware_concurrency() may legally return 0; hw - 1 must not wrap to UINT32_MAX.
    static_assert(JobSystem::ResolveWorkerCount(0, 0) == 1);
    EXPECT_EQ(JobSystem::ResolveWorkerCount(0, 0), 1u);
    EXPECT_EQ(JobSystem::ResolveWorkerCount(0, 1), 1u);
    EXPECT_EQ(JobSystem::ResolveWorkerCount(0, 8), 7u);
    EXPECT_EQ(JobSystem::ResolveWorkerCount(3, 0), 3u);

    // Operator-supplied counts are capped, not trusted.
    EXPECT_EQ(JobSystem::ResolveWorkerCount(UINT32_MAX, 16), cap);
    EXPECT_EQ(JobSystem::ResolveWorkerCount(cap + 1, 16), cap);
    EXPECT_EQ(JobSystem::ResolveWorkerCount(cap, 16), cap);
    EXPECT_EQ(JobSystem::ResolveWorkerCount(0, UINT32_MAX), cap);
}

TEST(JobSystemHardening_ParseWorkerCountRejectsGarbageAndSaturates)
{
    using Spark::JobSystem;
    EXPECT_EQ(JobSystem::ParseWorkerCount("4"), 4u);
    EXPECT_EQ(JobSystem::ParseWorkerCount(" 12 "), 12u);
    EXPECT_EQ(JobSystem::ParseWorkerCount(""), 0u);
    EXPECT_EQ(JobSystem::ParseWorkerCount("-1"), 0u);
    EXPECT_EQ(JobSystem::ParseWorkerCount("4abc"), 0u);
    EXPECT_EQ(JobSystem::ParseWorkerCount("abc"), 0u);
    // std::atoi has undefined behaviour here; the parser saturates and Initialize caps.
    EXPECT_EQ(JobSystem::ParseWorkerCount("99999999999999999999"), UINT32_MAX);
    EXPECT_EQ(JobSystem::ResolveWorkerCount(JobSystem::ParseWorkerCount("99999999999999999999"), 8),
              JobSystem::kMaxWorkerThreads);
}

TEST(JobSystemHardening_PartialStartFailureJoinsStartedThreads)
{
    std::vector<std::thread> threads;
    std::atomic<bool> stop{false};
    std::atomic<int> exited{0};
    int started = 0;
    bool stopRequested = false;
    bool threw = false;

    try
    {
        Spark::JobSystem::StartThreadsOrRollback(
            threads, 5,
            [&]
            {
                // The fourth start fails the way std::thread does when the OS refuses a thread.
                if (started == 3)
                    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
                ++started;
                return std::thread(
                    [&]
                    {
                        while (!stop.load())
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        exited.fetch_add(1);
                    });
            },
            [&]
            {
                stopRequested = true;
                stop.store(true);
            });
    }
    catch (const std::system_error&)
    {
        threw = true;
    }

    // The failure reaches the caller and no joinable thread is left behind (one would
    // call std::terminate when `threads` is destroyed): every started thread was joined.
    EXPECT_TRUE(threw);
    EXPECT_TRUE(stopRequested);
    EXPECT_TRUE(threads.empty());
    EXPECT_EQ(started, 3);
    EXPECT_EQ(exited.load(), 3);

    // Keep a failing assertion above from terminating the test binary.
    stop.store(true);
    for (auto& thread : threads)
    {
        if (thread.joinable())
            thread.join();
    }
}
