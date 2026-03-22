/**
 * @file SSAOTemporal.h
 * @brief Temporal denoising for SSAO with history reprojection
 * @author Spark Engine Team
 * @date 2025
 *
 * Adds temporal stability to SSAO by accumulating results across frames.
 * Uses motion vectors to reproject previous frame's AO and blends with
 * current frame using a configurable blend factor. Reduces noise without
 * increasing per-frame sample count.
 *
 * @see ScreenSpaceEffects.h, TemporalEffects.h
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
    // SSAO Temporal Settings
    // =========================================================================

    struct SSAOTemporalSettings
    {
        bool enabled = true;
        float blendFactor = 0.9f;           ///< History weight [0=no temporal, 1=full history]
        float motionRejectionScale = 10.0f; ///< Motion-based rejection sensitivity
        float depthRejectionScale = 100.0f; ///< Depth difference rejection sensitivity
        bool useVarianceClipping = true;    ///< Clip history to neighborhood variance
        float varianceGamma = 1.0f;         ///< Variance clip box size (1.0 = standard)
    };

    // =========================================================================
    // SSAO Temporal Filter
    // =========================================================================

    /**
     * @brief CPU reference implementation of temporal SSAO denoising
     *
     * GPU implementation uses the same algorithm as a compute shader.
     * Double-buffered AO textures: write to current, read from previous.
     */
    class SSAOTemporalFilter
    {
      public:
        SSAOTemporalFilter() = default;
        ~SSAOTemporalFilter() = default;

        void Initialize(uint32_t width, uint32_t height)
        {
            m_width = width;
            m_height = height;
            m_historyBuffer.resize(width * height, 0.0f);
            m_outputBuffer.resize(width * height, 0.0f);
            m_frameCount = 0;
            m_initialized = true;
        }

        void Shutdown()
        {
            m_historyBuffer.clear();
            m_outputBuffer.clear();
            m_initialized = false;
        }

        void Resize(uint32_t width, uint32_t height)
        {
            m_width = width;
            m_height = height;
            m_historyBuffer.assign(width * height, 0.0f);
            m_outputBuffer.assign(width * height, 0.0f);
            m_frameCount = 0;
        }

        /**
         * @brief Apply temporal filtering to SSAO output
         *
         * @param currentAO    Current frame's SSAO values (single channel, [0,1])
         * @param motionX      Per-pixel motion vectors X (in pixels)
         * @param motionY      Per-pixel motion vectors Y (in pixels)
         * @param depthCurrent Current frame depth values
         * @param depthPrev    Previous frame depth values (for rejection)
         * @return Temporally filtered AO values
         */
        const float* Apply(const float* currentAO, const float* motionX, const float* motionY,
                           const float* depthCurrent, const float* depthPrev)
        {
            if (!m_initialized || !currentAO)
                return currentAO;

            m_frameCount++;

            for (uint32_t y = 0; y < m_height; ++y)
            {
                for (uint32_t x = 0; x < m_width; ++x)
                {
                    uint32_t idx = y * m_width + x;
                    float currentVal = currentAO[idx];

                    // Reproject: find where this pixel was in the previous frame
                    float prevX = static_cast<float>(x);
                    float prevY = static_cast<float>(y);
                    if (motionX && motionY)
                    {
                        prevX -= motionX[idx];
                        prevY -= motionY[idx];
                    }

                    // Sample history with bilinear interpolation
                    float historyVal = SampleBilinear(m_historyBuffer.data(), prevX, prevY);

                    // Rejection: discard history if motion or depth difference is too large
                    float blendWeight = m_settings.blendFactor;

                    if (motionX && motionY)
                    {
                        float motionLen = std::sqrt(motionX[idx] * motionX[idx] + motionY[idx] * motionY[idx]);
                        float motionReject = std::exp(-motionLen * m_settings.motionRejectionScale);
                        blendWeight *= motionReject;
                    }

                    if (depthCurrent && depthPrev)
                    {
                        int px = std::clamp(static_cast<int>(prevX), 0, static_cast<int>(m_width - 1));
                        int py = std::clamp(static_cast<int>(prevY), 0, static_cast<int>(m_height - 1));
                        float depthDiff = std::abs(depthCurrent[idx] - depthPrev[py * m_width + px]);
                        float depthReject = std::exp(-depthDiff * m_settings.depthRejectionScale);
                        blendWeight *= depthReject;
                    }

                    // Variance clipping: clip history to neighborhood statistics
                    if (m_settings.useVarianceClipping)
                    {
                        float mean = 0.0f;
                        float variance = 0.0f;
                        int count = 0;

                        // 3x3 neighborhood statistics
                        for (int dy = -1; dy <= 1; ++dy)
                        {
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                int nx = static_cast<int>(x) + dx;
                                int ny = static_cast<int>(y) + dy;
                                if (nx >= 0 && nx < static_cast<int>(m_width) && ny >= 0 &&
                                    ny < static_cast<int>(m_height))
                                {
                                    float val = currentAO[ny * m_width + nx];
                                    mean += val;
                                    variance += val * val;
                                    count++;
                                }
                            }
                        }

                        if (count > 0)
                        {
                            mean /= count;
                            variance = variance / count - mean * mean;
                            float stddev = std::sqrt(std::max(variance, 0.0f));
                            float gamma = m_settings.varianceGamma;

                            float clampMin = mean - gamma * stddev;
                            float clampMax = mean + gamma * stddev;
                            historyVal = std::clamp(historyVal, clampMin, clampMax);
                        }
                    }

                    // Blend current with history
                    if (m_frameCount <= 1)
                    {
                        m_outputBuffer[idx] = currentVal; // No history on first frame
                    }
                    else
                    {
                        m_outputBuffer[idx] = std::lerp(currentVal, historyVal, blendWeight);
                    }
                }
            }

            // Swap: current output becomes next frame's history
            std::swap(m_historyBuffer, m_outputBuffer);

            return m_historyBuffer.data();
        }

        SSAOTemporalSettings& GetSettings() { return m_settings; }
        const SSAOTemporalSettings& GetSettings() const { return m_settings; }
        bool IsInitialized() const { return m_initialized; }

      private:
        float SampleBilinear(const float* buffer, float x, float y) const
        {
            x = std::clamp(x, 0.0f, static_cast<float>(m_width - 1));
            y = std::clamp(y, 0.0f, static_cast<float>(m_height - 1));

            int x0 = static_cast<int>(x);
            int y0 = static_cast<int>(y);
            int x1 = std::min(x0 + 1, static_cast<int>(m_width - 1));
            int y1 = std::min(y0 + 1, static_cast<int>(m_height - 1));

            float fx = x - x0;
            float fy = y - y0;

            float v00 = buffer[y0 * m_width + x0];
            float v10 = buffer[y0 * m_width + x1];
            float v01 = buffer[y1 * m_width + x0];
            float v11 = buffer[y1 * m_width + x1];

            return v00 * (1 - fx) * (1 - fy) + v10 * fx * (1 - fy) + v01 * (1 - fx) * fy + v11 * fx * fy;
        }

        SSAOTemporalSettings m_settings;
        std::vector<float> m_historyBuffer;
        std::vector<float> m_outputBuffer;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_frameCount = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
