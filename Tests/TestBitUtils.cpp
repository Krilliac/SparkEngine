// TestBitUtils.cpp - Tests for BitUtils helpers
// Tests PopCount, TrailingZeros, LeadingZeros, IsPowerOfTwo, NextPowerOfTwo, ForEachSetBit

#include "TestFramework.h"
#include <bit>
#include <cstdint>
#include <vector>

// Simplified BitUtils for testing (mirrors engine implementation)
namespace BitUtils
{
    [[nodiscard]] constexpr int PopCount(uint32_t value) noexcept
    {
        return std::popcount(value);
    }
    [[nodiscard]] constexpr int PopCount(uint64_t value) noexcept
    {
        return std::popcount(value);
    }
    [[nodiscard]] constexpr int TrailingZeros(uint32_t value) noexcept
    {
        return std::countr_zero(value);
    }
    [[nodiscard]] constexpr int TrailingZeros(uint64_t value) noexcept
    {
        return std::countr_zero(value);
    }
    [[nodiscard]] constexpr int LeadingZeros(uint32_t value) noexcept
    {
        return std::countl_zero(value);
    }
    [[nodiscard]] constexpr int LeadingZeros(uint64_t value) noexcept
    {
        return std::countl_zero(value);
    }
    [[nodiscard]] constexpr bool IsPowerOfTwo(uint64_t value) noexcept
    {
        return std::has_single_bit(value);
    }

    [[nodiscard]] constexpr uint64_t NextPowerOfTwo(uint64_t value) noexcept
    {
        if (value == 0)
            return 1;
        return std::bit_ceil(value);
    }

    template <typename Fn> constexpr void ForEachSetBit(uint64_t mask, Fn&& fn)
    {
        while (mask != 0)
        {
            int bit = std::countr_zero(mask);
            fn(bit);
            mask &= mask - 1;
        }
    }
} // namespace BitUtils

TEST(BitUtils_PopCount_Zero)
{
    EXPECT_EQ(BitUtils::PopCount(uint32_t(0)), 0);
}

TEST(BitUtils_PopCount_One)
{
    EXPECT_EQ(BitUtils::PopCount(uint32_t(1)), 1);
}

TEST(BitUtils_PopCount_AllBits)
{
    EXPECT_EQ(BitUtils::PopCount(uint32_t(0xFF)), 8);
    EXPECT_EQ(BitUtils::PopCount(UINT32_MAX), 32);
}

TEST(BitUtils_PopCount_64bit)
{
    EXPECT_EQ(BitUtils::PopCount(uint64_t(0)), 0);
    EXPECT_EQ(BitUtils::PopCount(UINT64_MAX), 64);
}

TEST(BitUtils_TrailingZeros)
{
    EXPECT_EQ(BitUtils::TrailingZeros(uint32_t(1)), 0);
    EXPECT_EQ(BitUtils::TrailingZeros(uint32_t(8)), 3);
    EXPECT_EQ(BitUtils::TrailingZeros(uint32_t(0)), 32);
}

TEST(BitUtils_LeadingZeros)
{
    EXPECT_EQ(BitUtils::LeadingZeros(uint32_t(1)), 31);
    EXPECT_EQ(BitUtils::LeadingZeros(uint32_t(0)), 32);
    EXPECT_EQ(BitUtils::LeadingZeros(UINT32_MAX), 0);
}

TEST(BitUtils_IsPowerOfTwo)
{
    EXPECT_FALSE(BitUtils::IsPowerOfTwo(0));
    EXPECT_TRUE(BitUtils::IsPowerOfTwo(1));
    EXPECT_TRUE(BitUtils::IsPowerOfTwo(2));
    EXPECT_FALSE(BitUtils::IsPowerOfTwo(3));
    EXPECT_TRUE(BitUtils::IsPowerOfTwo(256));
    EXPECT_FALSE(BitUtils::IsPowerOfTwo(255));
}

TEST(BitUtils_NextPowerOfTwo)
{
    EXPECT_EQ(BitUtils::NextPowerOfTwo(0), 1u);
    EXPECT_EQ(BitUtils::NextPowerOfTwo(1), 1u);
    EXPECT_EQ(BitUtils::NextPowerOfTwo(3), 4u);
    EXPECT_EQ(BitUtils::NextPowerOfTwo(8), 8u);
    EXPECT_EQ(BitUtils::NextPowerOfTwo(9), 16u);
}

TEST(BitUtils_ForEachSetBit)
{
    std::vector<int> bits;
    BitUtils::ForEachSetBit(0b1010'1100ULL, [&](int bit) { bits.push_back(bit); });

    EXPECT_EQ(bits.size(), 4u);
    EXPECT_EQ(bits[0], 2);
    EXPECT_EQ(bits[1], 3);
    EXPECT_EQ(bits[2], 5);
    EXPECT_EQ(bits[3], 7);
}

TEST(BitUtils_ForEachSetBit_Empty)
{
    int count = 0;
    BitUtils::ForEachSetBit(0ULL, [&](int) { ++count; });
    EXPECT_EQ(count, 0);
}
