/**
 * @file TestContainerUtilsReal.cpp
 * @brief Real-class tests for Spark::ContainerUtils helpers
 */

#include "TestFramework.h"
#include "Utils/ContainerUtils.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================================
// MapFind
// ============================================================================

TEST(ContainerUtilsReal_MapFindPresent)
{
    std::unordered_map<std::string, int> m = {{"a", 1}, {"b", 2}};
    auto result = Spark::ContainerUtils::MapFind(m, std::string("a"));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1);
}

TEST(ContainerUtilsReal_MapFindMissing)
{
    std::unordered_map<std::string, int> m = {{"a", 1}};
    auto result = Spark::ContainerUtils::MapFind(m, std::string("z"));
    EXPECT_FALSE(result.has_value());
}

TEST(ContainerUtilsReal_MapFindOrPresent)
{
    std::map<int, std::string> m = {{1, "one"}, {2, "two"}};
    auto result = Spark::ContainerUtils::MapFindOr(m, 1, std::string("default"));
    EXPECT_EQ(result, std::string("one"));
}

TEST(ContainerUtilsReal_MapFindOrMissing)
{
    std::map<int, std::string> m = {{1, "one"}};
    auto result = Spark::ContainerUtils::MapFindOr(m, 99, std::string("fallback"));
    EXPECT_EQ(result, std::string("fallback"));
}

// ============================================================================
// EraseIf
// ============================================================================

TEST(ContainerUtilsReal_EraseIfVector)
{
    std::vector<int> v = {1, 2, 3, 4, 5, 6};
    auto removed = Spark::ContainerUtils::EraseIf(v, [](int x) { return x % 2 == 0; });
    EXPECT_EQ(removed, 3u);
    EXPECT_EQ(v.size(), 3u);
    EXPECT_TRUE(Spark::ContainerUtils::Contains(v, 1));
    EXPECT_FALSE(Spark::ContainerUtils::Contains(v, 2));
}

TEST(ContainerUtilsReal_EraseIfMap)
{
    std::unordered_map<std::string, int> m = {{"a", 1}, {"b", -1}, {"c", 3}};
    auto removed = Spark::ContainerUtils::EraseIf(m, [](const auto& kv) { return kv.second < 0; });
    EXPECT_EQ(removed, 1u);
    EXPECT_EQ(m.size(), 2u);
}

// ============================================================================
// Contains
// ============================================================================

TEST(ContainerUtilsReal_ContainsVector)
{
    std::vector<int> v = {10, 20, 30};
    EXPECT_TRUE(Spark::ContainerUtils::Contains(v, 20));
    EXPECT_FALSE(Spark::ContainerUtils::Contains(v, 99));
}

TEST(ContainerUtilsReal_ContainsSet)
{
    std::set<std::string> s = {"alpha", "beta"};
    EXPECT_TRUE(Spark::ContainerUtils::Contains(s, std::string("alpha")));
    EXPECT_FALSE(Spark::ContainerUtils::Contains(s, std::string("gamma")));
}

TEST(ContainerUtilsReal_ContainsUnorderedSet)
{
    std::unordered_set<int> us = {5, 10, 15};
    EXPECT_TRUE(Spark::ContainerUtils::Contains(us, 10));
    EXPECT_FALSE(Spark::ContainerUtils::Contains(us, 7));
}

TEST(ContainerUtilsReal_EmptyContainer)
{
    std::vector<int> v;
    EXPECT_FALSE(Spark::ContainerUtils::Contains(v, 1));

    std::unordered_map<int, int> m;
    auto result = Spark::ContainerUtils::MapFind(m, 1);
    EXPECT_FALSE(result.has_value());
}
