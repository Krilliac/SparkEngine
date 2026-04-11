/**
 * @file TestSSAOTemporalFilter.cpp
 * @brief Phase J — CPU reference tests for the SSAO temporal denoiser
 *
 * Exercises the header-only `Spark::Graphics::SSAOTemporalFilter` class
 * directly — no GPU device, no D3D11. The filter ships a full CPU
 * reference implementation that the pipeline's `PostProcessPass::SSAOTemporal`
 * shader mirrors at a per-frame spatial level; these tests pin the CPU
 * oracle's behaviour so future pipeline refactors can't quietly change it.
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Graphics/SSAOTemporal.h"

#include <cmath>
#include <vector>

namespace
{
    constexpr uint32_t kW = 8;
    constexpr uint32_t kH = 8;
    constexpr float kFlatAO = 0.7f;

    std::vector<float> FlatBuffer(float value)
    {
        return std::vector<float>(static_cast<size_t>(kW) * kH, value);
    }

    std::vector<float> ZeroMotion()
    {
        return std::vector<float>(static_cast<size_t>(kW) * kH, 0.0f);
    }
} // namespace

using Spark::Graphics::SSAOTemporalFilter;
using Spark::Graphics::SSAOTemporalSettings;

TEST(SSAOTemporal_DefaultSettings)
{
    SSAOTemporalSettings s;
    EXPECT_TRUE(s.enabled);
    EXPECT_NEAR(s.blendFactor, 0.9f, 1e-6f);
    EXPECT_NEAR(s.motionRejectionScale, 10.0f, 1e-6f);
    EXPECT_NEAR(s.depthRejectionScale, 100.0f, 1e-6f);
    EXPECT_TRUE(s.useVarianceClipping);
    EXPECT_NEAR(s.varianceGamma, 1.0f, 1e-6f);
}

TEST(SSAOTemporal_InitializeAndShutdown)
{
    SSAOTemporalFilter f;
    EXPECT_FALSE(f.IsInitialized());
    f.Initialize(kW, kH);
    EXPECT_TRUE(f.IsInitialized());
    f.Shutdown();
    EXPECT_FALSE(f.IsInitialized());
}

TEST(SSAOTemporal_ResizeResetsHistory)
{
    SSAOTemporalFilter f;
    f.Initialize(kW, kH);
    EXPECT_TRUE(f.IsInitialized());
    // Re-initialise with a different viewport — internal history buffers
    // must grow/shrink to match. IsInitialized stays true.
    f.Resize(16, 12);
    EXPECT_TRUE(f.IsInitialized());
    f.Shutdown();
}

TEST(SSAOTemporal_FirstFrameReturnsCurrent)
{
    SSAOTemporalFilter f;
    f.Initialize(kW, kH);
    auto ao = FlatBuffer(kFlatAO);
    auto mx = ZeroMotion();
    auto my = ZeroMotion();
    auto depth = FlatBuffer(0.5f);
    // First call — no history yet, filter must pass through current values.
    const float* out = f.Apply(ao.data(), mx.data(), my.data(), depth.data(), depth.data());
    EXPECT_TRUE(out != nullptr);
    for (uint32_t i = 0; i < kW * kH; ++i)
    {
        EXPECT_NEAR(out[i], kFlatAO, 1e-4f);
    }
}

TEST(SSAOTemporal_ConstantFieldRemainsConstant)
{
    SSAOTemporalFilter f;
    f.Initialize(kW, kH);
    auto ao = FlatBuffer(kFlatAO);
    auto mx = ZeroMotion();
    auto my = ZeroMotion();
    auto depth = FlatBuffer(0.5f);

    const float* out = nullptr;
    for (int frame = 0; frame < 5; ++frame)
    {
        out = f.Apply(ao.data(), mx.data(), my.data(), depth.data(), depth.data());
    }
    // After 5 frames of a constant field the blended result must still be
    // constant — the variance-clip band collapses onto the mean.
    EXPECT_TRUE(out != nullptr);
    for (uint32_t i = 0; i < kW * kH; ++i)
    {
        EXPECT_NEAR(out[i], kFlatAO, 1e-3f);
    }
}

TEST(SSAOTemporal_NullInputIsSafe)
{
    SSAOTemporalFilter f;
    f.Initialize(kW, kH);
    // Passing a null current buffer is documented to return current (nullptr).
    const float* out = f.Apply(nullptr, nullptr, nullptr, nullptr, nullptr);
    EXPECT_TRUE(out == nullptr);
}

TEST(SSAOTemporal_SettingsAreMutable)
{
    SSAOTemporalFilter f;
    f.Initialize(kW, kH);
    f.GetSettings().blendFactor = 0.25f;
    f.GetSettings().useVarianceClipping = false;
    const auto& s = f.GetSettings();
    EXPECT_NEAR(s.blendFactor, 0.25f, 1e-6f);
    EXPECT_FALSE(s.useVarianceClipping);
}

TEST(SSAOTemporal_StepTowardHistoryOverFrames)
{
    // With full history weight and no motion/depth rejection, a sudden change
    // in current-frame AO should be pulled toward the prior mean across
    // several frames — the filter is a low-pass in time.
    SSAOTemporalFilter f;
    f.Initialize(kW, kH);
    f.GetSettings().blendFactor = 1.0f;          // all-history
    f.GetSettings().useVarianceClipping = false; // disable clamp to isolate time behaviour
    f.GetSettings().motionRejectionScale = 0.0f; // never reject on motion
    f.GetSettings().depthRejectionScale = 0.0f;  // never reject on depth

    auto mx = ZeroMotion();
    auto my = ZeroMotion();
    auto depth = FlatBuffer(0.5f);

    // Seed 3 frames of 1.0 to prime history.
    auto hi = FlatBuffer(1.0f);
    for (int i = 0; i < 3; ++i)
    {
        f.Apply(hi.data(), mx.data(), my.data(), depth.data(), depth.data());
    }
    // Now feed 0.0 — the filter must not immediately collapse to 0.
    auto lo = FlatBuffer(0.0f);
    const float* out = f.Apply(lo.data(), mx.data(), my.data(), depth.data(), depth.data());
    EXPECT_TRUE(out != nullptr);
    // Center pixel should be closer to history (1.0) than current (0.0).
    float center = out[(kH / 2) * kW + (kW / 2)];
    EXPECT_GT(center, 0.5f);
}
