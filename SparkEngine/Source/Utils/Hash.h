/**
 * @file Hash.h
 * @brief Compile-time and runtime hash utilities for strings and composite keys
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides FNV-1a hashing (32-bit and 64-bit) with `constexpr` support so
 * string keys can be hashed at compile time. Includes a hash-combining helper
 * modelled after `boost::hash_combine` for building composite hash values from
 * multiple fields, and user-defined literals for ergonomic compile-time hashing.
 *
 * ## Why FNV-1a?
 * FNV-1a is simple, dependency-free, produces excellent distribution for short
 * ASCII strings (common in game engines: asset names, command names, component
 * type tags), and is `constexpr`-friendly with no lookup tables.
 *
 * ## Usage
 * @code
 *   using namespace Spark::HashLiterals;
 *
 *   // Compile-time string hash (no runtime cost)
 *   constexpr uint64_t key = "player_spawn"_hash64;
 *   constexpr uint32_t key32 = "player_spawn"_hash32;
 *
 *   // Runtime hashing
 *   uint64_t h = Spark::FNV1a64("some_asset_name");
 *
 *   // Composite key for std::unordered_map
 *   size_t combined = Spark::CombineHash(typeHash, instanceId);
 *
 *   // Switch on a string (compile-time dispatch)
 *   switch (Spark::FNV1a64(commandName)) {
 *       case "quit"_hash64:  DoQuit();  break;
 *       case "reload"_hash64: DoReload(); break;
 *   }
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Spark
{

    // =========================================================================
    // FNV-1a constants
    // =========================================================================

    namespace Detail
    {
        inline constexpr uint32_t FNV_PRIME_32 = 16777619u;
        inline constexpr uint32_t FNV_OFFSET_32 = 2166136261u;

        inline constexpr uint64_t FNV_PRIME_64 = 1099511628211ull;
        inline constexpr uint64_t FNV_OFFSET_64 = 14695981039346656037ull;
    } // namespace Detail

    // =========================================================================
    // FNV-1a 32-bit
    // =========================================================================

    /**
     * @brief Compute a 32-bit FNV-1a hash of a null-terminated C string.
     *
     * This overload is `constexpr`, so when called with a string literal the
     * compiler computes the hash at compile time with zero runtime cost.
     *
     * @param str  Null-terminated string to hash. Must not be null.
     * @return     32-bit FNV-1a hash value.
     */
    [[nodiscard]] constexpr uint32_t FNV1a32(const char* str) noexcept
    {
        uint32_t hash = Detail::FNV_OFFSET_32;
        while (*str != '\0')
        {
            hash ^= static_cast<uint32_t>(static_cast<unsigned char>(*str));
            hash *= Detail::FNV_PRIME_32;
            ++str;
        }
        return hash;
    }

    /**
     * @brief Compute a 32-bit FNV-1a hash of a `std::string_view`.
     *
     * @param sv  String view to hash (may contain embedded nulls).
     * @return    32-bit FNV-1a hash value.
     */
    [[nodiscard]] constexpr uint32_t FNV1a32(std::string_view sv) noexcept
    {
        uint32_t hash = Detail::FNV_OFFSET_32;
        for (char c : sv)
        {
            hash ^= static_cast<uint32_t>(static_cast<unsigned char>(c));
            hash *= Detail::FNV_PRIME_32;
        }
        return hash;
    }

    /**
     * @brief Compute a 32-bit FNV-1a hash of a raw byte buffer.
     *
     * @param data  Pointer to the first byte. Must not be null if `size > 0`.
     * @param size  Number of bytes to hash.
     * @return      32-bit FNV-1a hash value.
     */
    [[nodiscard]] constexpr uint32_t FNV1a32(const void* data, size_t size) noexcept
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        uint32_t hash = Detail::FNV_OFFSET_32;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= static_cast<uint32_t>(bytes[i]);
            hash *= Detail::FNV_PRIME_32;
        }
        return hash;
    }

    // =========================================================================
    // FNV-1a 64-bit
    // =========================================================================

    /**
     * @brief Compute a 64-bit FNV-1a hash of a null-terminated C string.
     *
     * Preferred over the 32-bit variant for map keys where collision resistance
     * matters. `constexpr`-capable for compile-time evaluation.
     *
     * @param str  Null-terminated string to hash. Must not be null.
     * @return     64-bit FNV-1a hash value.
     */
    [[nodiscard]] constexpr uint64_t FNV1a64(const char* str) noexcept
    {
        uint64_t hash = Detail::FNV_OFFSET_64;
        while (*str != '\0')
        {
            hash ^= static_cast<uint64_t>(static_cast<unsigned char>(*str));
            hash *= Detail::FNV_PRIME_64;
            ++str;
        }
        return hash;
    }

    /**
     * @brief Compute a 64-bit FNV-1a hash of a `std::string_view`.
     *
     * @param sv  String view to hash.
     * @return    64-bit FNV-1a hash value.
     */
    [[nodiscard]] constexpr uint64_t FNV1a64(std::string_view sv) noexcept
    {
        uint64_t hash = Detail::FNV_OFFSET_64;
        for (char c : sv)
        {
            hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
            hash *= Detail::FNV_PRIME_64;
        }
        return hash;
    }

    /**
     * @brief Compute a 64-bit FNV-1a hash of a raw byte buffer.
     *
     * @param data  Pointer to the first byte. Must not be null if `size > 0`.
     * @param size  Number of bytes to hash.
     * @return      64-bit FNV-1a hash value.
     */
    [[nodiscard]] constexpr uint64_t FNV1a64(const void* data, size_t size) noexcept
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        uint64_t hash = Detail::FNV_OFFSET_64;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= static_cast<uint64_t>(bytes[i]);
            hash *= Detail::FNV_PRIME_64;
        }
        return hash;
    }

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

    // =========================================================================
    // User-defined literals
    // =========================================================================

    /**
     * @brief User-defined literal namespace for hash literals.
     *
     * Import with `using namespace Spark::HashLiterals;` to enable
     * `"name"_hash32` and `"name"_hash64` syntax.
     */
    namespace HashLiterals
    {
        /**
         * @brief Compute a 64-bit FNV-1a hash at compile time.
         *
         * @code
         *   using namespace Spark::HashLiterals;
         *   constexpr uint64_t k = "player_spawn"_hash64;
         * @endcode
         */
        [[nodiscard]] consteval uint64_t operator""_hash64(const char* str, size_t /*len*/) noexcept
        {
            return FNV1a64(str);
        }

        /**
         * @brief Compute a 32-bit FNV-1a hash at compile time.
         *
         * @code
         *   using namespace Spark::HashLiterals;
         *   constexpr uint32_t k = "player_spawn"_hash32;
         * @endcode
         */
        [[nodiscard]] consteval uint32_t operator""_hash32(const char* str, size_t /*len*/) noexcept
        {
            return FNV1a32(str);
        }
    } // namespace HashLiterals

} // namespace Spark
