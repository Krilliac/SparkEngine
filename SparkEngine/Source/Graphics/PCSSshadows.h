/**
 * @file PCSSshadows.h
 * @brief Percentage-Closer Soft Shadows with variable penumbra
 * @author Spark Engine Team
 * @date 2025
 *
 * Implements PCSS (Percentage-Closer Soft Shadows) for realistic soft shadow
 * rendering with distance-dependent penumbra size. Shadows are sharper near
 * contact points and softer further from occluders.
 *
 * Algorithm: Blocker search → Penumbra estimation → PCF with variable kernel
 *
 * @see ShadowAtlas.h, CachedShadowAtlas.h, LightingSystem.h
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // PCSS Settings
    // =========================================================================

    struct PCSSSettings
    {
        bool enabled = true;
        float lightSize = 1.0f;        ///< Light source size (world units, larger = softer)
        int blockerSearchSamples = 16; ///< Samples for blocker depth search
        int pcfSamples = 32;           ///< Samples for final PCF filter
        float maxPenumbraSize = 32.0f; ///< Max penumbra in shadow map texels
        float minPenumbraSize = 1.0f;  ///< Min penumbra (avoids hard edges)
        float nearPlane = 0.1f;        ///< Shadow frustum near plane
        float shadowBias = 0.002f;     ///< Depth bias to reduce self-shadowing
        float normalBias = 0.5f;       ///< Normal-based offset to reduce peter-panning
        bool useRotatedPoisson = true; ///< Rotate sample pattern per-pixel (reduces banding)
    };

    // =========================================================================
    // PCSS Sample Patterns
    // =========================================================================

    /**
     * @brief Pre-computed Poisson disk sample points for PCSS filtering
     */
    struct PCSSSamplePattern
    {
        static constexpr int kMaxSamples = 64;

        struct Sample
        {
            float x, y;
        };

        Sample samples[kMaxSamples];
        int count = 0;

        /** @brief Generate a Poisson disk distribution */
        void GeneratePoisson(int numSamples, uint32_t seed = 0)
        {
            count = std::min(numSamples, kMaxSamples);

            // Stratified jittered spiral pattern
            float goldenAngle = 2.399963f; // pi * (3 - sqrt(5))
            for (int i = 0; i < count; ++i)
            {
                float r = std::sqrt(static_cast<float>(i + 0.5f) / count);
                float theta = static_cast<float>(i) * goldenAngle;

                // Optional per-pixel rotation via seed
                if (seed != 0)
                {
                    float noise = static_cast<float>(seed ^ (i * 0x9E3779B9u)) * (1.0f / 4294967296.0f);
                    theta += noise * 6.28318f;
                }

                samples[i].x = r * std::cos(theta);
                samples[i].y = r * std::sin(theta);
            }
        }

        /** @brief Generate a regular grid pattern (fallback) */
        void GenerateGrid(int gridSize)
        {
            count = 0;
            for (int y = 0; y < gridSize && count < kMaxSamples; ++y)
            {
                for (int x = 0; x < gridSize && count < kMaxSamples; ++x)
                {
                    samples[count].x = (static_cast<float>(x) + 0.5f) / gridSize * 2.0f - 1.0f;
                    samples[count].y = (static_cast<float>(y) + 0.5f) / gridSize * 2.0f - 1.0f;
                    count++;
                }
            }
        }
    };

    // =========================================================================
    // PCSS Shadow Evaluator
    // =========================================================================

    /**
     * @brief CPU-side PCSS shadow computation (for reference/testing)
     *
     * The actual GPU implementation uses these same algorithms in HLSL.
     * This CPU version is used for validation and editor preview.
     */
    class PCSSShadowEvaluator
    {
      public:
        PCSSShadowEvaluator() { m_pattern.GeneratePoisson(32); }

        void SetSettings(const PCSSSettings& settings) { m_settings = settings; }
        const PCSSSettings& GetSettings() const { return m_settings; }

        /**
         * @brief Evaluate shadow at a point given a shadow map
         *
         * @param shadowMapDepths  Shadow map depth values (row-major, normalized [0,1])
         * @param mapWidth         Shadow map resolution
         * @param mapHeight        Shadow map resolution
         * @param uvX              Shadow map UV coordinate X [0,1]
         * @param uvY              Shadow map UV coordinate Y [0,1]
         * @param receiverDepth    Depth of the receiving surface [0,1]
         * @return Shadow factor [0=fully shadowed, 1=fully lit]
         */
        float Evaluate(const float* shadowMapDepths, uint32_t mapWidth, uint32_t mapHeight, float uvX, float uvY,
                       float receiverDepth) const
        {
            // Step 1: Blocker search — find average depth of occluders
            float avgBlockerDepth = 0.0f;
            int blockerCount = 0;
            float searchRadius = m_settings.lightSize / m_settings.nearPlane;

            for (int i = 0; i < m_settings.blockerSearchSamples && i < m_pattern.count; ++i)
            {
                float sampleU = uvX + m_pattern.samples[i].x * searchRadius / mapWidth;
                float sampleV = uvY + m_pattern.samples[i].y * searchRadius / mapHeight;

                float depth = SampleShadowMap(shadowMapDepths, mapWidth, mapHeight, sampleU, sampleV);
                if (depth < receiverDepth - m_settings.shadowBias)
                {
                    avgBlockerDepth += depth;
                    blockerCount++;
                }
            }

            // No blockers = fully lit
            if (blockerCount == 0)
                return 1.0f;

            avgBlockerDepth /= blockerCount;

            // Step 2: Penumbra estimation
            float penumbraSize = EstimatePenumbra(receiverDepth, avgBlockerDepth);

            // Step 3: PCF with variable kernel
            float shadow = 0.0f;
            for (int i = 0; i < m_settings.pcfSamples && i < m_pattern.count; ++i)
            {
                float sampleU = uvX + m_pattern.samples[i].x * penumbraSize / mapWidth;
                float sampleV = uvY + m_pattern.samples[i].y * penumbraSize / mapHeight;

                float depth = SampleShadowMap(shadowMapDepths, mapWidth, mapHeight, sampleU, sampleV);
                shadow += (depth >= receiverDepth - m_settings.shadowBias) ? 1.0f : 0.0f;
            }

            return shadow / m_settings.pcfSamples;
        }

        /**
         * @brief Estimate penumbra size from blocker and receiver depths
         *
         * Based on the similar triangles formula:
         * penumbra = lightSize * (receiverDepth - blockerDepth) / blockerDepth
         */
        float EstimatePenumbra(float receiverDepth, float blockerDepth) const
        {
            if (blockerDepth <= 0.0001f)
                return m_settings.minPenumbraSize;

            float penumbra = m_settings.lightSize * (receiverDepth - blockerDepth) / blockerDepth;
            return std::clamp(penumbra, m_settings.minPenumbraSize, m_settings.maxPenumbraSize);
        }

        /** @brief Get the HLSL shader code for PCSS (embeddable in shadow sampling) */
        static const char* GetHLSLCode()
        {
            return R"(
// PCSS Shadow Sampling
// Call: float shadow = PCSShadow(shadowMap, shadowSampler, shadowUV, receiverDepth, lightSize);

static const int PCSS_BLOCKER_SAMPLES = 16;
static const int PCSS_PCF_SAMPLES = 32;

// Poisson disk samples (golden angle spiral)
static const float2 poissonDisk[32] = {
    float2(-0.9465, 0.1797), float2(-0.7431, -0.3055), float2(-0.5264, 0.6361),
    float2(-0.3159, -0.7548), float2(-0.1033, 0.0720), float2(0.1177, -0.4425),
    float2(0.3338, 0.3516), float2(0.5493, -0.1267), float2(0.7640, 0.5686),
    float2(-0.8813, -0.4724), float2(-0.6658, 0.7460), float2(-0.4502, -0.0235),
    float2(-0.2347, 0.8945), float2(-0.0192, -0.6470), float2(0.1964, 0.2006),
    float2(0.4119, -0.5509), float2(0.6274, 0.7034), float2(0.8430, -0.3441),
    float2(-0.7888, 0.4256), float2(-0.5733, -0.5963), float2(-0.3578, 0.3249),
    float2(-0.1423, -0.8252), float2(0.0732, 0.4684), float2(0.2888, -0.2772),
    float2(0.5043, 0.6419), float2(0.7198, -0.6106), float2(0.9354, 0.1407),
    float2(-0.6201, 0.0463), float2(-0.4046, -0.4037), float2(-0.1891, 0.7537),
    float2(0.0264, -0.1037), float2(0.2420, 0.5537)
};

float PCSShadow(Texture2D shadowMap, SamplerComparisonState shadowSampler,
                float2 uv, float depth, float lightSize, float2 texelSize)
{
    // Step 1: Blocker search
    float blockerSum = 0;
    int blockerCount = 0;
    float searchRadius = lightSize * texelSize.x * 20.0;

    for (int i = 0; i < PCSS_BLOCKER_SAMPLES; i++)
    {
        float2 offset = poissonDisk[i] * searchRadius;
        float d = shadowMap.SampleLevel(shadowSampler, uv + offset, 0).r;
        if (d < depth)
        {
            blockerSum += d;
            blockerCount++;
        }
    }

    if (blockerCount == 0) return 1.0;

    float avgBlocker = blockerSum / blockerCount;

    // Step 2: Penumbra estimation
    float penumbra = lightSize * (depth - avgBlocker) / avgBlocker;
    penumbra = clamp(penumbra, 1.0, 32.0) * texelSize.x;

    // Step 3: Variable-size PCF
    float shadow = 0;
    for (int i = 0; i < PCSS_PCF_SAMPLES; i++)
    {
        float2 offset = poissonDisk[i] * penumbra;
        shadow += shadowMap.SampleCmpLevelZero(shadowSampler, uv + offset, depth).r;
    }

    return shadow / PCSS_PCF_SAMPLES;
}
)";
        }

      private:
        static float SampleShadowMap(const float* depths, uint32_t w, uint32_t h, float u, float v)
        {
            u = std::clamp(u, 0.0f, 1.0f);
            v = std::clamp(v, 0.0f, 1.0f);
            uint32_t x = std::min(static_cast<uint32_t>(u * w), w - 1);
            uint32_t y = std::min(static_cast<uint32_t>(v * h), h - 1);
            return depths[y * w + x];
        }

        PCSSSettings m_settings;
        PCSSSamplePattern m_pattern;
    };

} // namespace Spark::Graphics
