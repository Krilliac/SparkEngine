/**
 * @file TestHashReal.cpp
 * @brief Real-class tests for Spark::FNV1a32/64 + CombineHash
 */

#include "TestFramework.h"
#include "Utils/Hash.h"

TEST(HashReal_FNV1a32ConstStringDeterministic)
{
    // Same string should produce the same hash.
    const uint32_t h1 = Spark::FNV1a32("hello");
    const uint32_t h2 = Spark::FNV1a32("hello");
    EXPECT_EQ(h1, h2);
}

TEST(HashReal_FNV1a32DifferentStringsDifferentHash)
{
    const uint32_t h1 = Spark::FNV1a32("hello");
    const uint32_t h2 = Spark::FNV1a32("world");
    EXPECT_TRUE(h1 != h2);
}

TEST(HashReal_FNV1a64ConstStringDeterministic)
{
    const uint64_t h1 = Spark::FNV1a64("hello");
    const uint64_t h2 = Spark::FNV1a64("hello");
    EXPECT_EQ(h1, h2);
}

TEST(HashReal_FNV1a64EmptyString)
{
    // Empty string should hash to FNV offset basis (non-zero).
    const uint64_t h = Spark::FNV1a64("");
    EXPECT_TRUE(h != 0);
}

TEST(HashReal_FNV1aStringViewMatchesConstChar)
{
    const char* cstr = "test_string";
    std::string_view sv{cstr};
    EXPECT_EQ(Spark::FNV1a32(cstr), Spark::FNV1a32(sv));
    EXPECT_EQ(Spark::FNV1a64(cstr), Spark::FNV1a64(sv));
}

TEST(HashReal_FNV1aRawDataMatchesString)
{
    const char* cstr = "abc";
    EXPECT_EQ(Spark::FNV1a32(cstr, 3), Spark::FNV1a32(cstr));
}

TEST(HashReal_CombineHash)
{
    size_t seed = 0;
    Spark::CombineHash(seed, 1);
    const size_t after1 = seed;
    Spark::CombineHash(seed, 2);
    const size_t after2 = seed;
    // CombineHash should change the seed each time.
    EXPECT_TRUE(after1 != static_cast<size_t>(0));
    EXPECT_TRUE(after2 != after1);
}

TEST(HashReal_CombineHashOrderMatters)
{
    size_t seedA = 0;
    size_t seedB = 0;
    Spark::CombineHash(seedA, 1);
    Spark::CombineHash(seedA, 2);
    Spark::CombineHash(seedB, 2);
    Spark::CombineHash(seedB, 1);
    // Different order produces different combined hash.
    EXPECT_TRUE(seedA != seedB);
}

TEST(HashReal_FNV1aCompileTimeConstexpr)
{
    // Verify constexpr evaluation works.
    constexpr uint32_t h = Spark::FNV1a32("compile_time");
    EXPECT_TRUE(h != 0);
}
