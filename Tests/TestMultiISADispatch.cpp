/**
 * @file TestMultiISADispatch.cpp
 * @brief Tests for multi-ISA CPU function dispatch
 */

#include "TestFramework.h"
#include <cstdint>

namespace
{

    enum class TestISALevel : uint8_t
    {
        SSE2 = 0,
        SSE4 = 1,
        AVX = 2,
        AVX2 = 3,
        Count
    };

    template <typename FuncT> FuncT SelectBestISA(TestISALevel detected, const FuncT (&variants)[4])
    {
        for (int level = static_cast<int>(detected); level >= 0; --level)
        {
            if (variants[level])
                return variants[level];
        }
        return variants[0];
    }

} // anonymous namespace

TEST(MultiISA_SelectBest)
{
    using FuncT = int (*)();
    auto sse2_fn = []() -> int { return 2; };
    auto avx2_fn = []() -> int { return 8; };

    FuncT variants[4] = {sse2_fn, nullptr, nullptr, avx2_fn};

    auto best = SelectBestISA(TestISALevel::AVX2, variants);
    EXPECT_EQ(best(), 8);

    auto fallback = SelectBestISA(TestISALevel::AVX, variants);
    EXPECT_EQ(fallback(), 2); // Falls back to SSE2

    auto baseline = SelectBestISA(TestISALevel::SSE2, variants);
    EXPECT_EQ(baseline(), 2);
}

TEST(MultiISA_FallbackChain)
{
    using FuncT = int (*)();
    auto sse2_fn = []() -> int { return 1; };
    auto sse4_fn = []() -> int { return 4; };

    FuncT variants[4] = {sse2_fn, sse4_fn, nullptr, nullptr};

    auto avx2Select = SelectBestISA(TestISALevel::AVX2, variants);
    EXPECT_EQ(avx2Select(), 4); // Falls back to SSE4

    auto sse4Select = SelectBestISA(TestISALevel::SSE4, variants);
    EXPECT_EQ(sse4Select(), 4);
}

TEST(MultiISA_AllPopulated)
{
    using FuncT = int (*)();
    auto sse2_fn = []() -> int { return 2; };
    auto sse4_fn = []() -> int { return 4; };
    auto avx_fn = []() -> int { return 6; };
    auto avx2_fn = []() -> int { return 8; };

    FuncT variants[4] = {sse2_fn, sse4_fn, avx_fn, avx2_fn};

    EXPECT_EQ(SelectBestISA(TestISALevel::SSE2, variants)(), 2);
    EXPECT_EQ(SelectBestISA(TestISALevel::SSE4, variants)(), 4);
    EXPECT_EQ(SelectBestISA(TestISALevel::AVX, variants)(), 6);
    EXPECT_EQ(SelectBestISA(TestISALevel::AVX2, variants)(), 8);
}

TEST(MultiISA_OnlyBaseline)
{
    using FuncT = int (*)();
    auto sse2_fn = []() -> int { return 2; };

    FuncT variants[4] = {sse2_fn, nullptr, nullptr, nullptr};

    // All levels should fall back to SSE2
    EXPECT_EQ(SelectBestISA(TestISALevel::SSE2, variants)(), 2);
    EXPECT_EQ(SelectBestISA(TestISALevel::SSE4, variants)(), 2);
    EXPECT_EQ(SelectBestISA(TestISALevel::AVX, variants)(), 2);
    EXPECT_EQ(SelectBestISA(TestISALevel::AVX2, variants)(), 2);
}

TEST(MultiISA_SkipLevelsFallbackCorrectly)
{
    using FuncT = int (*)();
    auto sse2_fn = []() -> int { return 2; };
    auto avx_fn = []() -> int { return 6; };

    // SSE4 slot empty, AVX2 slot empty
    FuncT variants[4] = {sse2_fn, nullptr, avx_fn, nullptr};

    EXPECT_EQ(SelectBestISA(TestISALevel::AVX2, variants)(), 6); // Falls to AVX
    EXPECT_EQ(SelectBestISA(TestISALevel::AVX, variants)(), 6);  // Exact match
    EXPECT_EQ(SelectBestISA(TestISALevel::SSE4, variants)(), 2); // Falls to SSE2
    EXPECT_EQ(SelectBestISA(TestISALevel::SSE2, variants)(), 2); // Exact match
}

TEST(MultiISA_DifferentFunctionSignatures)
{
    using FloatFunc = float (*)();
    auto sse2_fn = []() -> float { return 1.0f; };
    auto avx2_fn = []() -> float { return 4.0f; };

    FloatFunc variants[4] = {sse2_fn, nullptr, nullptr, avx2_fn};

    EXPECT_NEAR(SelectBestISA(TestISALevel::AVX2, variants)(), 4.0f, 0.001f);
    EXPECT_NEAR(SelectBestISA(TestISALevel::SSE4, variants)(), 1.0f, 0.001f);
}

TEST(MultiISA_HighestAvailableSelected)
{
    using FuncT = int (*)();
    auto sse2_fn = []() -> int { return 2; };
    auto sse4_fn = []() -> int { return 4; };
    auto avx2_fn = []() -> int { return 8; };

    FuncT variants[4] = {sse2_fn, sse4_fn, nullptr, avx2_fn};

    // When AVX is detected but no AVX variant, should pick SSE4
    EXPECT_EQ(SelectBestISA(TestISALevel::AVX, variants)(), 4);
    // When AVX2 is detected, should pick AVX2
    EXPECT_EQ(SelectBestISA(TestISALevel::AVX2, variants)(), 8);
}
