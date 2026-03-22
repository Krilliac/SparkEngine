/**
 * @file BilateralUpsample.h
 * @brief Shared bilateral upsample utility for half-resolution effects
 * @author Spark Engine Team
 * @date 2025
 *
 * Edge-preserving upsampling for effects computed at half or quarter resolution
 * (SSAO, volumetric fog, SSR). Uses depth-aware bilateral weights to prevent
 * blurring across depth discontinuities (object edges).
 *
 * Shared across multiple effects to avoid duplicating upsample code.
 *
 * @see ScreenSpaceEffects.h, FogSystem.h, SSAOTemporal.h
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Bilateral Upsample Settings
    // =========================================================================

    struct BilateralUpsampleSettings
    {
        float depthThreshold = 0.01f; ///< Depth difference threshold for edge detection
        float normalThreshold = 0.9f; ///< Normal dot product threshold for edge detection
        bool useNormals = false;      ///< Use normal buffer for edge detection (in addition to depth)
    };

    // =========================================================================
    // Bilateral Upsampler
    // =========================================================================

    /**
     * @brief Edge-preserving bilateral upsample from half-res to full-res
     *
     * CPU reference implementation. GPU version runs as a pixel/compute shader.
     *
     * Algorithm:
     * 1. For each full-res pixel, find the 4 nearest half-res samples
     * 2. Weight each sample by depth similarity (closer depth = higher weight)
     * 3. If no samples have similar depth (edge), use nearest-depth fallback
     */
    class BilateralUpsampler
    {
      public:
        BilateralUpsampler() = default;
        ~BilateralUpsampler() = default;

        /**
         * @brief Upsample a half-resolution buffer to full resolution
         *
         * @param halfResData      Half-res input (single channel, row-major)
         * @param halfWidth        Half-res width
         * @param halfHeight       Half-res height
         * @param fullDepth        Full-res depth buffer
         * @param halfDepth        Half-res depth buffer (sampled at half-res centers)
         * @param fullWidth        Full-res width
         * @param fullHeight       Full-res height
         * @param output           Output buffer (full-res, single channel)
         */
        void Upsample(const float* halfResData, uint32_t halfWidth, uint32_t halfHeight, const float* fullDepth,
                      const float* halfDepth, uint32_t fullWidth, uint32_t fullHeight, float* output) const
        {
            for (uint32_t y = 0; y < fullHeight; ++y)
            {
                for (uint32_t x = 0; x < fullWidth; ++x)
                {
                    uint32_t fullIdx = y * fullWidth + x;

                    // Map full-res pixel to half-res coordinates
                    float hx = static_cast<float>(x) * halfWidth / fullWidth;
                    float hy = static_cast<float>(y) * halfHeight / fullHeight;

                    int hx0 = std::clamp(static_cast<int>(hx), 0, static_cast<int>(halfWidth - 1));
                    int hy0 = std::clamp(static_cast<int>(hy), 0, static_cast<int>(halfHeight - 1));
                    int hx1 = std::min(hx0 + 1, static_cast<int>(halfWidth - 1));
                    int hy1 = std::min(hy0 + 1, static_cast<int>(halfHeight - 1));

                    float fullD = fullDepth[fullIdx];

                    // 4 half-res samples with depth-based weights
                    float samples[4];
                    float depths[4];
                    float weights[4];

                    samples[0] = halfResData[hy0 * halfWidth + hx0];
                    samples[1] = halfResData[hy0 * halfWidth + hx1];
                    samples[2] = halfResData[hy1 * halfWidth + hx0];
                    samples[3] = halfResData[hy1 * halfWidth + hx1];

                    depths[0] = halfDepth[hy0 * halfWidth + hx0];
                    depths[1] = halfDepth[hy0 * halfWidth + hx1];
                    depths[2] = halfDepth[hy1 * halfWidth + hx0];
                    depths[3] = halfDepth[hy1 * halfWidth + hx1];

                    // Bilinear weights
                    float fx = hx - hx0;
                    float fy = hy - hy0;
                    float bilinearWeights[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};

                    // Depth-based bilateral weights
                    float totalWeight = 0.0f;
                    for (int i = 0; i < 4; ++i)
                    {
                        float depthDiff = std::abs(fullD - depths[i]);
                        float depthWeight = std::exp(-depthDiff / std::max(m_settings.depthThreshold, 0.0001f));
                        weights[i] = bilinearWeights[i] * depthWeight;
                        totalWeight += weights[i];
                    }

                    if (totalWeight > 0.0001f)
                    {
                        float result = 0.0f;
                        for (int i = 0; i < 4; ++i)
                            result += samples[i] * weights[i];
                        output[fullIdx] = result / totalWeight;
                    }
                    else
                    {
                        // Fallback: use the sample with closest depth
                        int closest = 0;
                        float minDiff = std::abs(fullD - depths[0]);
                        for (int i = 1; i < 4; ++i)
                        {
                            float diff = std::abs(fullD - depths[i]);
                            if (diff < minDiff)
                            {
                                minDiff = diff;
                                closest = i;
                            }
                        }
                        output[fullIdx] = samples[closest];
                    }
                }
            }
        }

        /**
         * @brief Upsample multi-channel data (e.g., RGB volumetric fog)
         */
        void UpsampleRGB(const float* halfResRGB, uint32_t halfWidth, uint32_t halfHeight, const float* fullDepth,
                         const float* halfDepth, uint32_t fullWidth, uint32_t fullHeight, float* outputRGB) const
        {
            // Process each channel independently
            size_t halfPixels = halfWidth * halfHeight;
            size_t fullPixels = fullWidth * fullHeight;

            std::vector<float> channelIn(halfPixels);
            std::vector<float> channelOut(fullPixels);

            for (int ch = 0; ch < 3; ++ch)
            {
                // Extract channel
                for (size_t i = 0; i < halfPixels; ++i)
                    channelIn[i] = halfResRGB[i * 3 + ch];

                Upsample(channelIn.data(), halfWidth, halfHeight, fullDepth, halfDepth, fullWidth, fullHeight,
                         channelOut.data());

                // Write back
                for (size_t i = 0; i < fullPixels; ++i)
                    outputRGB[i * 3 + ch] = channelOut[i];
            }
        }

        /** @brief Get HLSL shader code for GPU bilateral upsample */
        static const char* GetHLSLCode()
        {
            return R"(
// Bilateral Upsample — shared utility for half-res effects
// Input: half-res texture + full-res depth + half-res depth
// Output: full-res upsampled result

float BilateralUpsample(
    Texture2D<float> halfResTex,
    Texture2D<float> fullDepthTex,
    Texture2D<float> halfDepthTex,
    SamplerState pointSampler,
    float2 fullResUV,
    float2 halfResTexelSize,
    float depthThreshold)
{
    float fullDepth = fullDepthTex.Sample(pointSampler, fullResUV).r;

    // 4 nearest half-res texels
    float2 halfUV = fullResUV;
    float2 offsets[4] = {
        float2(0, 0),
        float2(halfResTexelSize.x, 0),
        float2(0, halfResTexelSize.y),
        halfResTexelSize
    };

    float result = 0;
    float totalWeight = 0;

    for (int i = 0; i < 4; i++)
    {
        float2 sampleUV = halfUV + offsets[i] - halfResTexelSize * 0.5;
        float sampleVal = halfResTex.Sample(pointSampler, sampleUV).r;
        float sampleDepth = halfDepthTex.Sample(pointSampler, sampleUV).r;

        float depthDiff = abs(fullDepth - sampleDepth);
        float weight = exp(-depthDiff / max(depthThreshold, 0.0001));

        result += sampleVal * weight;
        totalWeight += weight;
    }

    return totalWeight > 0.0001 ? result / totalWeight : halfResTex.Sample(pointSampler, halfUV).r;
}
)";
        }

        BilateralUpsampleSettings& GetSettings() { return m_settings; }
        const BilateralUpsampleSettings& GetSettings() const { return m_settings; }

      private:
        BilateralUpsampleSettings m_settings;
    };

} // namespace Spark::Graphics
