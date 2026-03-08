// TestObjectPool.cpp - Tests for ObjectPool template
// Tests the pool acquire/release cycle and capacity management

#include "TestFramework.h"
#include <vector>
#include <memory>

// Simplified ObjectPool for testing (mirrors engine implementation)
template <typename T> class TestObjectPool
{
  public:
    explicit TestObjectPool(size_t capacity)
    {
        m_objects.resize(capacity);
        m_available.reserve(capacity);
        for (size_t i = 0; i < capacity; ++i)
        {
            m_objects[i] = std::make_unique<T>();
            m_available.push_back(m_objects[i].get());
        }
    }

    T* Acquire()
    {
        if (m_available.empty())
            return nullptr;
        T* obj = m_available.back();
        m_available.pop_back();
        return obj;
    }

    void Release(T* obj)
    {
        if (!obj)
            return;
        m_available.push_back(obj);
    }

    size_t GetCapacity() const { return m_objects.size(); }
    size_t GetAvailableCount() const { return m_available.size(); }
    size_t GetActiveCount() const { return m_objects.size() - m_available.size(); }

  private:
    std::vector<std::unique_ptr<T>> m_objects;
    std::vector<T*> m_available;
};

struct TestObj
{
    int value = 0;
};

TEST(ObjectPool_InitialState)
{
    TestObjectPool<TestObj> pool(10);
    EXPECT_EQ(pool.GetCapacity(), 10u);
    EXPECT_EQ(pool.GetAvailableCount(), 10u);
    EXPECT_EQ(pool.GetActiveCount(), 0u);
}

TEST(ObjectPool_AcquireSingle)
{
    TestObjectPool<TestObj> pool(10);
    auto* obj = pool.Acquire();
    EXPECT_TRUE(obj != nullptr);
    EXPECT_EQ(pool.GetAvailableCount(), 9u);
    EXPECT_EQ(pool.GetActiveCount(), 1u);
}

TEST(ObjectPool_AcquireAndRelease)
{
    TestObjectPool<TestObj> pool(5);
    auto* obj = pool.Acquire();
    EXPECT_TRUE(obj != nullptr);
    pool.Release(obj);
    EXPECT_EQ(pool.GetAvailableCount(), 5u);
    EXPECT_EQ(pool.GetActiveCount(), 0u);
}

TEST(ObjectPool_ExhaustPool)
{
    TestObjectPool<TestObj> pool(3);
    auto* a = pool.Acquire();
    auto* b = pool.Acquire();
    auto* c = pool.Acquire();
    EXPECT_TRUE(a != nullptr);
    EXPECT_TRUE(b != nullptr);
    EXPECT_TRUE(c != nullptr);

    // Pool should be empty now
    auto* d = pool.Acquire();
    EXPECT_TRUE(d == nullptr);
    EXPECT_EQ(pool.GetAvailableCount(), 0u);
}

TEST(ObjectPool_ReleaseNull)
{
    TestObjectPool<TestObj> pool(5);
    pool.Release(nullptr); // Should not crash
    EXPECT_EQ(pool.GetAvailableCount(), 5u);
}

TEST(ObjectPool_MultipleAcquireRelease)
{
    TestObjectPool<TestObj> pool(10);
    std::vector<TestObj*> acquired;

    // Acquire all
    for (int i = 0; i < 10; ++i)
    {
        auto* obj = pool.Acquire();
        EXPECT_TRUE(obj != nullptr);
        obj->value = i;
        acquired.push_back(obj);
    }
    EXPECT_EQ(pool.GetActiveCount(), 10u);

    // Release half
    for (int i = 0; i < 5; ++i)
        pool.Release(acquired[i]);
    EXPECT_EQ(pool.GetActiveCount(), 5u);
    EXPECT_EQ(pool.GetAvailableCount(), 5u);

    // Re-acquire
    for (int i = 0; i < 5; ++i)
    {
        auto* obj = pool.Acquire();
        EXPECT_TRUE(obj != nullptr);
    }
    EXPECT_EQ(pool.GetActiveCount(), 10u);
}
