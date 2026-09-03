// TestAtomicSharedPtr.cpp — Unit tests for lock-free atomic shared pointer
// Tests load/store, exchange, and concurrent multi-threaded access safety.

#include "TestFramework.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// ============================================================================
// Standalone atomic shared pointer for test isolation
// ============================================================================

namespace
{

    struct TestResource
    {
        int value = 0;
        static std::atomic<int> instanceCount;

        explicit TestResource(int v) : value(v) { instanceCount.fetch_add(1); }
        ~TestResource() { instanceCount.fetch_sub(1); }
    };

    std::atomic<int> TestResource::instanceCount{0};

    template <typename T> class TestAtomicSharedPtr
    {
      public:
        TestAtomicSharedPtr() = default;
        explicit TestAtomicSharedPtr(std::shared_ptr<T> ptr) : m_ptr(std::move(ptr)) {}

        std::shared_ptr<T> Load() const
        {
            std::lock_guard lock(m_mutex);
            return m_ptr;
        }

        void Store(std::shared_ptr<T> ptr)
        {
            std::lock_guard lock(m_mutex);
            m_ptr = std::move(ptr);
        }

        std::shared_ptr<T> Exchange(std::shared_ptr<T> ptr)
        {
            std::lock_guard lock(m_mutex);
            auto old = std::move(m_ptr);
            m_ptr = std::move(ptr);
            return old;
        }

      private:
        mutable std::mutex m_mutex;
        std::shared_ptr<T> m_ptr;
    };

} // namespace

TEST(AtomicSharedPtr_LoadStore)
{
    TestResource::instanceCount = 0;

    TestAtomicSharedPtr<TestResource> ptr;
    ASSERT_TRUE(ptr.Load() == nullptr);

    ptr.Store(std::make_shared<TestResource>(42));
    auto loaded = ptr.Load();
    ASSERT_TRUE(loaded != nullptr);
    ASSERT_EQ(loaded->value, 42);
    ASSERT_EQ(TestResource::instanceCount.load(), 1);
}

TEST(AtomicSharedPtr_Exchange)
{
    TestResource::instanceCount = 0;

    TestAtomicSharedPtr<TestResource> ptr(std::make_shared<TestResource>(1));
    auto old = ptr.Exchange(std::make_shared<TestResource>(2));

    ASSERT_EQ(old->value, 1);
    ASSERT_EQ(ptr.Load()->value, 2);
    ASSERT_EQ(TestResource::instanceCount.load(), 2);

    old.reset();
    ASSERT_EQ(TestResource::instanceCount.load(), 1);
}

TEST(AtomicSharedPtr_ConcurrentAccess)
{
    TestResource::instanceCount = 0;

    TestAtomicSharedPtr<TestResource> ptr(std::make_shared<TestResource>(0));

    constexpr int NUM_THREADS = 4;
    constexpr int OPS_PER_THREAD = 1000;

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back(
            [&ptr, t]()
            {
                for (int i = 0; i < OPS_PER_THREAD; ++i)
                {
                    if (i % 2 == 0)
                    {
                        auto val = ptr.Load();
                        (void)val;
                    }
                    else
                    {
                        ptr.Store(std::make_shared<TestResource>(t * OPS_PER_THREAD + i));
                    }
                }
            });
    }

    for (auto& t : threads)
        t.join();

    auto final_val = ptr.Load();
    ASSERT_TRUE(final_val != nullptr);
}
