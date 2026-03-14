// TestHash.cpp - Tests for Spark::Hash utilities (FNV-1a, CombineHash, literals)
#include "TestFramework.h"
#include "Utils/Hash.h"

using namespace Spark::HashLiterals;

// ============================================================================
// FNV1a32 basic correctness
// ============================================================================

TEST(Hash_FNV1a32_EmptyString)
{
    // Empty string should equal the FNV offset basis
    constexpr uint32_t emptyHash = Spark::FNV1a32("");
    EXPECT_EQ(emptyHash, 2166136261u);
}

TEST(Hash_FNV1a32_KnownValue)
{
    // Known FNV-1a 32-bit hash of "hello"
    constexpr uint32_t h = Spark::FNV1a32("hello");
    EXPECT_EQ(h, 1335831723u);
}

TEST(Hash_FNV1a32_Deterministic)
{
    uint32_t a = Spark::FNV1a32("SparkEngine");
    uint32_t b = Spark::FNV1a32("SparkEngine");
    EXPECT_EQ(a, b);
}

TEST(Hash_FNV1a32_Distinct)
{
    uint32_t a = Spark::FNV1a32("player_spawn");
    uint32_t b = Spark::FNV1a32("enemy_spawn");
    EXPECT_NE(a, b);
}

TEST(Hash_FNV1a32_StringViewOverload)
{
    std::string_view sv = "hello";
    uint32_t fromSv = Spark::FNV1a32(sv);
    uint32_t fromCStr = Spark::FNV1a32("hello");
    EXPECT_EQ(fromSv, fromCStr);
}

// ============================================================================
// FNV1a64 basic correctness
// ============================================================================

TEST(Hash_FNV1a64_EmptyString)
{
    constexpr uint64_t emptyHash = Spark::FNV1a64("");
    EXPECT_EQ(emptyHash, 14695981039346656037ull);
}

TEST(Hash_FNV1a64_KnownValue)
{
    // Known FNV-1a 64-bit hash of "hello"
    constexpr uint64_t h = Spark::FNV1a64("hello");
    EXPECT_EQ(h, 11831194018420276491ull);
}

TEST(Hash_FNV1a64_Deterministic)
{
    uint64_t a = Spark::FNV1a64("SparkEngine");
    uint64_t b = Spark::FNV1a64("SparkEngine");
    EXPECT_EQ(a, b);
}

TEST(Hash_FNV1a64_Distinct)
{
    EXPECT_NE(Spark::FNV1a64("abc"), Spark::FNV1a64("ABC"));
    EXPECT_NE(Spark::FNV1a64("foo"), Spark::FNV1a64("bar"));
}

TEST(Hash_FNV1a64_StringViewAndCStr_Match)
{
    const char* cstr = "physics_update";
    std::string_view sv{cstr};
    EXPECT_EQ(Spark::FNV1a64(cstr), Spark::FNV1a64(sv));
}

// ============================================================================
// Compile-time evaluation
// ============================================================================

TEST(Hash_Constexpr_64)
{
    constexpr uint64_t h = Spark::FNV1a64("constexpr_test");
    EXPECT_NE(h, 0u);
    // Must be the same every time (compile-time determined)
    EXPECT_EQ(h, Spark::FNV1a64("constexpr_test"));
}

// ============================================================================
// User-defined literals
// ============================================================================

TEST(Hash_Literal_hash64)
{
    constexpr uint64_t lit = "player_spawn"_hash64;
    uint64_t rt = Spark::FNV1a64("player_spawn");
    EXPECT_EQ(lit, rt);
}

TEST(Hash_Literal_hash32)
{
    constexpr uint32_t lit = "player_spawn"_hash32;
    uint32_t rt = Spark::FNV1a32("player_spawn");
    EXPECT_EQ(lit, rt);
}

TEST(Hash_Literal_SwitchDispatch)
{
    // Demonstrates switch-on-string pattern
    const char* cmd = "reload";
    int result = 0;
    switch (Spark::FNV1a64(cmd))
    {
    case "quit"_hash64:
        result = 1;
        break;
    case "reload"_hash64:
        result = 2;
        break;
    case "save"_hash64:
        result = 3;
        break;
    default:
        result = -1;
        break;
    }
    EXPECT_EQ(result, 2);
}

// ============================================================================
// CombineHash
// ============================================================================

TEST(Hash_CombineHash_Modifies_Seed)
{
    size_t seed = 0;
    Spark::CombineHash(seed, 12345u);
    EXPECT_NE(seed, 0u);
}

TEST(Hash_CombineHash_OrderMatters)
{
    size_t a = 0;
    Spark::CombineHash(a, 1u);
    Spark::CombineHash(a, 2u);

    size_t b = 0;
    Spark::CombineHash(b, 2u);
    Spark::CombineHash(b, 1u);

    EXPECT_NE(a, b);
}

TEST(Hash_HashCombine_ReturningVariant)
{
    size_t combined = Spark::HashCombine(static_cast<size_t>(0), static_cast<size_t>(42));
    EXPECT_NE(combined, 0u);
    EXPECT_NE(combined, static_cast<size_t>(42));
}

TEST(Hash_CombineHash_Deterministic)
{
    size_t a = 0, b = 0;
    Spark::CombineHash(a, 99u);
    Spark::CombineHash(b, 99u);
    EXPECT_EQ(a, b);
}
