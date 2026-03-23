/**
 * @file BitUtils.h
 * @brief Constexpr bit manipulation utilities wrapping C++20 <bit>
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides concise, named helpers for common bit operations used in collision
 * masks, render layers, and component type IDs. All functions are `constexpr`
 * and `noexcept`.
 *
 * ## Usage
 * @code
 *   uint32_t mask = 0b1010'1100;
 *   int count = Spark::BitUtils::PopCount(mask);        // 4
 *   int tz    = Spark::BitUtils::TrailingZeros(mask);   // 2
 *   bool pow2 = Spark::BitUtils::IsPowerOfTwo(256u);    // true
 *
 *   Spark::BitUtils::ForEachSetBit(mask, [](int bit) {
 *       // Called with bit = 2, 3, 5, 7
 *   });
 * @endcode
 */

#pragma once

#include <bit>
#include <cstdint>

namespace Spark::BitUtils
{

    /**
     * @brief Count the number of set bits (population count).
     */
    [[nodiscard]] constexpr int PopCount(uint32_t value) noexcept
    {
        return std::popcount(value);
    }

    /** @overload */
    [[nodiscard]] constexpr int PopCount(uint64_t value) noexcept
    {
        return std::popcount(value);
    }

    /**
     * @brief Count trailing zero bits (index of the lowest set bit).
     * @return Number of trailing zeros, or bit width if value is 0.
     */
    [[nodiscard]] constexpr int TrailingZeros(uint32_t value) noexcept
    {
        return std::countr_zero(value);
    }

    /** @overload */
    [[nodiscard]] constexpr int TrailingZeros(uint64_t value) noexcept
    {
        return std::countr_zero(value);
    }

    /**
     * @brief Count leading zero bits.
     * @return Number of leading zeros, or bit width if value is 0.
     */
    [[nodiscard]] constexpr int LeadingZeros(uint32_t value) noexcept
    {
        return std::countl_zero(value);
    }

    /** @overload */
    [[nodiscard]] constexpr int LeadingZeros(uint64_t value) noexcept
    {
        return std::countl_zero(value);
    }

    /**
     * @brief Check if a value is a power of two.
     * @note Returns false for 0.
     */
    [[nodiscard]] constexpr bool IsPowerOfTwo(uint64_t value) noexcept
    {
        return std::has_single_bit(value);
    }

    /**
     * @brief Return the smallest power of two >= value.
     * @note Returns 1 for value 0.
     */
    [[nodiscard]] constexpr uint64_t NextPowerOfTwo(uint64_t value) noexcept
    {
        if (value == 0)
            return 1;
        return std::bit_ceil(value);
    }

    /**
     * @brief Invoke a callable for each set bit index, LSB-first.
     * @param mask The bitmask to iterate.
     * @param fn   Called with the zero-based index of each set bit.
     */
    template <typename Fn> constexpr void ForEachSetBit(uint64_t mask, Fn&& fn)
    {
        while (mask != 0)
        {
            int bit = std::countr_zero(mask);
            fn(bit);
            mask &= mask - 1; // Clear lowest set bit
        }
    }

} // namespace Spark::BitUtils
