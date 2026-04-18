/**
 * @file TestDenoiserInterface.cpp
 * @brief Phase Q — CPU reference tests for SoftwareDenoiser / IDenoiser
 *
 * Exercises `Spark::Graphics::SoftwareDenoiser` (the joint-bilateral
 * fallback) and the `IDenoiser` contract it implements. The denoiser
 * is pure CPU so every test runs on every platform's CI.
 *
 * Phase Q also wires the denoiser as a per-instance `unique_ptr`
 * member of `Spark::Graphics::GraphicsEngine`, default-constructed
 * as a SoftwareDenoiser. These tests pin the backend's public
 * contract so any future OIDN / OptiX swap-in preserves behaviour.
 */

#include "TestFramework.h"

#include "Graphics/DenoiserInterface.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using Spark::Graphics::DenoiserBackend;
using Spark::Graphics::DenoiserBuffer;
using Spark::Graphics::DenoiserQuality;
using Spark::Graphics::DenoiserSettings;
using Spark::Graphics::IDenoiser;
using Spark::Graphics::SoftwareDenoiser;

namespace
{
    constexpr uint32_t kW = 8;
    constexpr uint32_t kH = 8;

    // Build a flat RGB image with a uniform color.
    std::vector<float> MakeFlatImage(float r, float g, float b)
    {
        std::vector<float> image(static_cast<size_t>(kW) * kH * 3);
        for (uint32_t i = 0; i < kW * kH; ++i)
        {
            image[i * 3 + 0] = r;
            image[i * 3 + 1] = g;
            image[i * 3 + 2] = b;
        }
        return image;
    }

    // Add pseudo-random noise to an image. Uses a simple hash-based
    // sequence so the output is deterministic across platforms but
    // still irregular enough for the bilateral filter to smooth. A
    // pure checkerboard pattern defeats the joint filter because
    // every neighbor's color-delta is uniform.
    std::vector<float> MakeNoisyImage(float baseR, float baseG, float baseB, float noiseAmplitude = 0.1f)
    {
        std::vector<float> image(static_cast<size_t>(kW) * kH * 3);
        for (uint32_t y = 0; y < kH; ++y)
        {
            for (uint32_t x = 0; x < kW; ++x)
            {
                // Deterministic hash of (x, y) mapped to [-1, 1].
                uint32_t h = x * 73856093u ^ y * 19349663u;
                h ^= (h >> 13);
                h *= 0x5bd1e995u;
                h ^= (h >> 15);
                auto bipolar = [](uint32_t seed) -> float
                {
                    uint32_t s = seed;
                    s ^= s << 13;
                    s ^= s >> 17;
                    s ^= s << 5;
                    return (static_cast<float>(s & 0xFFFFu) / 32767.5f) - 1.0f;
                };
                uint32_t idx = (y * kW + x) * 3;
                image[idx + 0] = baseR + bipolar(h) * noiseAmplitude;
                image[idx + 1] = baseG + bipolar(h + 1u) * noiseAmplitude;
                image[idx + 2] = baseB + bipolar(h + 2u) * noiseAmplitude;
            }
        }
        return image;
    }

    // Compute variance of an RGB image (sum of per-channel variances).
    float ComputeVariance(const float* image, uint32_t width, uint32_t height)
    {
        float meanR = 0, meanG = 0, meanB = 0;
        uint32_t n = width * height;
        for (uint32_t i = 0; i < n; ++i)
        {
            meanR += image[i * 3 + 0];
            meanG += image[i * 3 + 1];
            meanB += image[i * 3 + 2];
        }
        meanR /= n;
        meanG /= n;
        meanB /= n;

        float var = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            float dr = image[i * 3 + 0] - meanR;
            float dg = image[i * 3 + 1] - meanG;
            float db = image[i * 3 + 2] - meanB;
            var += dr * dr + dg * dg + db * db;
        }
        return var / n;
    }

    DenoiserBuffer MakeBuffer(const std::vector<float>& data, uint32_t w, uint32_t h)
    {
        DenoiserBuffer buf;
        buf.data = data.data();
        buf.width = w;
        buf.height = h;
        buf.channels = 3;
        buf.rowStride = w * 3 * sizeof(float);
        return buf;
    }
} // namespace

// =========================================================================
// DenoiserSettings defaults
// =========================================================================

TEST(DenoiserInterface_DefaultSettings)
{
    DenoiserSettings s;
    EXPECT_FALSE(s.enabled);
    // enum class values are compared via underlying int so the test
    // framework's ostream-based diagnostic path can format them.
    EXPECT_EQ(static_cast<int>(s.backend), static_cast<int>(DenoiserBackend::Software));
    EXPECT_EQ(static_cast<int>(s.quality), static_cast<int>(DenoiserQuality::Balanced));
    EXPECT_TRUE(s.useAlbedoGuide);
    EXPECT_TRUE(s.useNormalGuide);
    EXPECT_FALSE(s.temporal);
    EXPECT_NEAR(s.blendFactor, 0.1f, 1e-6f);
}

// =========================================================================
// SoftwareDenoiser lifecycle
// =========================================================================

TEST(DenoiserInterface_SoftwareInitializeAndShutdown)
{
    SoftwareDenoiser d;
    DenoiserSettings s;
    EXPECT_TRUE(d.Initialize(s));
    EXPECT_EQ(static_cast<int>(d.GetBackend()), static_cast<int>(DenoiserBackend::Software));
    d.Shutdown();
}

TEST(DenoiserInterface_SoftwareExecuteBeforeInitializeFails)
{
    SoftwareDenoiser d;
    EXPECT_FALSE(d.Execute());
}

TEST(DenoiserInterface_SoftwareExecuteWithoutInputFails)
{
    SoftwareDenoiser d;
    DenoiserSettings s;
    d.Initialize(s);
    // No SetColorInput — Execute must return false (no data).
    EXPECT_FALSE(d.Execute());
    d.Shutdown();
}

// =========================================================================
// Passthrough / constant-field behaviour
// =========================================================================

TEST(DenoiserInterface_SoftwareConstantImageRoundTrip)
{
    SoftwareDenoiser d;
    DenoiserSettings s;
    d.Initialize(s);

    auto img = MakeFlatImage(0.5f, 0.6f, 0.7f);
    d.SetColorInput(MakeBuffer(img, kW, kH));
    EXPECT_TRUE(d.Execute());

    const float* out = d.GetOutput();
    EXPECT_TRUE(out != nullptr);
    EXPECT_EQ(d.GetOutputWidth(), kW);
    EXPECT_EQ(d.GetOutputHeight(), kH);

    // A flat image should come out virtually unchanged (bilateral
    // filter on a constant field is ~identity).
    for (uint32_t i = 0; i < kW * kH; ++i)
    {
        EXPECT_NEAR(out[i * 3 + 0], 0.5f, 1e-3f);
        EXPECT_NEAR(out[i * 3 + 1], 0.6f, 1e-3f);
        EXPECT_NEAR(out[i * 3 + 2], 0.7f, 1e-3f);
    }
    d.Shutdown();
}

TEST(DenoiserInterface_SoftwareReducesVarianceOnNoisyInput)
{
    SoftwareDenoiser d;
    DenoiserSettings s;
    d.Initialize(s);

    auto noisy = MakeNoisyImage(0.5f, 0.5f, 0.5f, /*amplitude*/ 0.2f);
    float noisyVar = ComputeVariance(noisy.data(), kW, kH);
    EXPECT_GT(noisyVar, 0.001f); // sanity: input is noisy

    d.SetColorInput(MakeBuffer(noisy, kW, kH));
    EXPECT_TRUE(d.Execute());

    float denoisedVar = ComputeVariance(d.GetOutput(), kW, kH);
    // Bilateral filter reduces variance on high-frequency noise.
    EXPECT_LT(denoisedVar, noisyVar);
    d.Shutdown();
}

// =========================================================================
// Optional guide buffers
// =========================================================================

TEST(DenoiserInterface_SoftwareAcceptsAlbedoGuide)
{
    SoftwareDenoiser d;
    DenoiserSettings s;
    d.Initialize(s);

    auto color = MakeFlatImage(0.3f, 0.3f, 0.3f);
    auto albedo = MakeFlatImage(0.8f, 0.2f, 0.1f);
    EXPECT_TRUE(d.SetColorInput(MakeBuffer(color, kW, kH)));
    EXPECT_TRUE(d.SetAlbedoGuide(MakeBuffer(albedo, kW, kH)));
    EXPECT_TRUE(d.Execute());
    d.Shutdown();
}

TEST(DenoiserInterface_SoftwareAcceptsNormalGuide)
{
    SoftwareDenoiser d;
    DenoiserSettings s;
    d.Initialize(s);

    auto color = MakeFlatImage(0.4f, 0.5f, 0.6f);
    auto normal = MakeFlatImage(0.0f, 0.0f, 1.0f); // all normals pointing +Z
    EXPECT_TRUE(d.SetColorInput(MakeBuffer(color, kW, kH)));
    EXPECT_TRUE(d.SetNormalGuide(MakeBuffer(normal, kW, kH)));
    EXPECT_TRUE(d.Execute());

    // Normal guide with a constant normal should not perturb a
    // constant input — result stays near the input.
    const float* out = d.GetOutput();
    for (uint32_t i = 0; i < kW * kH; ++i)
    {
        EXPECT_NEAR(out[i * 3 + 0], 0.4f, 1e-3f);
        EXPECT_NEAR(out[i * 3 + 1], 0.5f, 1e-3f);
        EXPECT_NEAR(out[i * 3 + 2], 0.6f, 1e-3f);
    }
    d.Shutdown();
}

TEST(DenoiserInterface_SoftwareWithBothGuidesSucceeds)
{
    SoftwareDenoiser d;
    DenoiserSettings s;
    d.Initialize(s);

    auto color = MakeNoisyImage(0.5f, 0.5f, 0.5f, 0.15f);
    auto albedo = MakeFlatImage(0.9f, 0.9f, 0.9f);
    auto normal = MakeFlatImage(0.0f, 0.0f, 1.0f);
    d.SetColorInput(MakeBuffer(color, kW, kH));
    d.SetAlbedoGuide(MakeBuffer(albedo, kW, kH));
    d.SetNormalGuide(MakeBuffer(normal, kW, kH));
    EXPECT_TRUE(d.Execute());
    d.Shutdown();
}

// =========================================================================
// Polymorphic dispatch via IDenoiser base
// =========================================================================

TEST(DenoiserInterface_PolymorphicDispatch)
{
    std::unique_ptr<IDenoiser> d = std::make_unique<SoftwareDenoiser>();
    DenoiserSettings s;
    EXPECT_TRUE(d->Initialize(s));
    EXPECT_EQ(static_cast<int>(d->GetBackend()), static_cast<int>(DenoiserBackend::Software));

    auto img = MakeFlatImage(0.25f, 0.5f, 0.75f);
    d->SetColorInput(MakeBuffer(img, kW, kH));
    EXPECT_TRUE(d->Execute());
    EXPECT_EQ(d->GetOutputWidth(), kW);
    EXPECT_EQ(d->GetOutputHeight(), kH);
    d->Shutdown();
}

TEST(DenoiserInterface_LastExecutionTimeIsNonNegative)
{
    SoftwareDenoiser d;
    DenoiserSettings s;
    d.Initialize(s);
    // No Execute yet — LastExecutionTime must still be valid (>=0).
    EXPECT_GE(d.GetLastExecutionTimeMs(), 0.0f);
    d.Shutdown();
}

// =========================================================================
// Settings variations
// =========================================================================

TEST(DenoiserInterface_QualityPresetsAreAccepted)
{
    SoftwareDenoiser d;
    DenoiserSettings s;
    s.quality = DenoiserQuality::Fast;
    EXPECT_TRUE(d.Initialize(s));
    d.Shutdown();

    s.quality = DenoiserQuality::High;
    EXPECT_TRUE(d.Initialize(s));
    d.Shutdown();
}

TEST(DenoiserInterface_TemporalSettingAccepted)
{
    SoftwareDenoiser d;
    DenoiserSettings s;
    s.temporal = true;
    s.blendFactor = 0.5f;
    EXPECT_TRUE(d.Initialize(s));

    auto img = MakeFlatImage(0.5f, 0.5f, 0.5f);
    d.SetColorInput(MakeBuffer(img, kW, kH));
    EXPECT_TRUE(d.Execute());
    d.Shutdown();
}

// =========================================================================
// Repeated Execute calls
// =========================================================================

TEST(DenoiserInterface_RepeatedExecuteProducesSameOutput)
{
    SoftwareDenoiser d;
    DenoiserSettings s;
    d.Initialize(s);

    auto noisy = MakeNoisyImage(0.5f, 0.5f, 0.5f, 0.15f);
    d.SetColorInput(MakeBuffer(noisy, kW, kH));

    d.Execute();
    std::vector<float> first(d.GetOutput(), d.GetOutput() + kW * kH * 3);

    d.Execute();
    const float* second = d.GetOutput();
    for (uint32_t i = 0; i < kW * kH * 3; ++i)
    {
        EXPECT_NEAR(first[i], second[i], 1e-6f);
    }
    d.Shutdown();
}
