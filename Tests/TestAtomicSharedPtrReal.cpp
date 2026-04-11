/**
 * @file TestAtomicSharedPtrReal.cpp
 * @brief Real-class tests for Spark::AtomicSharedPtr<T>
 *
 * Phase JJ deep-wire.
 */

#include "TestFramework.h"
#include "Utils/AtomicSharedPtr.h"

TEST(AtomicSharedPtrReal_DefaultNull)
{
    Spark::AtomicSharedPtr<int> ptr;
    EXPECT_TRUE(ptr.IsNull());
    EXPECT_TRUE(ptr.Load() == nullptr);
}

TEST(AtomicSharedPtrReal_StoreAndLoad)
{
    Spark::AtomicSharedPtr<int> ptr;
    auto v = std::make_shared<int>(42);
    ptr.Store(v);
    EXPECT_FALSE(ptr.IsNull());
    auto loaded = ptr.Load();
    EXPECT_TRUE(loaded != nullptr);
    if (loaded)
        EXPECT_EQ(*loaded, 42);
}

TEST(AtomicSharedPtrReal_ConstructFromSharedPtr)
{
    auto v = std::make_shared<int>(99);
    Spark::AtomicSharedPtr<int> ptr(v);
    EXPECT_FALSE(ptr.IsNull());
    auto loaded = ptr.Load();
    if (loaded)
        EXPECT_EQ(*loaded, 99);
}

TEST(AtomicSharedPtrReal_Exchange)
{
    Spark::AtomicSharedPtr<int> ptr;
    ptr.Store(std::make_shared<int>(1));

    auto newVal = std::make_shared<int>(2);
    auto old = ptr.Exchange(newVal);

    EXPECT_TRUE(old != nullptr);
    if (old)
        EXPECT_EQ(*old, 1);

    auto current = ptr.Load();
    if (current)
        EXPECT_EQ(*current, 2);
}

TEST(AtomicSharedPtrReal_CompareExchangeSuccess)
{
    Spark::AtomicSharedPtr<int> ptr;
    auto initial = std::make_shared<int>(1);
    ptr.Store(initial);

    auto expected = initial; // Matches.
    auto desired = std::make_shared<int>(5);
    const bool success = ptr.CompareExchange(expected, desired);
    EXPECT_TRUE(success);

    auto current = ptr.Load();
    if (current)
        EXPECT_EQ(*current, 5);
}

TEST(AtomicSharedPtrReal_CompareExchangeFailure)
{
    Spark::AtomicSharedPtr<int> ptr;
    ptr.Store(std::make_shared<int>(1));

    auto expected = std::make_shared<int>(99); // Does not match.
    auto desired = std::make_shared<int>(5);
    const bool success = ptr.CompareExchange(expected, desired);
    EXPECT_FALSE(success);

    // After failure, expected is updated to current.
    if (expected)
        EXPECT_EQ(*expected, 1);
}

TEST(AtomicSharedPtrReal_StoreNullSetsNull)
{
    Spark::AtomicSharedPtr<int> ptr;
    ptr.Store(std::make_shared<int>(7));
    EXPECT_FALSE(ptr.IsNull());
    ptr.Store(nullptr);
    EXPECT_TRUE(ptr.IsNull());
}
