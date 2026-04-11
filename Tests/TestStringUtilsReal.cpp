/**
 * @file TestStringUtilsReal.cpp
 * @brief Real-class tests for Spark::StringUtils namespace
 */

#include "TestFramework.h"
#include "Utils/StringUtils.h"

TEST(StringUtilsReal_ToLower)
{
    EXPECT_EQ(Spark::StringUtils::ToLower("Hello World"), std::string("hello world"));
    EXPECT_EQ(Spark::StringUtils::ToLower("UPPERCASE"), std::string("uppercase"));
    EXPECT_EQ(Spark::StringUtils::ToLower("mixed"), std::string("mixed"));
}

TEST(StringUtilsReal_ToUpper)
{
    EXPECT_EQ(Spark::StringUtils::ToUpper("hello"), std::string("HELLO"));
    EXPECT_EQ(Spark::StringUtils::ToUpper("Mixed Case"), std::string("MIXED CASE"));
}

TEST(StringUtilsReal_Trim)
{
    EXPECT_EQ(Spark::StringUtils::Trim("  hello  "), std::string("hello"));
    EXPECT_EQ(Spark::StringUtils::Trim("\t\nhello\r\n"), std::string("hello"));
    EXPECT_EQ(Spark::StringUtils::Trim("no-whitespace"), std::string("no-whitespace"));
    EXPECT_EQ(Spark::StringUtils::Trim(""), std::string(""));
}

TEST(StringUtilsReal_TrimLeftRight)
{
    EXPECT_EQ(Spark::StringUtils::TrimLeft("  hello  "), std::string("hello  "));
    EXPECT_EQ(Spark::StringUtils::TrimRight("  hello  "), std::string("  hello"));
}

TEST(StringUtilsReal_StartsWith)
{
    EXPECT_TRUE(Spark::StringUtils::StartsWith("hello world", "hello"));
    EXPECT_FALSE(Spark::StringUtils::StartsWith("hello world", "world"));
    EXPECT_TRUE(Spark::StringUtils::StartsWith("abc", ""));
}

TEST(StringUtilsReal_EndsWith)
{
    EXPECT_TRUE(Spark::StringUtils::EndsWith("hello world", "world"));
    EXPECT_FALSE(Spark::StringUtils::EndsWith("hello world", "hello"));
    EXPECT_TRUE(Spark::StringUtils::EndsWith("abc", ""));
}

TEST(StringUtilsReal_SplitChar)
{
    auto parts = Spark::StringUtils::Split("a,b,c,d", ',');
    EXPECT_EQ(parts.size(), static_cast<size_t>(4));
    EXPECT_EQ(parts[0], std::string("a"));
    EXPECT_EQ(parts[1], std::string("b"));
    EXPECT_EQ(parts[2], std::string("c"));
    EXPECT_EQ(parts[3], std::string("d"));
}

TEST(StringUtilsReal_SplitDelimiterString)
{
    auto parts = Spark::StringUtils::Split("a::b::c", std::string("::"));
    EXPECT_EQ(parts.size(), static_cast<size_t>(3));
    EXPECT_EQ(parts[0], std::string("a"));
    EXPECT_EQ(parts[1], std::string("b"));
    EXPECT_EQ(parts[2], std::string("c"));
}

TEST(StringUtilsReal_Replace)
{
    EXPECT_EQ(Spark::StringUtils::Replace("hello world", "world", "universe"), std::string("hello universe"));
    // Note: Replace only performs one substitution pass, not recursive
    // repeat-until-stable. "aaa" -> replace first "a" with "bb" ->
    // "bbaa", and the remaining "aa" is left alone.
    EXPECT_EQ(Spark::StringUtils::Replace("aaa", "a", "bb"), std::string("bbaa"));
    EXPECT_EQ(Spark::StringUtils::Replace("no match", "xyz", "abc"), std::string("no match"));
}
