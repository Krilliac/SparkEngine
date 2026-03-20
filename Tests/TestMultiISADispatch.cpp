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
