/**
 * @file TestAlignedHeapArrayReal.cpp
 * @brief Real-class tests for Spark::FixedHeapArray and DynamicHeapArray
 *
 * Phase JJ deep-wire.
 */

#include "TestFramework.h"
#include "Utils/AlignedHeapArray.h"

#include <cstdint>

TEST(AlignedHeapArrayReal_FixedDefaultConstruct)
{
    Spark::FixedHeapArray<float, 16> arr;
    EXPECT_EQ(arr.size(), static_cast<size_t>(16));
    EXPECT_EQ(arr.byte_size(), static_cast<size_t>(64));
    EXPECT_TRUE(arr.data() != nullptr);
}

TEST(AlignedHeapArrayReal_FixedFillAndAccess)
{
    Spark::FixedHeapArray<int32_t, 8> arr;
    arr.Fill(42);
    for (size_t i = 0; i < arr.size(); ++i)
    {
        EXPECT_EQ(arr[i], 42);
    }
}

TEST(AlignedHeapArrayReal_FixedZero)
{
    Spark::FixedHeapArray<int32_t, 8> arr;
    arr.Fill(99);
    arr.Zero();
    for (size_t i = 0; i < arr.size(); ++i)
    {
        EXPECT_EQ(arr[i], 0);
    }
}

TEST(AlignedHeapArrayReal_FixedAlignment)
{
    Spark::FixedHeapArray<float, 16, 32> arr;
    const auto addr = reinterpret_cast<uintptr_t>(arr.data());
    EXPECT_EQ(addr % 32, static_cast<uintptr_t>(0));
}

TEST(AlignedHeapArrayReal_FixedIteration)
{
    Spark::FixedHeapArray<int, 4> arr;
    arr.Fill(5);
    int sum = 0;
    for (auto it = arr.begin(); it != arr.end(); ++it)
        sum += *it;
    EXPECT_EQ(sum, 20);
}

TEST(AlignedHeapArrayReal_FixedMoveSemantics)
{
    Spark::FixedHeapArray<int, 4> a;
    a.Fill(7);
    Spark::FixedHeapArray<int, 4> b = std::move(a);
    EXPECT_EQ(b[0], 7);
    EXPECT_EQ(b[3], 7);
}
