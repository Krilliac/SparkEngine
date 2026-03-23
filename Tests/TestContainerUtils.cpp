// TestContainerUtils.cpp - Tests for ContainerUtils helpers
// Tests MapFind, MapFindOr, EraseIf, Contains

#include "TestFramework.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Simplified ContainerUtils for testing (mirrors engine implementation)
namespace ContainerUtils
{
    namespace Detail
    {
        template <typename C, typename V, typename = void> struct HasContainsMember : std::false_type
        {
        };

        template <typename C, typename V>
        struct HasContainsMember<C, V, std::void_t<decltype(std::declval<const C&>().contains(std::declval<V>()))>>
            : std::true_type
        {
        };
    } // namespace Detail

    template <typename Map, typename Key>
    [[nodiscard]] auto MapFind(const Map& map, const Key& key) -> std::optional<typename Map::mapped_type>
    {
        auto it = map.find(key);
        if (it != map.end())
            return it->second;
        return std::nullopt;
    }

    template <typename Map, typename Key, typename Default>
    [[nodiscard]] auto MapFindOr(const Map& map, const Key& key, Default&& defaultVal) -> typename Map::mapped_type
    {
        auto it = map.find(key);
        if (it != map.end())
            return it->second;
        return static_cast<typename Map::mapped_type>(std::forward<Default>(defaultVal));
    }

    template <typename Container, typename Pred>
    auto EraseIf(Container& container, Pred pred) -> typename Container::size_type
    {
        return std::erase_if(container, pred);
    }

    template <typename Container, typename Value>
    [[nodiscard]] bool Contains(const Container& container, const Value& value)
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
} // namespace ContainerUtils

TEST(ContainerUtils_MapFind_Found)
{
    std::map<std::string, int> m = {{"a", 1}, {"b", 2}};
    auto result = ContainerUtils::MapFind(m, std::string("a"));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1);
}

TEST(ContainerUtils_MapFind_NotFound)
{
    std::map<std::string, int> m = {{"a", 1}};
    auto result = ContainerUtils::MapFind(m, std::string("z"));
    EXPECT_FALSE(result.has_value());
}

TEST(ContainerUtils_MapFind_UnorderedMap)
{
    std::unordered_map<int, std::string> m = {{1, "one"}, {2, "two"}};
    auto result = ContainerUtils::MapFind(m, 2);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::string("two"));
}

TEST(ContainerUtils_MapFindOr_Found)
{
    std::map<int, int> m = {{1, 100}};
    int val = ContainerUtils::MapFindOr(m, 1, 0);
    EXPECT_EQ(val, 100);
}

TEST(ContainerUtils_MapFindOr_Default)
{
    std::map<int, int> m = {{1, 100}};
    int val = ContainerUtils::MapFindOr(m, 99, -1);
    EXPECT_EQ(val, -1);
}

TEST(ContainerUtils_EraseIf_Vector)
{
    std::vector<int> v = {1, 2, 3, 4, 5};
    auto removed = ContainerUtils::EraseIf(v, [](int x) { return x % 2 == 0; });
    EXPECT_EQ(removed, 2u);
    EXPECT_EQ(v.size(), 3u);
}

TEST(ContainerUtils_EraseIf_Map)
{
    std::map<std::string, int> m = {{"a", 1}, {"b", -1}, {"c", 3}};
    auto removed = ContainerUtils::EraseIf(m, [](const auto& kv) { return kv.second < 0; });
    EXPECT_EQ(removed, 1u);
    EXPECT_EQ(m.size(), 2u);
}

TEST(ContainerUtils_Contains_Vector)
{
    std::vector<int> v = {10, 20, 30};
    EXPECT_TRUE(ContainerUtils::Contains(v, 20));
    EXPECT_FALSE(ContainerUtils::Contains(v, 99));
}

TEST(ContainerUtils_Contains_Set)
{
    std::set<std::string> s = {"hello", "world"};
    EXPECT_TRUE(ContainerUtils::Contains(s, std::string("hello")));
    EXPECT_FALSE(ContainerUtils::Contains(s, std::string("missing")));
}

TEST(ContainerUtils_Contains_Map)
{
    std::map<int, int> m = {{1, 10}, {2, 20}};
    EXPECT_TRUE(ContainerUtils::Contains(m, 1));
    EXPECT_FALSE(ContainerUtils::Contains(m, 3));
}
