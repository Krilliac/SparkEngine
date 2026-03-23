/**
 * @file StringPool.h
 * @brief String interning for O(1) equality comparison of repeated names
 * @author Spark Engine Team
 * @date 2026
 *
 * AI behavior names, animation clips, dialogue node IDs, and asset paths are
 * often stored as separate std::string allocations and compared with O(n)
 * string equality. StringPool interns strings into a stable set, returning
 * a lightweight StringID that can be compared by hash value in O(1).
 *
 * ## Usage
 * @code
 *   Spark::StringPool pool;
 *   Spark::StringID patrol = pool.Intern("Patrol");
 *   Spark::StringID patrol2 = pool.Intern("Patrol");
 *   ASSERT(patrol == patrol2);   // Same hash, same string
 *
 *   std::string_view name = pool.Resolve(patrol);  // "Patrol"
 *
 *   // Global pool for engine-wide interning
 *   auto id = Spark::StringPool::Global().Intern("player_spawn");
 * @endcode
 *
 * @note Hash collisions: if two different strings hash to the same uint64_t
 * (extremely unlikely with FNV-1a 64-bit), the first one wins. The second
 * string will resolve to the first string's text.
 */

#pragma once

#include "Hash.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Spark
{

    /**
     * @struct StringID
     * @brief Lightweight identifier for an interned string. O(1) equality.
     */
    struct StringID
    {
        uint64_t hash = 0;

        [[nodiscard]] constexpr bool operator==(const StringID&) const = default;

        /**
         * @brief Check if this is the null/empty sentinel.
         */
        [[nodiscard]] constexpr bool IsNull() const noexcept { return hash == 0; }

        /**
         * @brief Return the null sentinel.
         */
        [[nodiscard]] static constexpr StringID Null() noexcept { return {}; }
    };

    /**
     * @class StringPool
     * @brief Interns strings and returns stable StringIDs for O(1) comparison.
     *
     * Not thread-safe. Callers must synchronize externally if used across threads.
     */
    class StringPool
    {
      public:
        /**
         * @brief Intern a string, returning its StringID.
         *
         * If the string was already interned, returns the existing ID. The
         * returned StringID is valid for the lifetime of this StringPool.
         */
        [[nodiscard]] StringID Intern(std::string_view str)
        {
            uint64_t h = FNV1a64(str);

            // If this hash is already interned, return it
            if (m_hashToString.contains(h))
                return {h};

            // Insert the string into the stable set and record the mapping
            auto [it, inserted] = m_strings.emplace(str);
            m_hashToString.emplace(h, &(*it));
            return {h};
        }

        /**
         * @brief Resolve a StringID back to its string.
         * @return The interned string, or an empty string_view if not found.
         */
        [[nodiscard]] std::string_view Resolve(StringID id) const
        {
            auto it = m_hashToString.find(id.hash);
            if (it == m_hashToString.end())
                return {};
            return *it->second;
        }

        /**
         * @brief Check if a string has been interned.
         */
        [[nodiscard]] bool Contains(std::string_view str) const
        {
            uint64_t h = FNV1a64(str);
            return m_hashToString.contains(h);
        }

        /**
         * @brief Number of unique strings interned.
         */
        [[nodiscard]] size_t GetCount() const { return m_strings.size(); }

        /**
         * @brief Clear all interned strings. Existing StringIDs become invalid.
         */
        void Clear()
        {
            m_strings.clear();
            m_hashToString.clear();
        }

        /**
         * @brief Access the engine-wide global string pool.
         */
        static StringPool& Global()
        {
            static StringPool instance;
            return instance;
        }

      private:
        // Stable storage: unordered_set does not invalidate pointers on insert
        std::unordered_set<std::string> m_strings;
        // Reverse lookup from hash to stable string pointer
        std::unordered_map<uint64_t, const std::string*> m_hashToString;
    };

} // namespace Spark

// std::hash specialization for use in unordered containers keyed by StringID
template <> struct std::hash<Spark::StringID>
{
    size_t operator()(const Spark::StringID& id) const noexcept { return id.hash; }
};
