/**
 * @file BitFlags.h
 * @brief Type-safe bitwise operations for enum class flags
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a BitFlags<E> wrapper and an ENABLE_BITMASK_OPERATORS macro
 * for type-safe flag operations on scoped enums, replacing manual bit
 * shifting and masking throughout the engine.
 *
 * ## Usage
 * @code
 *   enum class RenderPass : uint32_t {
 *       None       = 0,
 *       Shadow     = 1 << 0,
 *       GBuffer    = 1 << 1,
 *       Lighting   = 1 << 2,
 *       PostFX     = 1 << 3,
 *       UI         = 1 << 4,
 *       All        = 0xFFFFFFFF
 *   };
 *   SPARK_ENABLE_BITMASK_OPERATORS(RenderPass)
 *
 *   RenderPass passes = RenderPass::Shadow | RenderPass::GBuffer;
 *   if ((passes & RenderPass::Shadow) != RenderPass::None) { ... }
 *
 *   // Or use the BitFlags wrapper for richer API:
 *   Spark::BitFlags<RenderPass> flags;
 *   flags.Set(RenderPass::Shadow);
 *   flags.Set(RenderPass::Lighting);
 *   bool hasShadow = flags.Has(RenderPass::Shadow);  // true
 *   flags.Clear(RenderPass::Shadow);
 * @endcode
 */

#pragma once

#include <cstdint>
#include <type_traits>
#include <utility>
#include "TypeTraits.h"

// =============================================================================
// Free operator overloads for enum class bitmasks
// =============================================================================

/**
 * @brief Enable |, &, ^, ~ operators for a scoped enum.
 *
 * Place this macro after the enum definition, at namespace scope.
 * The enum's underlying type must be an integer type.
 */
#define SPARK_ENABLE_BITMASK_OPERATORS(EnumType)                                                                       \
    inline constexpr EnumType operator|(EnumType lhs, EnumType rhs)                                                    \
    {                                                                                                                  \
        return static_cast<EnumType>(std::to_underlying(lhs) | std::to_underlying(rhs));                               \
    }                                                                                                                  \
    inline constexpr EnumType operator&(EnumType lhs, EnumType rhs)                                                    \
    {                                                                                                                  \
        return static_cast<EnumType>(std::to_underlying(lhs) & std::to_underlying(rhs));                               \
    }                                                                                                                  \
    inline constexpr EnumType operator^(EnumType lhs, EnumType rhs)                                                    \
    {                                                                                                                  \
        return static_cast<EnumType>(std::to_underlying(lhs) ^ std::to_underlying(rhs));                               \
    }                                                                                                                  \
    inline constexpr EnumType operator~(EnumType val)                                                                  \
    {                                                                                                                  \
        return static_cast<EnumType>(~std::to_underlying(val));                                                        \
    }                                                                                                                  \
    inline constexpr EnumType& operator|=(EnumType& lhs, EnumType rhs)                                                 \
    {                                                                                                                  \
        lhs = lhs | rhs;                                                                                               \
        return lhs;                                                                                                    \
    }                                                                                                                  \
    inline constexpr EnumType& operator&=(EnumType& lhs, EnumType rhs)                                                 \
    {                                                                                                                  \
        lhs = lhs & rhs;                                                                                               \
        return lhs;                                                                                                    \
    }                                                                                                                  \
    inline constexpr EnumType& operator^=(EnumType& lhs, EnumType rhs)                                                 \
    {                                                                                                                  \
        lhs = lhs ^ rhs;                                                                                               \
        return lhs;                                                                                                    \
    }

namespace Spark
{

    /**
     * @class BitFlags
     * @brief Type-safe bitmask wrapper for enum class types.
     *
     * Provides a richer API (Set, Clear, Has, Toggle) while maintaining
     * type safety. The underlying storage matches the enum's underlying type.
     *
     * @tparam E  Enum class type with power-of-two values.
     */
    template <Spark::Enum E> class BitFlags
    {
        using Underlying = std::underlying_type_t<E>;

      public:
        constexpr BitFlags() : m_value(0) {}
        constexpr BitFlags(E flag) : m_value(std::to_underlying(flag)) {}
        constexpr explicit BitFlags(Underlying raw) : m_value(raw) {}

        /**
         * @brief Set one or more flags.
         * @param flag  Flag(s) to set.
         */
        constexpr void Set(E flag) { m_value |= std::to_underlying(flag); }

        /**
         * @brief Clear one or more flags.
         * @param flag  Flag(s) to clear.
         */
        constexpr void Clear(E flag) { m_value &= ~std::to_underlying(flag); }

        /**
         * @brief Toggle one or more flags.
         * @param flag  Flag(s) to toggle.
         */
        constexpr void Toggle(E flag) { m_value ^= std::to_underlying(flag); }

        /**
         * @brief Check if all specified flags are set.
         * @param flag  Flag(s) to test.
         * @return      true if all specified bits are set.
         */
        constexpr bool Has(E flag) const
        {
            auto f = std::to_underlying(flag);
            return (m_value & f) == f;
        }

        /**
         * @brief Check if any of the specified flags are set.
         * @param flag  Flag(s) to test.
         * @return      true if any specified bit is set.
         */
        constexpr bool HasAny(E flag) const { return (m_value & std::to_underlying(flag)) != 0; }

        /**
         * @brief Check if no flags are set.
         * @return  true if the value is zero.
         */
        constexpr bool IsEmpty() const { return m_value == 0; }

        /**
         * @brief Clear all flags.
         */
        constexpr void ClearAll() { m_value = 0; }

        /**
         * @brief Get the raw underlying value.
         * @return  Raw integer value of the flags.
         */
        constexpr Underlying GetRaw() const { return m_value; }

        constexpr bool operator==(const BitFlags& other) const { return m_value == other.m_value; }
        constexpr bool operator!=(const BitFlags& other) const { return m_value != other.m_value; }

      private:
        Underlying m_value;
    };

} // namespace Spark
