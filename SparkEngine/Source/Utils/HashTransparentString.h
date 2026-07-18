/**
 * @file HashTransparentString.h
 * @brief Transparent string hash/equality functors for heterogeneous map lookup
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides transparent hash and equality functors so
 * `std::unordered_map<std::string, T>` supports heterogeneous lookup by
 * `std::string_view` or `const char*` without constructing a temporary
 * `std::string`. Part of the `Utils/Hash.h` umbrella header.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace Spark
{

    // =========================================================================
    // Transparent string hashing for heterogeneous map lookup
    // =========================================================================

    /**
     * @brief Transparent hash functor for std::unordered_map<std::string, T>.
     *
     * Enables `map.find(string_view)` and `map.find(const char*)` without
     * constructing a temporary std::string. Requires C++20 and an equality
     * comparator with `is_transparent` (see `TransparentStringEqual`).
     *
     * @note Uses `std::hash<std::string_view>`, which the standard requires to
     * hash the same byte sequence identically across `std::string`,
     * `std::string_view`, and `const char*` — so lookups remain correct no
     * matter which key type the caller passes.
     */
    struct TransparentStringHash
    {
        using is_transparent = void;

        [[nodiscard]] size_t operator()(std::string_view sv) const noexcept
        {
            return std::hash<std::string_view>{}(sv);
        }
        [[nodiscard]] size_t operator()(const std::string& s) const noexcept
        {
            return std::hash<std::string_view>{}(s);
        }
        [[nodiscard]] size_t operator()(const char* s) const noexcept { return std::hash<std::string_view>{}(s); }
    };

    /**
     * @brief Transparent equality functor matching @ref TransparentStringHash.
     *
     * All comparisons route through `std::string_view` to avoid allocations.
     */
    struct TransparentStringEqual
    {
        using is_transparent = void;

        [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    };

} // namespace Spark
