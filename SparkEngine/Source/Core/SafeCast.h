/**
 * @file SafeCast.h
 * @brief Type-safe cast utilities with debug-mode validation
 *
 * Provides Spark::NarrowCast<To>(from) for checked narrowing conversions
 * and Spark::CheckedCast<Derived*>(base) for validated downcasts.
 *
 * Both are zero-cost in Release builds (compile to static_cast).
 * In Debug builds they assert that no data is lost or types are mismatched.
 *
 * NOT intended to replace reinterpret_cast in low-level code (sockets, GPU,
 * serialization) — those casts are inherently unsafe by necessity.
 */

#pragma once

#include "Utils/Assert.h"

#include <concepts>
#include <limits>
#include <type_traits>

namespace Spark
{

    // ============================================================================
    // NarrowCast — checked narrowing numeric conversion
    // ============================================================================

    /**
 * @brief Narrowing cast with debug-mode overflow/truncation check
 *
 * In debug builds, asserts that the round-trip cast preserves the value.
 * In release builds, compiles to a plain static_cast with zero overhead.
 *
 * Usage:
 *   size_t bigVal = GetSize();
 *   int smallVal = Spark::NarrowCast<int>(bigVal);  // asserts if truncated
 */
    template <typename To, typename From>
        requires std::is_arithmetic_v<To> && std::is_arithmetic_v<From>
    constexpr To NarrowCast(From value)
    {
        auto result = static_cast<To>(value);
#if defined(_DEBUG) || defined(DEBUG)
        // An exact round-trip is only meaningful for integer targets, where a changed
        // value means truncation/overflow (a bug). Narrowing to a floating-point type
        // (e.g. double->float) loses precision by design, so an equality check there
        // would fire on legitimate conversions — skip it.
        if constexpr (std::is_integral_v<To>)
        {
            ASSERT_MSG(static_cast<From>(result) == value, "NarrowCast: value changed during conversion");
            // For signed/unsigned mismatch, also check sign preservation
            if constexpr (std::is_signed_v<To> != std::is_signed_v<From>)
            {
                ASSERT_MSG((value >= From{}) == (result >= To{}), "NarrowCast: sign changed during conversion");
            }
        }
#endif
        return result;
    }

    // ============================================================================
    // CheckedCast — validated pointer downcast
    // ============================================================================

    /**
 * @brief Safe downcast with debug-mode dynamic_cast verification
 *
 * In debug builds, verifies the cast via dynamic_cast and asserts on mismatch.
 * In release builds, compiles to a plain static_cast with zero overhead.
 *
 * Use this for known-safe downcasts where you've already checked the type
 * but want debug-mode safety nets. For truly polymorphic dispatch, prefer
 * virtual functions or dynamic_cast directly.
 *
 * Usage:
 *   RHIDevice* base = GetDevice();
 *   auto* d3d = Spark::CheckedCast<D3D11Device*>(base);
 */
    template <typename To, typename From>
        requires std::is_pointer_v<To> && std::is_pointer_v<From>
    To CheckedCast(From ptr)
    {
        if (ptr == nullptr)
        {
            return nullptr;
        }

#if defined(_DEBUG) || defined(DEBUG)
        auto* result = dynamic_cast<To>(ptr);
        ASSERT_MSG(result != nullptr, "CheckedCast: dynamic_cast failed — type mismatch");
        return result;
#else
        return static_cast<To>(ptr);
#endif
    }

} // namespace Spark
