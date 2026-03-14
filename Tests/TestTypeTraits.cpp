// TestTypeTraits.cpp - Tests for Spark::TypeTraits concepts and utilities
#include "TestFramework.h"
#include "Utils/TypeTraits.h"

#include <string>
#include <string_view>

// ============================================================================
// Arithmetic concept
// ============================================================================

TEST(TypeTraits_ArithmeticInt)
{
    EXPECT_TRUE((Spark::Arithmetic<int>));
    EXPECT_TRUE((Spark::Arithmetic<float>));
    EXPECT_TRUE((Spark::Arithmetic<double>));
    EXPECT_TRUE((Spark::Arithmetic<unsigned long long>));
}

TEST(TypeTraits_ArithmeticNotEnum)
{
    enum class Color
    {
        Red
    };
    EXPECT_FALSE((Spark::Arithmetic<Color>));
    EXPECT_FALSE((Spark::Arithmetic<std::string>));
}

// ============================================================================
// Enum concept
// ============================================================================

TEST(TypeTraits_EnumConcept)
{
    enum class Direction
    {
        North,
        South
    };
    enum OldEnum
    {
        A,
        B
    };
    EXPECT_TRUE((Spark::Enum<Direction>));
    EXPECT_TRUE((Spark::Enum<OldEnum>));
    EXPECT_FALSE((Spark::Enum<int>));
}

// ============================================================================
// StringLike concept
// ============================================================================

TEST(TypeTraits_StringLikeConcept)
{
    EXPECT_TRUE((Spark::StringLike<std::string>));
    EXPECT_TRUE((Spark::StringLike<std::string_view>));
    EXPECT_TRUE((Spark::StringLike<const char*>));
    EXPECT_FALSE((Spark::StringLike<int>));
    EXPECT_FALSE((Spark::StringLike<float>));
}

// ============================================================================
// TriviallyCopyable concept
// ============================================================================

TEST(TypeTraits_TriviallyCopyable)
{
    EXPECT_TRUE((Spark::TriviallyCopyable<int>));
    EXPECT_TRUE((Spark::TriviallyCopyable<float>));
    struct Plain
    {
        int x;
        float y;
    };
    EXPECT_TRUE((Spark::TriviallyCopyable<Plain>));
    EXPECT_FALSE((Spark::TriviallyCopyable<std::string>));
}

// ============================================================================
// TypeList
// ============================================================================

TEST(TypeTraits_TypeListSize)
{
    using Empty = Spark::TypeList<>;
    using One = Spark::TypeList<int>;
    using Three = Spark::TypeList<int, float, double>;

    EXPECT_EQ(Empty::size, 0u);
    EXPECT_EQ(One::size, 1u);
    EXPECT_EQ(Three::size, 3u);
}

// ============================================================================
// TypeIndex
// ============================================================================

TEST(TypeTraits_TypeIndex)
{
    using List = Spark::TypeList<int, float, double>;
    EXPECT_EQ((Spark::TypeIndex<int, List>::value), 0u);
    EXPECT_EQ((Spark::TypeIndex<float, List>::value), 1u);
    EXPECT_EQ((Spark::TypeIndex<double, List>::value), 2u);
}

// ============================================================================
// TypeAt
// ============================================================================

TEST(TypeTraits_TypeAt)
{
    using List = Spark::TypeList<int, float, double>;
    EXPECT_TRUE((std::is_same_v<Spark::TypeAt<0, List>::type, int>));
    EXPECT_TRUE((std::is_same_v<Spark::TypeAt<1, List>::type, float>));
    EXPECT_TRUE((std::is_same_v<Spark::TypeAt<2, List>::type, double>));
}

// ============================================================================
// AlwaysFalse
// ============================================================================

TEST(TypeTraits_AlwaysFalse)
{
    EXPECT_FALSE((Spark::AlwaysFalse<int>));
    EXPECT_FALSE((Spark::AlwaysFalse<std::string>));
    EXPECT_FALSE((Spark::AlwaysFalse<void>));
}

// ============================================================================
// RemoveCVRef
// ============================================================================

TEST(TypeTraits_RemoveCVRef)
{
    EXPECT_TRUE((std::is_same_v<Spark::RemoveCVRef<const int&>, int>));
    EXPECT_TRUE((std::is_same_v<Spark::RemoveCVRef<volatile double&&>, double>));
    EXPECT_TRUE((std::is_same_v<Spark::RemoveCVRef<int>, int>));
}

// ============================================================================
// IsOneOf
// ============================================================================

TEST(TypeTraits_IsOneOf)
{
    EXPECT_TRUE((Spark::IsOneOf<int, float, int, double>));
    EXPECT_TRUE((Spark::IsOneOf<float, float>));
    EXPECT_FALSE((Spark::IsOneOf<int, float, double>));
    EXPECT_FALSE((Spark::IsOneOf<int>));
}
