/**
 * @file HashCombine.h
 * @brief Hash-combining helpers for building composite hash values
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a hash-combining helper modelled after `boost::hash_combine` for
 * building composite hash values from multiple fields. Part of the
 * `Utils/Hash.h` umbrella header.
 */

#pragma once

#include <cstddef>

namespace Spark
{

    // =========================================================================
    // CombineHash — combine two hash values into one
    // =========================================================================

    /**
     * @brief Combine an existing hash seed with a new hash value.
     *
     * Uses the same mixing formula as `boost::hash_combine`. Call repeatedly to
     * fold multiple fields into a single composite hash:
     *
     * @code
     *   size_t h = 0;
     *   Spark::CombineHash(h, std::hash<int>{}(x));
     *   Spark::CombineHash(h, std::hash<int>{}(y));
     *   Spark::CombineHash(h, std::hash<int>{}(z));
     * @endcode
     *
     * @param seed   The running hash accumulator (modified in-place).
     * @param value  A new hash value to fold in.
     */
    inline void CombineHash(size_t& seed, size_t value) noexcept
    {
        // Magic constant: golden ratio phi ≈ 2^64 / φ, chosen for avalanche effect
        seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    }

    /**
     * @brief Return a new combined hash from two values without modifying either.
     *
     * Non-modifying variant suitable for one-shot composite key construction.
     * Distinct name from `CombineHash` to avoid overload ambiguity.
     *
     * @param a  First hash value.
     * @param b  Second hash value to combine.
     * @return   New combined hash.
     */
    [[nodiscard]] inline size_t HashCombine(size_t a, size_t b) noexcept
    {
        CombineHash(a, b);
        return a;
    }

} // namespace Spark
