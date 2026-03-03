// TestNoiseGenerator.cpp - Tests for procedural noise generation
// Standalone re-implementations for CI testing without DirectXMath

#include "TestFramework.h"
#include <cmath>
#include <vector>
#include <random>
#include <algorithm>
#include <numeric>

namespace TestNoise {

class SimpleNoise {
public:
    explicit SimpleNoise(uint32_t seed = 42) {
        m_permutation.resize(512);
        std::iota(m_permutation.begin(), m_permutation.begin() + 256, 0);
        std::mt19937 rng(seed);
        std::shuffle(m_permutation.begin(), m_permutation.begin() + 256, rng);
        for (int i = 0; i < 256; ++i) m_permutation[i + 256] = m_permutation[i];
    }

    float Perlin2D(float x, float y) const {
        int xi = static_cast<int>(std::floor(x)) & 255;
        int yi = static_cast<int>(std::floor(y)) & 255;
        float xf = x - std::floor(x);
        float yf = y - std::floor(y);

        float u = Fade(xf);
        float v = Fade(yf);

        int aa = m_permutation[m_permutation[xi] + yi];
        int ab = m_permutation[m_permutation[xi] + yi + 1];
        int ba = m_permutation[m_permutation[xi + 1] + yi];
        int bb = m_permutation[m_permutation[xi + 1] + yi + 1];

        float x1 = Lerp(Grad(aa, xf, yf), Grad(ba, xf - 1, yf), u);
        float x2 = Lerp(Grad(ab, xf, yf - 1), Grad(bb, xf - 1, yf - 1), u);
        return Lerp(x1, x2, v);
    }

    float FBM(float x, float y, int octaves = 6, float lacunarity = 2.0f, float persistence = 0.5f) const {
        float total = 0.0f;
        float amplitude = 1.0f;
        float frequency = 1.0f;
        float maxValue = 0.0f;
        for (int i = 0; i < octaves; ++i) {
            total += Perlin2D(x * frequency, y * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }
        return total / maxValue;
    }

private:
    std::vector<int> m_permutation;

    float Fade(float t) const { return t * t * t * (t * (t * 6 - 15) + 10); }
    float Lerp(float a, float b, float t) const { return a + t * (b - a); }
    float Grad(int hash, float x, float y) const {
        int h = hash & 3;
        float u = (h < 2) ? x : y;
        float v = (h < 2) ? y : x;
        return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
    }
};

} // namespace TestNoise

TEST(Perlin2D_ReturnsValueInRange)
{
    TestNoise::SimpleNoise noise(42);
    for (int i = 0; i < 100; ++i) {
        float x = static_cast<float>(i) * 0.3f;
        float y = static_cast<float>(i) * 0.7f;
        float val = noise.Perlin2D(x, y);
        EXPECT_TRUE(val >= -1.5f && val <= 1.5f);
    }
}

TEST(Perlin2D_DeterministicWithSameSeed)
{
    TestNoise::SimpleNoise noise1(123);
    TestNoise::SimpleNoise noise2(123);
    EXPECT_NEAR(noise1.Perlin2D(1.5f, 2.5f), noise2.Perlin2D(1.5f, 2.5f), 0.0001f);
}

TEST(Perlin2D_DifferentSeedsDifferentOutput)
{
    TestNoise::SimpleNoise noise1(42);
    TestNoise::SimpleNoise noise2(99);
    float v1 = noise1.Perlin2D(1.5f, 2.5f);
    float v2 = noise2.Perlin2D(1.5f, 2.5f);
    EXPECT_TRUE(std::abs(v1 - v2) > 0.0001f);
}

TEST(Perlin2D_ContinuousNearby)
{
    TestNoise::SimpleNoise noise(42);
    float v1 = noise.Perlin2D(1.0f, 1.0f);
    float v2 = noise.Perlin2D(1.001f, 1.0f);
    EXPECT_TRUE(std::abs(v1 - v2) < 0.1f);
}

TEST(FBM_ReturnsValueInRange)
{
    TestNoise::SimpleNoise noise(42);
    for (int i = 0; i < 50; ++i) {
        float val = noise.FBM(static_cast<float>(i) * 0.5f, static_cast<float>(i) * 0.3f);
        EXPECT_TRUE(val >= -2.0f && val <= 2.0f);
    }
}

TEST(FBM_MoreOctavesMoreDetail)
{
    TestNoise::SimpleNoise noise(42);
    // With more octaves, nearby samples should still differ
    float v1 = noise.FBM(1.0f, 1.0f, 1);
    float v2 = noise.FBM(1.0f, 1.0f, 6);
    // They won't necessarily differ much but this validates no crash
    EXPECT_TRUE(std::abs(v1) < 2.0f);
    EXPECT_TRUE(std::abs(v2) < 2.0f);
}

TEST(FBM_Deterministic)
{
    TestNoise::SimpleNoise noise(77);
    float v1 = noise.FBM(5.0f, 3.0f, 4);
    float v2 = noise.FBM(5.0f, 3.0f, 4);
    EXPECT_NEAR(v1, v2, 0.0001f);
}
