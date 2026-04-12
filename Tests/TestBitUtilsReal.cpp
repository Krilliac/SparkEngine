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
