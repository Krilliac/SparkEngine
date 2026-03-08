// TestStringUtils.cpp - Tests for string manipulation utilities
// Uses the actual StringUtils.h header

#include "TestFramework.h"
#include "Utils/StringUtils.h"

using namespace Spark::StringUtils;

// =============================================================================
// Tests
// =============================================================================

TEST(StringUtils_ToLower)
{
    EXPECT_EQ(ToLower("Hello World"), std::string("hello world"));
    EXPECT_EQ(ToLower("ALLCAPS"), std::string("allcaps"));
    EXPECT_EQ(ToLower("already"), std::string("already"));
    EXPECT_EQ(ToLower(""), std::string(""));
}

TEST(StringUtils_ToUpper)
{
    EXPECT_EQ(ToUpper("Hello World"), std::string("HELLO WORLD"));
    EXPECT_EQ(ToUpper("lowercase"), std::string("LOWERCASE"));
    EXPECT_EQ(ToUpper("ALREADY"), std::string("ALREADY"));
}

TEST(StringUtils_Trim)
{
    EXPECT_EQ(Trim("  hello  "), std::string("hello"));
    EXPECT_EQ(Trim("\t\nhello\r\n"), std::string("hello"));
    EXPECT_EQ(Trim("nospace"), std::string("nospace"));
    EXPECT_EQ(Trim("   "), std::string(""));
    EXPECT_EQ(Trim(""), std::string(""));
}

TEST(StringUtils_TrimLeft)
{
    EXPECT_EQ(TrimLeft("  hello  "), std::string("hello  "));
    EXPECT_EQ(TrimLeft("hello"), std::string("hello"));
}

TEST(StringUtils_TrimRight)
{
    EXPECT_EQ(TrimRight("  hello  "), std::string("  hello"));
    EXPECT_EQ(TrimRight("hello"), std::string("hello"));
}

TEST(StringUtils_StartsWith)
{
    EXPECT_TRUE(StartsWith("hello world", "hello"));
    EXPECT_TRUE(StartsWith("hello", "hello"));
    EXPECT_FALSE(StartsWith("hello", "world"));
    EXPECT_FALSE(StartsWith("hi", "hello"));
    EXPECT_TRUE(StartsWith("anything", ""));
}

TEST(StringUtils_EndsWith)
{
    EXPECT_TRUE(EndsWith("hello world", "world"));
    EXPECT_TRUE(EndsWith("hello", "hello"));
    EXPECT_FALSE(EndsWith("hello", "world"));
    EXPECT_FALSE(EndsWith("hi", "hello"));
    EXPECT_TRUE(EndsWith("anything", ""));
}

TEST(StringUtils_Contains)
{
    EXPECT_TRUE(Contains("hello world", "world"));
    EXPECT_TRUE(Contains("hello world", "lo wo"));
    EXPECT_FALSE(Contains("hello", "xyz"));
    EXPECT_TRUE(Contains("hello", ""));
}

TEST(StringUtils_SplitChar)
{
    auto parts = Split("a,b,c", ',');
    EXPECT_EQ((int)parts.size(), 3);
    EXPECT_EQ(parts[0], std::string("a"));
    EXPECT_EQ(parts[1], std::string("b"));
    EXPECT_EQ(parts[2], std::string("c"));
}

TEST(StringUtils_SplitString)
{
    auto parts = Split("one::two::three", "::");
    EXPECT_EQ((int)parts.size(), 3);
    EXPECT_EQ(parts[0], std::string("one"));
    EXPECT_EQ(parts[1], std::string("two"));
    EXPECT_EQ(parts[2], std::string("three"));
}

TEST(StringUtils_SplitNoDelimiter)
{
    auto parts = Split("hello", ',');
    EXPECT_EQ((int)parts.size(), 1);
    EXPECT_EQ(parts[0], std::string("hello"));
}

TEST(StringUtils_Join)
{
    std::vector<std::string> parts = {"a", "b", "c"};
    EXPECT_EQ(Join(parts, ", "), std::string("a, b, c"));
    EXPECT_EQ(Join(parts, ""), std::string("abc"));
    EXPECT_EQ(Join({}, ", "), std::string(""));
}

TEST(StringUtils_Replace)
{
    EXPECT_EQ(Replace("hello world", "world", "there"), std::string("hello there"));
    EXPECT_EQ(Replace("hello", "xyz", "abc"), std::string("hello"));
    EXPECT_EQ(Replace("aaa", "a", "b"), std::string("baa")); // Only first
}

TEST(StringUtils_ReplaceAll)
{
    EXPECT_EQ(ReplaceAll("aaa", "a", "b"), std::string("bbb"));
    EXPECT_EQ(ReplaceAll("hello world world", "world", "there"), std::string("hello there there"));
    EXPECT_EQ(ReplaceAll("hello", "", "x"), std::string("hello")); // Empty from
}

TEST(StringUtils_ToString)
{
    EXPECT_EQ(ToString(42), std::string("42"));
    EXPECT_EQ(ToString(true), std::string("true"));
    EXPECT_EQ(ToString(false), std::string("false"));
}

TEST(StringUtils_ParseInt)
{
    auto v = ParseInt("42");
    EXPECT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), 42);

    v = ParseInt("-10");
    EXPECT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), -10);

    EXPECT_FALSE(ParseInt("abc").has_value());
    EXPECT_FALSE(ParseInt("").has_value());
    EXPECT_FALSE(ParseInt("12abc").has_value());
}

TEST(StringUtils_ParseFloat)
{
    auto v = ParseFloat("3.14");
    EXPECT_TRUE(v.has_value());
    EXPECT_NEAR(v.value(), 3.14f, 0.01f);

    EXPECT_FALSE(ParseFloat("abc").has_value());
    EXPECT_FALSE(ParseFloat("").has_value());
}

TEST(StringUtils_ParseBool)
{
    EXPECT_TRUE(ParseBool("true").value());
    EXPECT_TRUE(ParseBool("1").value());
    EXPECT_TRUE(ParseBool("yes").value());
    EXPECT_TRUE(ParseBool("ON").value());

    EXPECT_FALSE(ParseBool("false").value());
    EXPECT_FALSE(ParseBool("0").value());
    EXPECT_FALSE(ParseBool("no").value());
    EXPECT_FALSE(ParseBool("OFF").value());

    EXPECT_FALSE(ParseBool("maybe").has_value());
}

TEST(StringUtils_Format)
{
    EXPECT_EQ(Format("Hello %s", "world"), std::string("Hello world"));
    EXPECT_EQ(Format("FPS: %d", 60), std::string("FPS: 60"));
    EXPECT_EQ(Format("%.2f", 3.14159f), std::string("3.14"));
}
