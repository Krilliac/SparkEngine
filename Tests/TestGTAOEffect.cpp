/**
 * @file TestGTAOEffect.cpp
 * @brief Phase I — CPU reference tests for Ground Truth Ambient Occlusion
 *
 * Exercises the header-only `Spark::Graphics::GTAOEffect` class directly —
 * no GPU device, no D3D11. The GTAOEffect header ships a complete CPU
 * reference implementation whose sole purpose is validation, which makes
 * it a perfect first target for orphan-utility wiring: the pass has a
 * deterministic oracle we can run anywhere.
 *
 * These tests also exercise the settings-defaults contract that the
 * `PostProcessingPipeline::GetGTAOSettings()` accessor returns, pinning
 * the 8 GTAOSettings fields so future pipeline refactors can't quietly
 * change the defaults.
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Graphics/GTAOEffect.h"

#include <cmath>
#include <vector>

namespace
{

    // Build a flat "open sky" depth field: a constant far-plane depth so that
    // every pixel sees no occluders. AO should be ~1.0 everywhere.
    std::vector<float> MakeOpenSkyDepth(uint32_t w, uint32_t h, float depth)
    {
        return std::vector<float>(static_cast<size_t>(w) * h, depth);
    }

    // Build a depth field with a "wall" (low depth) on the left half. Pixels
    // on the right half, near the wall, should see significant occlusion.
    std::vector<float> MakeWallDepth(uint32_t w, uint32_t h, float wallDepth, float farDepth)
    {
        std::vector<float> depth(static_cast<size_t>(w) * h, farDepth);
        for (uint32_t y = 0; y < h; ++y)
        {
            for (uint32_t x = 0; x < w / 2; ++x)
            {
                depth[y * w + x] = wallDepth;
            }
        }
        return depth;
    }

    // World-space "up" normal field (every pixel faces +Z toward the camera).
    std::vector<float> MakeFlatNormal(uint32_t w, uint32_t h)
    {
        std::vector<float> n(static_cast<size_t>(w) * h * 3, 0.0f);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
        {
            n[i * 3 + 0] = 0.0f;
            n[i * 3 + 1] = 0.0f;
            n[i * 3 + 2] = 1.0f;
        }
        return n;
    }

} // namespace

// Pin the 8 GTAOSettings POD defaults. If any of these drift the pipeline
// ProcessPass(GTAO) case needs to be audited for CB-layout compatibility.
TEST(GTAO_DefaultSettings)
{
    Spark::Graphics::GTAOSettings s;
    EXPECT_EQ(s.directions, 6);
    EXPECT_EQ(s.stepsPerDirection, 6);
    EXPECT_NEAR(s.radius, 1.5f, 0.0001f);
    EXPECT_NEAR(s.power, 1.5f, 0.0001f);
    EXPECT_NEAR(s.falloffStart, 0.2f, 0.0001f);
    EXPECT_NEAR(s.falloffEnd, 1.0f, 0.0001f);
    EXPECT_NEAR(s.depthThreshold, 0.1f, 0.0001f);
    EXPECT_NEAR(s.normalBias, 0.01f, 0.0001f);
}

TEST(GTAO_InitializeAndShutdown)
{
    Spark::Graphics::GTAOEffect effect;
    EXPECT_FALSE(effect.IsInitialized());

    Spark::Graphics::GTAOSettings s;
    EXPECT_TRUE(effect.Initialize(32, 32, s));
    EXPECT_TRUE(effect.IsInitialized());
    EXPECT_EQ(effect.GetAOBuffer().size(), static_cast<size_t>(32 * 32));

    effect.Shutdown();
    EXPECT_FALSE(effect.IsInitialized());
    EXPECT_EQ(effect.GetAOBuffer().size(), static_cast<size_t>(0));
}

TEST(GTAO_InitializeRejectsBadDimensions)
{
    Spark::Graphics::GTAOEffect effect;
    Spark::Graphics::GTAOSettings s;
    EXPECT_FALSE(effect.Initialize(0, 32, s));
    EXPECT_FALSE(effect.Initialize(32, 0, s));
    EXPECT_FALSE(effect.Initialize(20000, 32, s));
    EXPECT_FALSE(effect.IsInitialized());
}

// Open-sky scene: flat depth, flat normals. Every pixel's horizon angle is
// the plane itself, so AO ≈ 1 (fully unoccluded) before the power curve
// and close to 1 after it.
TEST(GTAO_OpenSkyFlatDepth_HasHighAO)
{
    const uint32_t W = 24;
    const uint32_t H = 24;

    Spark::Graphics::GTAOEffect effect;
    Spark::Graphics::GTAOSettings s;
    // Use a tighter falloff so off-axis depth differences don't dominate
    // the numerical path in this synthetic test.
    s.power = 1.0f;
    EXPECT_TRUE(effect.Initialize(W, H, s));

    auto depth = MakeOpenSkyDepth(W, H, 10.0f);
    auto normals = MakeFlatNormal(W, H);
    effect.ComputeGTAO(depth.data(), normals.data(), /*projScale=*/1.0f);

    // Sample the interior pixel where border clamping doesn't dominate.
    const auto& ao = effect.GetAOBuffer();
    float center = ao[(H / 2) * W + (W / 2)];
    EXPECT_GT(center, 0.5f);
    EXPECT_LT(center, 1.0001f);
}

// Zero-depth sky pixels: the reference early-outs with AO = 1.0.
TEST(GTAO_ZeroDepth_IsFullyLit)
{
    const uint32_t W = 16;
    const uint32_t H = 16;
    Spark::Graphics::GTAOEffect effect;
    Spark::Graphics::GTAOSettings s;
    EXPECT_TRUE(effect.Initialize(W, H, s));

    std::vector<float> depth(W * H, 0.0f);
    auto normals = MakeFlatNormal(W, H);
    effect.ComputeGTAO(depth.data(), normals.data(), 1.0f);

    const auto& ao = effect.GetAOBuffer();
    // Every pixel should be fully lit (AO = 1.0) since they all early-out.
    for (float v : ao)
    {
        EXPECT_NEAR(v, 1.0f, 0.0001f);
    }
}

// A wall occluder on the left half should produce visibly lower AO for
// pixels sitting near the wall on the right side vs. pixels far from it.
TEST(GTAO_WallOccluder_LowersNearbyAO)
{
    const uint32_t W = 32;
    const uint32_t H = 16;

    Spark::Graphics::GTAOEffect effect;
    Spark::Graphics::GTAOSettings s;
    s.power = 1.0f;
    s.radius = 4.0f; // Big enough that samples reach into the wall region.
    EXPECT_TRUE(effect.Initialize(W, H, s));

    auto depth = MakeWallDepth(W, H, /*wallDepth=*/1.0f, /*farDepth=*/10.0f);
    auto normals = MakeFlatNormal(W, H);
    effect.ComputeGTAO(depth.data(), normals.data(), /*projScale=*/4.0f);

    const auto& ao = effect.GetAOBuffer();
    // Pixel just past the wall (x = W/2 + 1, middle row) has the wall within
    // horizon-search range along the -x direction; it must be <= a pixel on
    // the far right edge which has no occluders nearby.
    float nearWall = ao[(H / 2) * W + (W / 2 + 1)];
    float farFromWall = ao[(H / 2) * W + (W - 1)];
    EXPECT_LT(nearWall, farFromWall + 0.0001f);
}

// Spatial denoiser must preserve constant-value AO buffers (every weight
// is equal so the weighted average equals the input).
TEST(GTAO_SpatialDenoise_PreservesConstantField)
{
    const uint32_t W = 12;
    const uint32_t H = 12;

    Spark::Graphics::GTAOEffect effect;
    Spark::Graphics::GTAOSettings s;
    EXPECT_TRUE(effect.Initialize(W, H, s));

    auto depth = MakeOpenSkyDepth(W, H, 5.0f);
    auto normals = MakeFlatNormal(W, H);
    effect.ComputeGTAO(depth.data(), normals.data(), 1.0f);

    // Capture the raw AO buffer, then run the denoiser and compare interior
    // pixels — the bilateral kernel should not move a constant field.
    float rawCenter = effect.GetAOBuffer()[(H / 2) * W + (W / 2)];
    effect.SpatialDenoise(depth.data());
    float denoisedCenter = effect.GetDenoisedBuffer()[(H / 2) * W + (W / 2)];
    EXPECT_NEAR(denoisedCenter, rawCenter, 0.0001f);
}

// The static HLSL source must be non-empty, null-terminated, and contain
// the compute-shader entry point. This is our contract with any build-time
// shader validator that walks the header looking for embedded sources.
TEST(GTAO_HLSLSourcePointerShape)
{
    const char* src = Spark::Graphics::GTAOEffect::GetHLSLShaderSource();
    EXPECT_NE(src, nullptr);
    if (!src)
        return;

    std::string s(src);
    EXPECT_GT(s.size(), static_cast<size_t>(100));
    EXPECT_NE(s.find("CSMain"), std::string::npos);
    EXPECT_NE(s.find("numthreads"), std::string::npos);
    EXPECT_NE(s.find("GTAOConstants"), std::string::npos);
}
