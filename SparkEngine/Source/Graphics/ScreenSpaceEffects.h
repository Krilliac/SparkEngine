/**
 * @file ScreenSpaceEffects.h
 * @brief Screen-space rendering effects: SSAO, SSR, screen-space shadows
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides CPU-side configuration and math utilities for screen-space
 * rendering techniques. GPU shader dispatch is handled by the renderer.
 *
 * Supported effects:
 * - SSAO (Screen-Space Ambient Occlusion) with multiple kernel modes
 * - SSR (Screen-Space Reflections) via ray marching
 * - Screen-Space Shadows (contact shadows)
 *
 * ## Usage
 * @code
 *   ScreenSpaceEffects sse;
 *   sse.Initialize(1920, 1080);
 *   sse.SetSSAOEnabled(true);
 *   sse.SetSSAORadius(0.5f);
 *
 *   auto& ssaoConfig = sse.GetSSAOSettings();
 *   auto kernel = sse.GenerateSSAOKernel(64);
 *   auto noise = sse.GenerateNoiseTexture(4);
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"

#include <vector>
#include <cmath>
#include <cstdint>
#include <string>
#include <algorithm>
#include <random>

namespace Spark
{
    namespace Graphics
    {

        // =============================================================================
        // SSAO Settings
        // =============================================================================

        enum class SSAOQuality
        {
            Low,    ///< 16 samples, half-res
            Medium, ///< 32 samples, half-res
            High,   ///< 64 samples, full-res
            Ultra   ///< 128 samples, full-res
        };

        struct SSAOSettings
        {
            bool enabled = true;
            SSAOQuality quality = SSAOQuality::Medium;

            float radius = 0.5f;        ///< Sampling radius in world units
            float bias = 0.025f;        ///< Depth bias to reduce self-occlusion
            float intensity = 1.0f;     ///< Occlusion intensity multiplier
            float power = 2.0f;         ///< Power curve for occlusion falloff
            float maxDistance = 100.0f; ///< Max distance for AO computation

            int kernelSize = 32;        ///< Number of hemisphere samples
            int noiseSize = 4;          ///< Size of random rotation noise texture
            int blurPasses = 2;         ///< Edge-aware blur passes
            float blurSharpness = 8.0f; ///< Blur edge-awareness sharpness

            bool halfResolution = true; ///< Compute at half resolution
            bool useNormals = true;     ///< Use normals for hemisphere orientation

            int GetSampleCount() const
            {
                switch (quality)
                {
                case SSAOQuality::Low:
                    return 16;
                case SSAOQuality::Medium:
                    return 32;
                case SSAOQuality::High:
                    return 64;
                case SSAOQuality::Ultra:
                    return 128;
                }
                return 32;
            }

            bool IsHalfRes() const { return quality <= SSAOQuality::Medium; }
        };

        // =============================================================================
        // SSR Settings
        // =============================================================================

        enum class SSRQuality
        {
            Low,    ///< 16 steps, no refinement
            Medium, ///< 32 steps, binary refinement
            High,   ///< 64 steps, binary refinement + jitter
            Ultra   ///< 128 steps, binary refinement + jitter + temporal
        };

        struct SSRSettings
        {
            bool enabled = false;
            SSRQuality quality = SSRQuality::Medium;

            float maxDistance = 50.0f; ///< Maximum ray march distance
            float stepSize = 0.5f;     ///< Initial ray step size
            float thickness = 0.5f;    ///< Depth buffer thickness for hit test
            float fadeStart = 0.8f;    ///< Screen edge fade start (0 to 1)
            float fadeEnd = 1.0f;      ///< Screen edge fade end

            int maxSteps = 32;               ///< Maximum ray march steps
            int binarySearchSteps = 8;       ///< Binary search refinement steps
            float roughnessThreshold = 0.5f; ///< Skip SSR above this roughness
            bool jitterEnabled = true;       ///< Temporal jitter for ray start
            float temporalBlend = 0.9f;      ///< Temporal reprojection weight

            int GetStepCount() const
            {
                switch (quality)
                {
                case SSRQuality::Low:
                    return 16;
                case SSRQuality::Medium:
                    return 32;
                case SSRQuality::High:
                    return 64;
                case SSRQuality::Ultra:
                    return 128;
                }
                return 32;
            }
        };

        // =============================================================================
        // Contact Shadow Settings
        // =============================================================================

        struct ContactShadowSettings
        {
            bool enabled = false;
            float maxDistance = 0.5f;  ///< Max ray distance in view space
            int stepCount = 16;        ///< Number of ray march steps
            float thickness = 0.05f;   ///< Depth comparison thickness
            float fadeDistance = 0.3f; ///< Distance at which shadows fade out
            float intensity = 0.8f;    ///< Shadow darkness (0 = none, 1 = black)
        };

        // =============================================================================
        // SSAO Kernel Generation
        // =============================================================================

        struct SamplePoint
        {
            float x, y, z;

            float Length() const { return std::sqrt(x * x + y * y + z * z); }

            SamplePoint Normalized() const
            {
                float len = Length();
                if (len < 0.0001f)
                    return {0, 1, 0};
                return {x / len, y / len, z / len};
            }

            SamplePoint operator*(float s) const { return {x * s, y * s, z * s}; }
        };

        namespace SSAOKernel
        {

            /// Generate hemisphere sample kernel with cosine-weighted distribution
            inline std::vector<SamplePoint> GenerateKernel(int sampleCount, uint32_t seed = 42)
            {
                std::vector<SamplePoint> kernel;
                kernel.reserve(sampleCount);
                std::mt19937 rng(seed);
                std::uniform_real_distribution<float> dist(0.0f, 1.0f);

                for (int i = 0; i < sampleCount; ++i)
                {
                    // Random point in hemisphere
                    float theta = 2.0f * 3.14159265f * dist(rng);
                    float cosPhi = dist(rng); // Cosine-weighted
                    float sinPhi = std::sqrt(1.0f - cosPhi * cosPhi);

                    SamplePoint sample = {
                        sinPhi * std::cos(theta), sinPhi * std::sin(theta),
                        cosPhi // Z is up (hemisphere oriented along normal)
                    };

                    // Accelerating interpolation: samples closer to origin are more likely
                    float scale = static_cast<float>(i) / static_cast<float>(sampleCount);
                    scale = 0.1f + scale * scale * 0.9f; // lerp(0.1, 1.0, scale^2)
                    sample = sample * scale;

                    kernel.push_back(sample);
                }
                return kernel;
            }

            /// Generate random rotation noise texture data (NxN, 2 channels)
            inline std::vector<SamplePoint> GenerateNoiseTexture(int size, uint32_t seed = 123)
            {
                std::vector<SamplePoint> noise;
                noise.reserve(size * size);
                std::mt19937 rng(seed);
                std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

                for (int i = 0; i < size * size; ++i)
                {
                    SamplePoint n = {dist(rng), dist(rng), 0.0f};
                    float len = std::sqrt(n.x * n.x + n.y * n.y);
                    if (len > 0.0001f)
                    {
                        n.x /= len;
                        n.y /= len;
                    }
                    noise.push_back(n);
                }
                return noise;
            }

        } // namespace SSAOKernel

        // =============================================================================
        // Screen-Space Effects Manager
        // =============================================================================

        struct ScreenSpaceMetrics
        {
            bool ssaoActive = false;
            bool ssrActive = false;
            bool contactShadowsActive = false;
            int ssaoSamples = 0;
            int ssrSteps = 0;
            int totalEffects = 0;
        };

        class ScreenSpaceEffects
        {
          public:
            ScreenSpaceEffects() = default;
            ~ScreenSpaceEffects() = default;

            bool Initialize(uint32_t width = 1920, uint32_t height = 1080)
            {
                m_width = width;
                m_height = height;
                m_ssaoKernel = SSAOKernel::GenerateKernel(m_ssaoSettings.kernelSize);
                m_noiseTexture = SSAOKernel::GenerateNoiseTexture(m_ssaoSettings.noiseSize);
                m_initialized = true;
                return true;
            }

            void Shutdown()
            {
                m_ssaoKernel.clear();
                m_noiseTexture.clear();
                m_initialized = false;
            }

            void Resize(uint32_t width, uint32_t height)
            {
                m_width = width;
                m_height = height;
            }

            // ---- SSAO ----
            void SetSSAOEnabled(bool enabled) { m_ssaoSettings.enabled = enabled; }
            bool IsSSAOEnabled() const { return m_ssaoSettings.enabled; }
            void SetSSAORadius(float radius) { m_ssaoSettings.radius = std::max(radius, 0.01f); }
            void SetSSAOIntensity(float intensity) { m_ssaoSettings.intensity = std::max(intensity, 0.0f); }
            void SetSSAOQuality(SSAOQuality quality)
            {
                m_ssaoSettings.quality = quality;
                m_ssaoSettings.kernelSize = m_ssaoSettings.GetSampleCount();
                m_ssaoSettings.halfResolution = m_ssaoSettings.IsHalfRes();
                m_ssaoKernel = SSAOKernel::GenerateKernel(m_ssaoSettings.kernelSize);
            }
            const SSAOSettings& GetSSAOSettings() const { return m_ssaoSettings; }
            SSAOSettings& GetSSAOSettings() { return m_ssaoSettings; }
            const std::vector<SamplePoint>& GetSSAOKernel() const { return m_ssaoKernel; }
            const std::vector<SamplePoint>& GetNoiseTexture() const { return m_noiseTexture; }

            /// Regenerate kernel with new sample count
            std::vector<SamplePoint> GenerateSSAOKernel(int sampleCount, uint32_t seed = 42)
            {
                m_ssaoKernel = SSAOKernel::GenerateKernel(sampleCount, seed);
                return m_ssaoKernel;
            }

            /// Regenerate noise texture
            std::vector<SamplePoint> GenerateNoiseTexture(int size, uint32_t seed = 123)
            {
                m_noiseTexture = SSAOKernel::GenerateNoiseTexture(size, seed);
                return m_noiseTexture;
            }

            // ---- SSR ----
            void SetSSREnabled(bool enabled) { m_ssrSettings.enabled = enabled; }
            bool IsSSREnabled() const { return m_ssrSettings.enabled; }
            void SetSSRQuality(SSRQuality quality)
            {
                m_ssrSettings.quality = quality;
                m_ssrSettings.maxSteps = m_ssrSettings.GetStepCount();
            }
            const SSRSettings& GetSSRSettings() const { return m_ssrSettings; }
            SSRSettings& GetSSRSettings() { return m_ssrSettings; }

            // ---- Contact Shadows ----
            void SetContactShadowsEnabled(bool enabled) { m_contactShadowSettings.enabled = enabled; }
            bool IsContactShadowsEnabled() const { return m_contactShadowSettings.enabled; }
            const ContactShadowSettings& GetContactShadowSettings() const { return m_contactShadowSettings; }
            ContactShadowSettings& GetContactShadowSettings() { return m_contactShadowSettings; }

            // ---- Metrics ----
            ScreenSpaceMetrics GetMetrics() const
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

            std::string Console_GetStatus() const
            {
                std::string s = "Screen-Space Effects:\n";
                s += "  SSAO: " + std::string(m_ssaoSettings.enabled ? "ON" : "OFF");
                if (m_ssaoSettings.enabled)
                    s += " (samples=" + std::to_string(m_ssaoSettings.GetSampleCount()) +
                         ", radius=" + std::to_string(m_ssaoSettings.radius) + ")";
                s += "\n";
                s += "  SSR: " + std::string(m_ssrSettings.enabled ? "ON" : "OFF");
                if (m_ssrSettings.enabled)
                    s += " (steps=" + std::to_string(m_ssrSettings.GetStepCount()) +
                         ", maxDist=" + std::to_string(m_ssrSettings.maxDistance) + ")";
                s += "\n";
                s += "  Contact Shadows: " + std::string(m_contactShadowSettings.enabled ? "ON" : "OFF") + "\n";
                return s;
            }

          private:
            bool m_initialized = false;
            uint32_t m_width = 1920, m_height = 1080;

            SSAOSettings m_ssaoSettings;
            SSRSettings m_ssrSettings;
            ContactShadowSettings m_contactShadowSettings;

            std::vector<SamplePoint> m_ssaoKernel;
            std::vector<SamplePoint> m_noiseTexture;
        };

    } // namespace Graphics
} // namespace Spark
