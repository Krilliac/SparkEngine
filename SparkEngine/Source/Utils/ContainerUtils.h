/**
 * @file ContainerUtils.h
 * @brief Safe container lookup and mutation helpers
 * @author Spark Engine Team
 * @date 2026
 *
 * Replaces scattered `map.find(key) != map.end()` and erase-if patterns with
 * concise, type-safe one-liners. All functions are `constexpr` where the
 * underlying container supports it.
 *
 * ## Usage
 * @code
 *   std::unordered_map<std::string, int> scores;
 *
 *   // Safe lookup returning std::optional
 *   auto val = Spark::ContainerUtils::MapFind(scores, "player1");
 *   if (val) { DoSomething(*val); }
 *
 *   // Lookup with fallback
 *   int s = Spark::ContainerUtils::MapFindOr(scores, "player1", 0);
 *
 *   // Erase elements matching a predicate
 *   Spark::ContainerUtils::EraseIf(scores, [](const auto& kv) { return kv.second < 0; });
 *
 *   // Membership check for any container
 *   std::vector<int> ids = {1, 2, 3};
 *   bool found = Spark::ContainerUtils::Contains(ids, 2);
 * @endcode
 */

#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Spark::ContainerUtils
{

    namespace Detail
    {
        // Detect whether a container has a .find() member (maps, sets)
        template <typename C, typename K, typename = void> struct HasFindMember : std::false_type
        {
        };

        template <typename C, typename K>
        struct HasFindMember<C, K, std::void_t<decltype(std::declval<const C&>().find(std::declval<K>()))>>
            : std::true_type
        {
        };

        // Detect whether a container has a .contains() member
        template <typename C, typename V, typename = void> struct HasContainsMember : std::false_type
        {
        };

        template <typename C, typename V>
        struct HasContainsMember<C, V, std::void_t<decltype(std::declval<const C&>().contains(std::declval<V>()))>>
            : std::true_type
        {
        };
    } // namespace Detail

    /**
     * @brief Look up a key in an associative container, returning std::optional.
     * @return The mapped value if found, or std::nullopt.
     */
    template <typename Map, typename Key>
    [[nodiscard]] constexpr auto MapFind(const Map& map, const Key& key) -> std::optional<typename Map::mapped_type>
    {
        auto it = map.find(key);
        if (it != map.end())
            return it->second;
        return std::nullopt;
    }

    /**
     * @brief Look up a key in an associative container with a default fallback.
     * @return The mapped value if found, or the provided default.
     */
    template <typename Map, typename Key, typename Default>
    [[nodiscard]] constexpr auto MapFindOr(const Map& map, const Key& key, Default&& defaultVal) ->
        typename Map::mapped_type
    {
        auto it = map.find(key);
        if (it != map.end())
            return it->second;
        return static_cast<typename Map::mapped_type>(std::forward<Default>(defaultVal));
    }

    /**
     * @brief Erase all elements matching a predicate.
     *
     * Uses `std::erase_if` for associative containers and the erase-remove idiom
     * for sequence containers.
     *
     * @return Number of elements removed.
     */
    template <typename Container, typename Pred>
    constexpr auto EraseIf(Container& container, Pred pred) -> typename Container::size_type
    {
        return std::erase_if(container, pred);
    }

    /**
     * @brief Check if a container contains a value.
     *
     * Uses `.contains()` for associative containers (maps, sets) and
     * `std::find` for sequence containers (vectors, arrays).
     */
    template <typename Container, typename Value>
    [[nodiscard]] constexpr bool Contains(const Container& container, const Value& value)
    {
        if constexpr (Detail::HasContainsMember<Container, Value>::value)
        {
            return container.contains(value);
        }
        else
        {
            return std::find(container.begin(), container.end(), value) != container.end();
        }
    }

} // namespace Spark::ContainerUtils
