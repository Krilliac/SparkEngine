/**
 * @file DenoiserInterface.h
 * @brief AI-powered image denoiser interface for ray-traced output
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides an interface for AI denoising of noisy ray-traced images.
 * Designed for Intel Open Image Denoise (OIDN) integration but abstracted
 * to support other denoiser backends (NVIDIA OptiX, AMD denoiser).
 *
 * Usage: Feed noisy color buffer + optional albedo/normal auxiliary buffers
 * to produce a clean, denoised image from 1-4 samples per pixel.
 *
 * @see HybridRT/HybridRTManager.h, DXRSupport.h
 *
 * @note **Lifecycle-only activation (Phase Q)** — SoftwareDenoiser is instantiated by
 * GraphicsEngine (m_denoiser) but Denoise() is never called from any render pass.
 * Full activation requires a ray-tracing pipeline consumer to feed noisy color/albedo/normal
 * buffers and invoke Denoise() after RT accumulation.
 *
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
    // Denoiser Types
    // =========================================================================

    enum class DenoiserBackend : uint8_t
    {
        None,
        OIDN_CPU, ///< Intel OIDN on CPU (SSE4.2/AVX2/AVX-512)
        OIDN_GPU, ///< Intel OIDN on GPU (CUDA/HIP/SYCL)
        OptiX,    ///< NVIDIA OptiX AI denoiser (CUDA)
        Neural,   ///< Neural MLP-based denoiser (compute shader)
        Software  ///< Simple bilateral filter fallback
    };

    enum class DenoiserQuality : uint8_t
    {
        Fast,     ///< Fastest, lowest quality (good for previews)
        Balanced, ///< Good quality/performance tradeoff
        High      ///< Highest quality (more samples, temporal)
    };

    struct DenoiserSettings
    {
        bool enabled = false;
        DenoiserBackend backend = DenoiserBackend::Software;
        DenoiserQuality quality = DenoiserQuality::Balanced;
        bool useAlbedoGuide = true; ///< Provide albedo buffer for better edges
        bool useNormalGuide = true; ///< Provide normal buffer for better geometry
        bool temporal = false;      ///< Use temporal accumulation across frames
        float blendFactor = 0.1f;   ///< Temporal blend: 0=full denoise, 1=no denoise
    };

    /**
     * @brief Image buffer for denoiser input/output
     */
    struct DenoiserBuffer
    {
        const float* data = nullptr; ///< RGB float data (3 channels, row-major)
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t channels = 3;  ///< 3 for RGB, 4 for RGBA
        uint32_t rowStride = 0; ///< Bytes per row (0 = tight packing)
    };

    // =========================================================================
    // Denoiser Interface
    // =========================================================================

    /**
     * @brief Abstract denoiser interface
     *
     * Designed for OIDN integration:
     * 1. Create denoiser with desired backend
     * 2. Set input buffers (color, optional albedo + normal)
     * 3. Execute() to run denoising
     * 4. Read output buffer
     */
    class IDenoiser
    {
      public:
        virtual ~IDenoiser() = default;

        virtual bool Initialize(const DenoiserSettings& settings) = 0;
        virtual void Shutdown() = 0;

        virtual bool SetColorInput(const DenoiserBuffer& color) = 0;
        virtual bool SetAlbedoGuide(const DenoiserBuffer& albedo) = 0;
        virtual bool SetNormalGuide(const DenoiserBuffer& normal) = 0;

        virtual bool Execute() = 0;

        virtual const float* GetOutput() const = 0;
        virtual uint32_t GetOutputWidth() const = 0;
        virtual uint32_t GetOutputHeight() const = 0;

        virtual DenoiserBackend GetBackend() const = 0;
        virtual float GetLastExecutionTimeMs() const = 0;
    };

    // =========================================================================
    // Software Denoiser (bilateral filter fallback)
    // =========================================================================

    /**
     * @brief Simple bilateral filter denoiser as OIDN fallback
     *
     * Not AI-powered — uses edge-preserving bilateral filtering.
     * Much lower quality than OIDN but works everywhere.
     */
    class SoftwareDenoiser : public IDenoiser
    {
      public:
        bool Initialize(const DenoiserSettings& settings) override
        {
            m_settings = settings;
            m_initialized = true;
            return true;
        }

        void Shutdown() override
        {
            m_output.clear();
            m_initialized = false;
        }

        bool SetColorInput(const DenoiserBuffer& color) override
        {
            m_colorInput = color;
            return true;
        }

        bool SetAlbedoGuide(const DenoiserBuffer& albedo) override
        {
            m_albedoInput = albedo;
            return true;
        }

        bool SetNormalGuide(const DenoiserBuffer& normal) override
        {
            m_normalInput = normal;
            return true;
        }

        bool Execute() override
        {
            if (!m_initialized || !m_colorInput.data)
                return false;

            uint32_t w = m_colorInput.width;
            uint32_t h = m_colorInput.height;
            m_output.resize(static_cast<size_t>(w) * h * 3);

            // Joint bilateral filter using color + optional guides
            constexpr int kRadius = 3;
            constexpr float kSigmaSpatial = 2.0f;
            constexpr float kSigmaColor = 0.1f;
            constexpr float kSigmaNormal = 0.5f;

            for (uint32_t y = 0; y < h; ++y)
            {
                for (uint32_t x = 0; x < w; ++x)
                {
                    float sumR = 0, sumG = 0, sumB = 0;
                    float sumWeight = 0;

                    const size_t centerIndex =
                        (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
                    const float* centerColor = m_colorInput.data + centerIndex;

                    for (int dy = -kRadius; dy <= kRadius; ++dy)
                    {
                        for (int dx = -kRadius; dx <= kRadius; ++dx)
                        {
                            int nx = static_cast<int>(x) + dx;
                            int ny = static_cast<int>(y) + dy;

                            if (nx < 0 || nx >= static_cast<int>(w) || ny < 0 || ny >= static_cast<int>(h))
                                continue;

                            const size_t neighborIndex =
                                (static_cast<size_t>(ny) * static_cast<size_t>(w) + static_cast<size_t>(nx)) * 3;
                            const float* neighborColor = m_colorInput.data + neighborIndex;

                            // Spatial weight
                            float spatialDist2 = static_cast<float>(dx * dx + dy * dy);
                            float ws = std::exp(-spatialDist2 / (2.0f * kSigmaSpatial * kSigmaSpatial));

                            // Color weight
                            float dr = centerColor[0] - neighborColor[0];
                            float dg = centerColor[1] - neighborColor[1];
                            float db = centerColor[2] - neighborColor[2];
                            float colorDist2 = dr * dr + dg * dg + db * db;
                            float wc = std::exp(-colorDist2 / (2.0f * kSigmaColor * kSigmaColor));

                            // Normal weight (if available)
                            float wn = 1.0f;
                            if (m_normalInput.data)
                            {
                                const float* cn = m_normalInput.data + centerIndex;
                                const float* nn = m_normalInput.data + neighborIndex;
                                float dot = cn[0] * nn[0] + cn[1] * nn[1] + cn[2] * nn[2];
                                wn = std::exp(-(1.0f - std::max(dot, 0.0f)) / (2.0f * kSigmaNormal * kSigmaNormal));
                            }

                            float weight = ws * wc * wn;
                            sumR += neighborColor[0] * weight;
                            sumG += neighborColor[1] * weight;
                            sumB += neighborColor[2] * weight;
                            sumWeight += weight;
                        }
                    }

                    uint32_t outIdx = (y * w + x) * 3;
                    if (sumWeight > 0.0001f)
                    {
                        m_output[outIdx + 0] = sumR / sumWeight;
                        m_output[outIdx + 1] = sumG / sumWeight;
                        m_output[outIdx + 2] = sumB / sumWeight;
                    }
                    else
                    {
                        m_output[outIdx + 0] = centerColor[0];
                        m_output[outIdx + 1] = centerColor[1];
                        m_output[outIdx + 2] = centerColor[2];
                    }
                }
            }

            return true;
        }

        const float* GetOutput() const override { return m_output.data(); }
        uint32_t GetOutputWidth() const override { return m_colorInput.width; }
        uint32_t GetOutputHeight() const override { return m_colorInput.height; }
        DenoiserBackend GetBackend() const override { return DenoiserBackend::Software; }
        float GetLastExecutionTimeMs() const override { return m_lastTimeMs; }

      private:
        DenoiserSettings m_settings;
        DenoiserBuffer m_colorInput;
        DenoiserBuffer m_albedoInput;
        DenoiserBuffer m_normalInput;
        std::vector<float> m_output;
        float m_lastTimeMs = 0.0f;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
