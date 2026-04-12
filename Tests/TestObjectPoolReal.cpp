/**
 * @file TestObjectPoolReal.cpp
 * @brief Real-class tests for Spark::ObjectPool<T, ThreadSafe>
 */

#include "TestFramework.h"
#include "Utils/ObjectPool.h"

namespace
{
    struct TestObject
    {
        int value = 0;
    };
} // namespace

TEST(ObjectPoolReal_ConstructWithCapacity)
{
    Spark::ObjectPool<TestObject> pool(8);
    EXPECT_EQ(pool.GetCapacity(), static_cast<size_t>(8));
    EXPECT_EQ(pool.GetAvailableCount(), static_cast<size_t>(8));
}

TEST(ObjectPoolReal_AcquireReducesAvailable)
{
    Spark::ObjectPool<TestObject> pool(4);
    auto* a = pool.Acquire();
    EXPECT_TRUE(a != nullptr);
    EXPECT_EQ(pool.GetAvailableCount(), static_cast<size_t>(3));

    auto* b = pool.Acquire();
    auto* c = pool.Acquire();
    EXPECT_TRUE(b != nullptr);
    EXPECT_TRUE(c != nullptr);
    EXPECT_EQ(pool.GetAvailableCount(), static_cast<size_t>(1));
}

TEST(ObjectPoolReal_ExhaustReturnsNullptr)
{
    Spark::ObjectPool<TestObject> pool(2);
    auto* a = pool.Acquire();
    auto* b = pool.Acquire();
    auto* c = pool.Acquire();
    EXPECT_TRUE(a != nullptr);
    EXPECT_TRUE(b != nullptr);
    EXPECT_TRUE(c == nullptr); // Pool exhausted.
}

TEST(ObjectPoolReal_ReleaseIncreasesAvailable)
{
    Spark::ObjectPool<TestObject> pool(3);
    auto* a = pool.Acquire();
    auto* b = pool.Acquire();
    EXPECT_EQ(pool.GetAvailableCount(), static_cast<size_t>(1));

    pool.Release(a);
    EXPECT_EQ(pool.GetAvailableCount(), static_cast<size_t>(2));

    pool.Release(b);
    EXPECT_EQ(pool.GetAvailableCount(), static_cast<size_t>(3));
}

TEST(ObjectPoolReal_ReleaseNullIsNoOp)
{
    Spark::ObjectPool<TestObject> pool(2);
    pool.Release(nullptr);
    EXPECT_EQ(pool.GetAvailableCount(), static_cast<size_t>(2));
}

TEST(ObjectPoolReal_AcquiredObjectsAreUsable)
{
    Spark::ObjectPool<TestObject> pool(2);
    auto* a = pool.Acquire();
    EXPECT_TRUE(a != nullptr);
    a->value = 42;
    EXPECT_EQ(a->value, 42);

    pool.Release(a);
    // After release, acquire again should succeed.
    auto* b = pool.Acquire();
    EXPECT_TRUE(b != nullptr);
}

TEST(ObjectPoolReal_ThreadSafeVariantWorks)
{
    Spark::ObjectPool<TestObject, /*ThreadSafe*/ true> pool(4);
    EXPECT_EQ(pool.GetAvailableCount(), static_cast<size_t>(4));
    auto* a = pool.Acquire();
    EXPECT_TRUE(a != nullptr);
    EXPECT_EQ(pool.GetAvailableCount(), static_cast<size_t>(3));
    pool.Release(a);
    EXPECT_EQ(pool.GetAvailableCount(), static_cast<size_t>(4));
}
