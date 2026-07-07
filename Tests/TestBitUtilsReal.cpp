/**
 * @file TestBitUtilsReal.cpp
 * @brief Real-class tests for Spark::BitUtils (replacing fake-coverage TestBitUtils.cpp)
 *
 * Phase JJ deep-wire.
 */

#include "TestFramework.h"
#include "Utils/BitUtils.h"

TEST(BitUtilsReal_PopCount32)
{
    EXPECT_EQ(Spark::BitUtils::PopCount(static_cast<uint32_t>(0)), 0);
    EXPECT_EQ(Spark::BitUtils::PopCount(static_cast<uint32_t>(0xFFu)), 8);
    EXPECT_EQ(Spark::BitUtils::PopCount(static_cast<uint32_t>(0xFFFFFFFFu)), 32);
    EXPECT_EQ(Spark::BitUtils::PopCount(static_cast<uint32_t>(0b10101010u)), 4);
}

TEST(BitUtilsReal_PopCount64)
{
    EXPECT_EQ(Spark::BitUtils::PopCount(static_cast<uint64_t>(0)), 0);
    EXPECT_EQ(Spark::BitUtils::PopCount(static_cast<uint64_t>(0xFFFFFFFFFFFFFFFFULL)), 64);
    EXPECT_EQ(Spark::BitUtils::PopCount(static_cast<uint64_t>(0xFF00FF00FF00FF00ULL)), 32);
}

TEST(BitUtilsReal_TrailingZeros)
{
    EXPECT_EQ(Spark::BitUtils::TrailingZeros(static_cast<uint32_t>(1)), 0);
    EXPECT_EQ(Spark::BitUtils::TrailingZeros(static_cast<uint32_t>(2)), 1);
    EXPECT_EQ(Spark::BitUtils::TrailingZeros(static_cast<uint32_t>(8)), 3);
    EXPECT_EQ(Spark::BitUtils::TrailingZeros(static_cast<uint32_t>(0b10101100u)), 2);
}

TEST(BitUtilsReal_LeadingZeros)
{
    EXPECT_EQ(Spark::BitUtils::LeadingZeros(static_cast<uint32_t>(0x80000000u)), 0);
    EXPECT_EQ(Spark::BitUtils::LeadingZeros(static_cast<uint32_t>(1)), 31);
    EXPECT_EQ(Spark::BitUtils::LeadingZeros(static_cast<uint32_t>(0xFFu)), 24);
}

TEST(BitUtilsReal_IsPowerOfTwo)
{
    EXPECT_TRUE(Spark::BitUtils::IsPowerOfTwo(static_cast<uint64_t>(1)));
    EXPECT_TRUE(Spark::BitUtils::IsPowerOfTwo(static_cast<uint64_t>(2)));
    EXPECT_TRUE(Spark::BitUtils::IsPowerOfTwo(static_cast<uint64_t>(256)));
    EXPECT_TRUE(Spark::BitUtils::IsPowerOfTwo(static_cast<uint64_t>(1024)));
    EXPECT_FALSE(Spark::BitUtils::IsPowerOfTwo(static_cast<uint64_t>(3)));
    EXPECT_FALSE(Spark::BitUtils::IsPowerOfTwo(static_cast<uint64_t>(0)));
    EXPECT_FALSE(Spark::BitUtils::IsPowerOfTwo(static_cast<uint64_t>(100)));
}

TEST(BitUtilsReal_PopCountCompileTime)
{
    // Verify constexpr nature by using in a static_assert-like context.
    constexpr int count = Spark::BitUtils::PopCount(static_cast<uint32_t>(0b1111u));
    EXPECT_EQ(count, 4);
}

TEST(BitUtilsReal_NextPowerOfTwo)
{
    // 0 is a special case that returns 1.
    EXPECT_EQ(Spark::BitUtils::NextPowerOfTwo(0), static_cast<uint64_t>(1));
    EXPECT_EQ(Spark::BitUtils::NextPowerOfTwo(1), static_cast<uint64_t>(1));
    // Exact powers of two are returned unchanged.
    EXPECT_EQ(Spark::BitUtils::NextPowerOfTwo(8), static_cast<uint64_t>(8));
    // Non-powers round up to the next power of two.
    EXPECT_EQ(Spark::BitUtils::NextPowerOfTwo(5), static_cast<uint64_t>(8));
    EXPECT_EQ(Spark::BitUtils::NextPowerOfTwo(100), static_cast<uint64_t>(128));
    // Large boundary near 2^63.
    EXPECT_EQ(Spark::BitUtils::NextPowerOfTwo((1ULL << 62) + 1), (1ULL << 63));
}

TEST(BitUtilsReal_ForEachSetBit)
{
    // Empty mask: the callback is never invoked.
    int counter = 0;
    Spark::BitUtils::ForEachSetBit(static_cast<uint64_t>(0), [&counter](int) { ++counter; });
    EXPECT_EQ(counter, 0);

    // Single bit: exactly one invocation with the correct index.
    std::vector<int> single;
    Spark::BitUtils::ForEachSetBit(static_cast<uint64_t>(1) << 5, [&single](int bit) { single.push_back(bit); });
    EXPECT_EQ(single.size(), static_cast<size_t>(1));
    EXPECT_EQ(single[0], 5);

    // All 64 bits set: 64 invocations.
    counter = 0;
    Spark::BitUtils::ForEachSetBit(0xFFFFFFFFFFFFFFFFULL, [&counter](int) { ++counter; });
    EXPECT_EQ(counter, 64);

    // LSB-first index order: 0b10101100 yields 2, 3, 5, 7 in ascending order.
    std::vector<int> indices;
    Spark::BitUtils::ForEachSetBit(static_cast<uint64_t>(0b10101100), [&indices](int bit) { indices.push_back(bit); });
    EXPECT_EQ(indices.size(), static_cast<size_t>(4));
    EXPECT_EQ(indices[0], 2);
    EXPECT_EQ(indices[1], 3);
    EXPECT_EQ(indices[2], 5);
    EXPECT_EQ(indices[3], 7);
}
