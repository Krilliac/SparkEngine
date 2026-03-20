// TestSHLighting.cpp - Tests for SH-based IBL evaluation and BRDF LUT generation

#include "TestFramework.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// Standalone SH implementation for CI testing (no DirectXMath dependency)
namespace TestSH
{

    static constexpr int SH_COUNT = 9;
    static constexpr float PI = 3.14159265358979323846f;

    struct SHCoefficients
    {
        std::array<float, SH_COUNT> r{};
        std::array<float, SH_COUNT> g{};
        std::array<float, SH_COUNT> b{};

        void Scale(float factor)
        {
            for (int i = 0; i < SH_COUNT; ++i)
            {
                r[i] *= factor;
                g[i] *= factor;
                b[i] *= factor;
            }
        }

        void Accumulate(const SHCoefficients& other, float weight)
        {
            for (int i = 0; i < SH_COUNT; ++i)
            {
                r[i] += other.r[i] * weight;
                g[i] += other.g[i] * weight;
                b[i] += other.b[i] * weight;
            }
        }
    };

    static void EvaluateBasis(float nx, float ny, float nz, float outBasis[SH_COUNT])
    {
        outBasis[0] = 0.282095f;
        outBasis[1] = 0.488603f * ny;
        outBasis[2] = 0.488603f * nz;
        outBasis[3] = 0.488603f * nx;
        outBasis[4] = 1.092548f * nx * ny;
        outBasis[5] = 1.092548f * ny * nz;
        outBasis[6] = 0.315392f * (3.0f * nz * nz - 1);
        outBasis[7] = 1.092548f * nx * nz;
        outBasis[8] = 0.546274f * (nx * nx - ny * ny);
    }

    static void EvaluateIrradiance(const SHCoefficients& sh, float nx, float ny, float nz, float& outR, float& outG,
                                   float& outB)
    {
        float basis[SH_COUNT];
        EvaluateBasis(nx, ny, nz, basis);

        static constexpr float A0 = 3.141593f;
        static constexpr float A1 = 2.094395f;
        static constexpr float A2 = 0.785398f;
        static constexpr float weights[SH_COUNT] = {A0, A1, A1, A1, A2, A2, A2, A2, A2};

        outR = 0.0f;
        outG = 0.0f;
        outB = 0.0f;
        for (int i = 0; i < SH_COUNT; ++i)
        {
            float w = basis[i] * weights[i];
            outR += sh.r[i] * w;
            outG += sh.g[i] * w;
            outB += sh.b[i] * w;
        }

        outR = std::max(0.0f, outR);
        outG = std::max(0.0f, outG);
        outB = std::max(0.0f, outB);
    }

    static SHCoefficients ProjectDirectionalLight(float dirX, float dirY, float dirZ, float r, float g, float b)
    {
        SHCoefficients sh{};
        float basis[SH_COUNT];
        EvaluateBasis(dirX, dirY, dirZ, basis);
        for (int i = 0; i < SH_COUNT; ++i)
        {
            sh.r[i] = basis[i] * r;
            sh.g[i] = basis[i] * g;
            sh.b[i] = basis[i] * b;
        }
        return sh;
    }

    static SHCoefficients ProjectAmbient(float r, float g, float b)
    {
        SHCoefficients sh{};
        float scale = 1.0f / 0.282095f;
        sh.r[0] = r * scale;
        sh.g[0] = g * scale;
        sh.b[0] = b * scale;
        return sh;
    }

    struct BRDFLUTPixel
    {
        float scale = 0.0f;
        float bias = 0.0f;
    };

    static float RadicalInverse(uint32_t bits)
    {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        return static_cast<float>(bits) * 2.3283064365386963e-10f;
    }

    static float GeometrySchlickGGX(float NdotV, float roughness)
    {
        float k = (roughness * roughness) / 2.0f;
        return NdotV / (NdotV * (1.0f - k) + k);
    }

    static float GeometrySmith(float NdotV, float NdotL, float roughness)
    {
        return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
    }

    static std::vector<BRDFLUTPixel> GenerateBRDFLUT(uint32_t size, uint32_t numSamples)
    {
        std::vector<BRDFLUTPixel> lut(size * size);
        for (uint32_t y = 0; y < size; ++y)
        {
            float roughness = std::max(static_cast<float>(y + 1) / static_cast<float>(size), 0.001f);
            float alpha = roughness * roughness;
            for (uint32_t x = 0; x < size; ++x)
            {
                float NdotV = std::max(static_cast<float>(x + 1) / static_cast<float>(size), 0.001f);
                float scaleSum = 0.0f;
                float biasSum = 0.0f;
                for (uint32_t i = 0; i < numSamples; ++i)
                {
                    float xi1 = static_cast<float>(i) / static_cast<float>(numSamples);
                    float xi2 = RadicalInverse(i);
                    float phi = 2.0f * PI * xi1;
                    float cosTheta = std::sqrt((1.0f - xi2) / (1.0f + (alpha * alpha - 1.0f) * xi2));
                    float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
                    float hx = sinTheta * std::cos(phi);
                    float hy = sinTheta * std::sin(phi);
                    float hz = cosTheta;
                    float vx = std::sqrt(1.0f - NdotV * NdotV);
                    float vz = NdotV;
                    float VdotH = vx * hx + vz * hz;
                    float lz = 2.0f * VdotH * hz - vz;
                    float NdotL = std::max(lz, 0.0f);
                    float NdotH = std::max(hz, 0.0f);
                    VdotH = std::max(VdotH, 0.0f);
                    if (NdotL > 0.0f)
                    {
                        float G = GeometrySmith(NdotV, NdotL, roughness);
                        float Gv = G * VdotH / (NdotH * NdotV);
                        float Fc = std::pow(1.0f - VdotH, 5.0f);
                        scaleSum += Gv * (1.0f - Fc);
                        biasSum += Gv * Fc;
                    }
                }
                lut[y * size + x] = {scaleSum / static_cast<float>(numSamples),
                                     biasSum / static_cast<float>(numSamples)};
            }
        }
        return lut;
    }

} // namespace TestSH

TEST(SHLighting_BasisEvaluation)
{
    float basis[9];

    // +Z direction
    TestSH::EvaluateBasis(0.0f, 0.0f, 1.0f, basis);
    EXPECT_NEAR(basis[0], 0.282095f, 1e-4f);
    EXPECT_NEAR(basis[2], 0.488603f, 1e-4f);
    EXPECT_NEAR(basis[1], 0.0f, 1e-4f);
    EXPECT_NEAR(basis[3], 0.0f, 1e-4f);

    // +X direction
    TestSH::EvaluateBasis(1.0f, 0.0f, 0.0f, basis);
    EXPECT_NEAR(basis[3], 0.488603f, 1e-4f);
    EXPECT_NEAR(basis[1], 0.0f, 1e-4f);
    EXPECT_NEAR(basis[2], 0.0f, 1e-4f);
}

TEST(SHLighting_ProjectAmbient)
{
    auto sh = TestSH::ProjectAmbient(0.5f, 0.3f, 0.1f);

    EXPECT_TRUE(sh.r[0] != 0.0f);
    EXPECT_TRUE(sh.g[0] != 0.0f);
    EXPECT_TRUE(sh.b[0] != 0.0f);

    for (int i = 1; i < TestSH::SH_COUNT; ++i)
    {
        EXPECT_NEAR(sh.r[i], 0.0f, 1e-6f);
        EXPECT_NEAR(sh.g[i], 0.0f, 1e-6f);
        EXPECT_NEAR(sh.b[i], 0.0f, 1e-6f);
    }
}

TEST(SHLighting_ProjectDirectionalLight)
{
    auto sh = TestSH::ProjectDirectionalLight(0.0f, 1.0f, 0.0f, 1.0f, 0.5f, 0.25f);

    EXPECT_TRUE(sh.r[0] != 0.0f);
    EXPECT_TRUE(sh.r[1] != 0.0f);
    EXPECT_NEAR(sh.r[2], 0.0f, 1e-6f);

    EXPECT_NEAR(sh.g[0] / sh.r[0], 0.5f, 1e-4f);
    EXPECT_NEAR(sh.b[0] / sh.r[0], 0.25f, 1e-4f);
}

TEST(SHLighting_IrradianceNonNegative)
{
    auto sh = TestSH::ProjectDirectionalLight(0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    float r, g, b;

    float directions[][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}};
    for (auto& dir : directions)
    {
        TestSH::EvaluateIrradiance(sh, dir[0], dir[1], dir[2], r, g, b);
        EXPECT_GE(r, 0.0f);
        EXPECT_GE(g, 0.0f);
        EXPECT_GE(b, 0.0f);
    }

    float rFront, gFront, bFront;
    TestSH::EvaluateIrradiance(sh, 0.0f, 0.0f, 1.0f, rFront, gFront, bFront);
    float rBack, gBack, bBack;
    TestSH::EvaluateIrradiance(sh, 0.0f, 0.0f, -1.0f, rBack, gBack, bBack);
    EXPECT_GT(rFront, rBack);
}

TEST(SHLighting_CoefficientOperations)
{
    TestSH::SHCoefficients sh{};
    sh.r[0] = 1.0f;
    sh.g[0] = 2.0f;
    sh.b[0] = 3.0f;

    sh.Scale(0.5f);
    EXPECT_NEAR(sh.r[0], 0.5f, 1e-6f);
    EXPECT_NEAR(sh.g[0], 1.0f, 1e-6f);
    EXPECT_NEAR(sh.b[0], 1.5f, 1e-6f);

    TestSH::SHCoefficients other{};
    other.r[0] = 1.0f;
    other.g[0] = 1.0f;
    other.b[0] = 1.0f;

    sh.Accumulate(other, 2.0f);
    EXPECT_NEAR(sh.r[0], 2.5f, 1e-6f);
    EXPECT_NEAR(sh.g[0], 3.0f, 1e-6f);
    EXPECT_NEAR(sh.b[0], 3.5f, 1e-6f);
}

TEST(SHLighting_BRDFLUT)
{
    auto lut = TestSH::GenerateBRDFLUT(16, 32);
    EXPECT_EQ(lut.size(), static_cast<size_t>(16 * 16));

    for (const auto& pixel : lut)
    {
        EXPECT_GE(pixel.scale, 0.0f);
        EXPECT_GE(pixel.bias, 0.0f);
        EXPECT_LE(pixel.scale + pixel.bias, 1.5f);
    }

    // At high NdotV, low roughness: scale should be meaningful
    auto& smooth = lut[0 * 16 + 15];
    EXPECT_GT(smooth.scale, 0.1f);
}

TEST(SHLighting_Constants)
{
    EXPECT_EQ(TestSH::SH_COUNT, 9);
    EXPECT_NEAR(TestSH::PI, 3.14159265f, 1e-5f);
}
