/**
 * @file ScreenSpaceEffects.cpp
 * @brief Implementation of screen-space rendering effects manager
 *
 * Provides CPU-side configuration, SSAO kernel generation, and noise texture
 * creation for screen-space rendering techniques. Actual GPU shader dispatch
 * is handled by the renderer -- this file manages the CPU-side state and
 * generates the data buffers that would be uploaded to constant buffers
 * and textures for compute shader passes.
 */

#include "ScreenSpaceEffects.h"
#include "../Utils/Validate.h"

namespace Spark::Graphics
{

    bool ScreenSpaceEffects::Initialize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "ScreenSpaceEffects::Initialize: invalid dimensions (%ux%u)",
                            width, height);
            return false;
        }

        m_width = width;
        m_height = height;
        m_ssaoKernel = SSAOKernel::GenerateKernel(m_ssaoSettings.kernelSize);
        m_noiseTexture = SSAOKernel::GenerateNoiseTexture(m_ssaoSettings.noiseSize);

        // GPU resource creation would happen here:
        // - Allocate half-res or full-res SSAO render target
        // - Allocate SSR ray-march render target
        // - Create constant buffers for kernel data
        // - Upload noise texture to GPU

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "ScreenSpaceEffects initialized (%ux%u, SSAO kernel=%d)", width,
                       height, m_ssaoSettings.kernelSize);
        return true;
    }

    void ScreenSpaceEffects::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "ScreenSpaceEffects shutting down");
        m_ssaoKernel.clear();
        m_noiseTexture.clear();
        m_initialized = false;
    }

    void ScreenSpaceEffects::Resize(uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;

        // GPU: recreate render targets at new resolution
        // If half-res SSAO, targets would be width/2 x height/2
    }

    // =========================================================================
    // SSAO configuration
    // =========================================================================

    void ScreenSpaceEffects::SetSSAOEnabled(bool enabled)
    {
        m_ssaoSettings.enabled = enabled;
    }

    void ScreenSpaceEffects::SetSSAORadius(float radius)
    {
        m_ssaoSettings.radius = std::max(radius, 0.01f);
    }

    void ScreenSpaceEffects::SetSSAOIntensity(float intensity)
    {
        m_ssaoSettings.intensity = std::max(intensity, 0.0f);
    }

    void ScreenSpaceEffects::SetSSAOQuality(SSAOQuality quality)
    {
        m_ssaoSettings.quality = quality;
        m_ssaoSettings.kernelSize = m_ssaoSettings.GetSampleCount();
        m_ssaoSettings.halfResolution = m_ssaoSettings.IsHalfRes();

        // Regenerate kernel with new sample count
        m_ssaoKernel = SSAOKernel::GenerateKernel(m_ssaoSettings.kernelSize);

        // GPU: re-upload kernel buffer, resize render targets if half-res changed
    }

    std::vector<SamplePoint> ScreenSpaceEffects::GenerateSSAOKernel(int sampleCount, uint32_t seed)
    {
        m_ssaoKernel = SSAOKernel::GenerateKernel(sampleCount, seed);

        // GPU: upload new kernel data to constant buffer
        // The kernel is a hemisphere of sample offsets that the SSAO compute
        // shader uses to probe depth around each pixel

        return m_ssaoKernel;
    }

    std::vector<SamplePoint> ScreenSpaceEffects::GenerateNoiseTexture(int size, uint32_t seed)
    {
        m_noiseTexture = SSAOKernel::GenerateNoiseTexture(size, seed);

        // GPU: upload noise data to a small tiled texture (typically 4x4)
        // The noise rotates the sample kernel per-pixel to reduce banding

        return m_noiseTexture;
    }

    // =========================================================================
    // SSR configuration
    // =========================================================================

    void ScreenSpaceEffects::SetSSREnabled(bool enabled)
    {
        m_ssrSettings.enabled = enabled;
    }

    void ScreenSpaceEffects::SetSSRQuality(SSRQuality quality)
    {
        m_ssrSettings.quality = quality;
        m_ssrSettings.maxSteps = m_ssrSettings.GetStepCount();

        // GPU: update SSR constant buffer with new step count
        // Higher quality = more ray march steps + binary search refinement
    }

    // =========================================================================
    // Contact Shadows configuration
    // =========================================================================

    void ScreenSpaceEffects::SetContactShadowsEnabled(bool enabled)
    {
        m_contactShadowSettings.enabled = enabled;
    }

    // =========================================================================
    // Metrics and diagnostics
    // =========================================================================

    ScreenSpaceMetrics ScreenSpaceEffects::GetMetrics() const
    {
        ScreenSpaceMetrics m;
        m.ssaoActive = m_ssaoSettings.enabled;
        m.ssrActive = m_ssrSettings.enabled;
        m.contactShadowsActive = m_contactShadowSettings.enabled;
        m.ssaoSamples = m_ssaoSettings.enabled ? m_ssaoSettings.GetSampleCount() : 0;
        m.ssrSteps = m_ssrSettings.enabled ? m_ssrSettings.GetStepCount() : 0;
        m.totalEffects = (m.ssaoActive ? 1 : 0) + (m.ssrActive ? 1 : 0) + (m.contactShadowsActive ? 1 : 0);
        return m;
    }

    std::string ScreenSpaceEffects::Console_GetStatus() const
    {
        std::string s = "Screen-Space Effects:\n";
        s += "  SSAO: " + std::string(m_ssaoSettings.enabled ? "ON" : "OFF");
        if (m_ssaoSettings.enabled)
        {
            s += " (samples=" + std::to_string(m_ssaoSettings.GetSampleCount()) +
                 ", radius=" + std::to_string(m_ssaoSettings.radius) + ")";
        }
        s += "\n";
        s += "  SSR: " + std::string(m_ssrSettings.enabled ? "ON" : "OFF");
        if (m_ssrSettings.enabled)
        {
            s += " (steps=" + std::to_string(m_ssrSettings.GetStepCount()) +
                 ", maxDist=" + std::to_string(m_ssrSettings.maxDistance) + ")";
        }
        s += "\n";
        s += "  Contact Shadows: " + std::string(m_contactShadowSettings.enabled ? "ON" : "OFF") + "\n";
        return s;
    }

} // namespace Spark::Graphics
