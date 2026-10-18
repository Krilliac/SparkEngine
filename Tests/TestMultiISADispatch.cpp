/**
 * @file TestMultiISADispatch.cpp
 * @brief Tests for multi-ISA CPU function dispatch and the stable-v1 CPU floor check
 */

#include "TestFramework.h"
#include "Utils/MultiISA.h"
#include <cstdint>
#include <string>

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

// ---------------------------------------------------------------------------
// BLD-100 / OD-04: runtime CPU feature detection and the SSE4.2 + POPCNT floor
// (Utils/MultiISA.h, called first thing by the engine and editor entry points).
// ---------------------------------------------------------------------------

namespace
{
    Spark::CpuFeatures FloorOnlyFeatures()
    {
        Spark::CpuFeatures features;
        features.isX86 = true;
        features.sse2 = true;
        features.sse3 = true;
        features.ssse3 = true;
        features.sse41 = true;
        features.sse42 = true;
        features.popcnt = true;
        return features;
    }
} // anonymous namespace

TEST(MultiISA_CpuFloor_FloorFeaturesAccepted)
{
    EXPECT_TRUE(Spark::DescribeStableCpuFloorFailure(FloorOnlyFeatures()).empty());
}

TEST(MultiISA_CpuFloor_MissingFeaturesNamed)
{
    Spark::CpuFeatures features = FloorOnlyFeatures();
    features.sse42 = false;
    features.popcnt = false;
    // Above-floor features must not rescue a CPU that misses the floor.
    features.avx = true;
    features.avx2 = true;
    const std::string message = Spark::DescribeStableCpuFloorFailure(features);
    EXPECT_FALSE(message.empty());
    EXPECT_NE(message.find("Missing: SSE4.2, POPCNT"), std::string::npos);
    EXPECT_EQ(message.find("SSE4.1,"), std::string::npos);
}

TEST(MultiISA_CpuFloor_Sse41OnlyCpuRejected)
{
    // A Core 2 (Penryn) class CPU: SSE4.1 but no SSE4.2 or POPCNT.
    Spark::CpuFeatures features = FloorOnlyFeatures();
    features.sse42 = false;
    features.popcnt = false;
    EXPECT_FALSE(Spark::DescribeStableCpuFloorFailure(features).empty());

    features = FloorOnlyFeatures();
    features.popcnt = false;
    EXPECT_NE(Spark::DescribeStableCpuFloorFailure(features).find("Missing: POPCNT"), std::string::npos);
}

TEST(MultiISA_CpuFloor_NonX86NotApplicable)
{
    const Spark::CpuFeatures features; // isX86 == false, every feature false
    EXPECT_TRUE(Spark::DescribeStableCpuFloorFailure(features).empty());
}

TEST(MultiISA_CpuFloor_HostDetectionConsistent)
{
    const Spark::CpuFeatures features = Spark::DetectCpuFeatures();
#if defined(__x86_64__) || defined(_M_X64)
    // This test binary is itself built for the floor and is running, so the
    // host must report it; a detection bug here would lock every user out.
    EXPECT_TRUE(features.isX86);
    EXPECT_TRUE(features.sse2);
    EXPECT_TRUE(features.sse42);
    EXPECT_TRUE(features.popcnt);
    EXPECT_TRUE(Spark::DescribeStableCpuFloorFailure(features).empty());
#endif
    // OS-enabled YMM state gates every VEX feature.
    if (features.avx2 || features.fma || features.f16c)
    {
        EXPECT_TRUE(features.avx);
    }
}

TEST(MultiISA_CpuFloor_DispatchLevelMatchesRuntimeFeatures)
{
    const Spark::CpuFeatures features = Spark::DetectCpuFeatures();
    auto& dispatch = Spark::MultiISADispatch::GetInstance();
    dispatch.Initialize();
    const Spark::ISALevel level = dispatch.GetDetectedLevel();
    if (level == Spark::ISALevel::AVX2)
    {
        EXPECT_TRUE(features.avx2 && features.fma);
    }
    if (level == Spark::ISALevel::AVX)
    {
        EXPECT_TRUE(features.avx);
        EXPECT_FALSE(features.avx2 && features.fma);
    }
    if (features.avx2 && features.fma)
    {
        EXPECT_EQ(static_cast<int>(level), static_cast<int>(Spark::ISALevel::AVX2));
    }
#if defined(__x86_64__) || defined(_M_X64)
    EXPECT_TRUE(static_cast<int>(level) >= static_cast<int>(Spark::ISALevel::SSE4));
#endif
}

TEST(MultiISA_CpuFloor_SelectLevelReportsKernelNotCapability)
{
    using FuncT = int (*)();
    auto baseline = []() -> int { return 2; };
    auto& dispatch = Spark::MultiISADispatch::GetInstance();
    dispatch.Initialize();

    // Only the baseline is compiled in (a floor build's shape): whatever the CPU
    // supports, the kernel that runs -- and the level reported for it -- is SSE2.
    const FuncT baselineOnly[4] = {baseline, nullptr, nullptr, nullptr};
    EXPECT_EQ(static_cast<int>(dispatch.SelectLevel(baselineOnly)), static_cast<int>(Spark::ISALevel::SSE2));
    EXPECT_EQ(dispatch.Select(baselineOnly)(), 2);

    // With every variant populated the selected level is exactly the detected one.
    auto sse4 = []() -> int { return 4; };
    auto avx = []() -> int { return 6; };
    auto avx2 = []() -> int { return 8; };
    const FuncT all[4] = {baseline, sse4, avx, avx2};
    EXPECT_EQ(static_cast<int>(dispatch.SelectLevel(all)), static_cast<int>(dispatch.GetDetectedLevel()));
    EXPECT_EQ(dispatch.Select(all)(), all[static_cast<size_t>(dispatch.GetDetectedLevel())]());
}
