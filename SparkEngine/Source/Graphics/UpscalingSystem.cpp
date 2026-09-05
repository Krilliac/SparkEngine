/**
 * @file UpscalingSystem.cpp
 * @brief CPU-side upscaling utilities: Halton jitter, DLL detection, resolution calculation, mode/quality names
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides cross-platform helpers that do not require D3D11 or GPU headers:
 *   - Halton low-discrepancy sequence generation for temporal jitter
 *   - Jitter offset calculation and NDC conversion
 *   - DLSS / XeSS runtime DLL detection (Windows path uses LoadLibrary via Platform.h)
 *   - Optimal render resolution calculation
 *   - Shader source accessors (returns string pointers from UpscalingShaders.h)
 *   - Upscaling mode / quality name lookups
 *   - Temporal mode detection and recommended jitter phase counts
 *   - UpscalingSystem::DetectFeatures (feature availability probing)
 *
 * GPU-side methods (resource creation, shader compilation, Execute dispatches)
 * live in UpscalingSystemWindows.cpp.
 */

#include "UpscalingSystem.h"
#include "../Core/Platform.h"
#include "UpscalingShaders.h"
#include "../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <utility>

// =============================================================================
// UPSCALING UTILITIES — Cross-platform helpers
// =============================================================================

namespace Spark
{
    namespace Graphics
    {
        namespace UpscalingUtils
        {

            // -------------------------------------------------------------------------
            // Halton Sequence
            // -------------------------------------------------------------------------

            /**
             * @brief Generate an element of a Halton low-discrepancy sequence
             *
             * Used to produce jitter offsets for temporal upscaling and TAA.
             * The Halton sequence provides well-distributed quasi-random sub-pixel
             * offsets across frames, minimising aliasing patterns.
             *
             * @param index  Frame index (0-based)
             * @param base   Prime base (use 2 for X, 3 for Y)
             * @return Value in [0, 1)
             */
            float GenerateHaltonSequence(uint32_t index, uint32_t base)
            {
                float result = 0.0f;
                float fraction = 1.0f / static_cast<float>(base);
                uint32_t i = index + 1; // Halton is 1-indexed

                while (i > 0)
                {
                    result += fraction * static_cast<float>(i % base);
                    i /= base;
                    fraction /= static_cast<float>(base);
                }

                return result;
            }

            // -------------------------------------------------------------------------
            // Jitter Offset Calculation
            // -------------------------------------------------------------------------

            /**
             * @brief Calculate sub-pixel jitter offset for temporal upscaling
             *
             * Returns a jitter offset in pixels using a Halton(2,3) sequence,
             * centred around zero (range [-0.5, 0.5] in render pixels).
             * This offset should be applied to the projection matrix before rendering
             * and provided to the upscaler for de-jittering.
             *
             * @param frameIndex   Current frame index
             * @param renderWidth  Internal render resolution width
             * @param renderHeight Internal render resolution height
             * @return Pair of (jitterX, jitterY) in pixel units
             */
            std::pair<float, float> CalculateJitterOffset(uint32_t frameIndex, uint32_t renderWidth,
                                                          uint32_t renderHeight)
            {
                // Use a 16-phase Halton sequence to keep the pattern periodic
                constexpr uint32_t kJitterPhaseCount = 16;
                uint32_t phaseIndex = frameIndex % kJitterPhaseCount;

                // Generate Halton values in [0, 1) and centre around zero
                float haltonX = GenerateHaltonSequence(phaseIndex, 2) - 0.5f;
                float haltonY = GenerateHaltonSequence(phaseIndex, 3) - 0.5f;

                // The jitter is in render-resolution pixel units
                // For projection matrix application, convert to NDC:
                //   ndcJitterX = jitterX * 2.0 / renderWidth
                //   ndcJitterY = jitterY * 2.0 / renderHeight
                (void)renderWidth;
                (void)renderHeight;

                return {haltonX, haltonY};
            }

            // -------------------------------------------------------------------------
            // DLL-based Feature Detection
            // -------------------------------------------------------------------------

            /**
             * @brief Check if NVIDIA DLSS is available on the system
             *
             * Checks for the presence of the NVIDIA NGX DLSS runtime DLL.
             * Actual DLSS evaluation also requires a compatible RTX GPU and
             * an appropriate driver version, but this serves as a first-pass gate.
             *
             * @return true if nvngx_dlss.dll is found
             */
            bool DetectDLSSAvailability()
            {
#ifdef SPARK_PLATFORM_WINDOWS
                // Try to load the DLSS DLL without executing DllMain entry point code
                HMODULE hModule = LoadLibraryExA("nvngx_dlss.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
                if (hModule != nullptr)
                {
                    FreeLibrary(hModule);
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DLSS runtime detected (nvngx_dlss.dll)");
                    return true;
                }

                // Also check the common NGX runtime path
                hModule = LoadLibraryExA("_nvngx.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
                if (hModule != nullptr)
                {
                    FreeLibrary(hModule);
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DLSS runtime detected (_nvngx.dll)");
                    return true;
                }

                SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "DLSS runtime not found");
                return false;
#else
                // DLSS is Windows + NVIDIA only
                return false;
#endif
            }

            /**
             * @brief Check if Intel XeSS is available on the system
             *
             * Checks for the presence of the Intel XeSS runtime DLL.
             * XeSS can run on any GPU that supports DP4a (most modern GPUs),
             * but is accelerated on Intel Arc GPUs with XMX hardware.
             *
             * @return true if libxess.dll is found
             */
            bool DetectXeSSAvailability()
            {
#ifdef SPARK_PLATFORM_WINDOWS
                HMODULE hModule = LoadLibraryExA("libxess.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
                if (hModule != nullptr)
                {
                    FreeLibrary(hModule);
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "XeSS runtime detected (libxess.dll)");
                    return true;
                }

                SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "XeSS runtime not found");
                return false;
#else
                // XeSS DX11/DX12 runtime is Windows only
                return false;
#endif
            }

            // -------------------------------------------------------------------------
            // Optimal Render Resolution
            // -------------------------------------------------------------------------

            /**
             * @brief Calculate the optimal render resolution for a given upscaling
             * configuration
             *
             * Returns the internal render resolution that the scene should be rendered
             * at before upscaling to the target display resolution.  Ensures even
             * dimensions for compute shader dispatch alignment.
             *
             * @param mode          Active upscaling mode
             * @param quality       Quality preset
             * @param displayWidth  Target display width
             * @param displayHeight Target display height
             * @return Pair of (renderWidth, renderHeight)
             */
            std::pair<uint32_t, uint32_t> GetOptimalRenderResolution(UpscalingMode mode, UpscalingQuality quality,
                                                                     uint32_t displayWidth, uint32_t displayHeight)
            {
                if (mode == UpscalingMode::None)
                {
                    return {displayWidth, displayHeight};
                }

                float scale = UpscalingSettings::GetRenderScale(quality);

                uint32_t renderWidth = std::max(1u, static_cast<uint32_t>(static_cast<float>(displayWidth) * scale));
                uint32_t renderHeight = std::max(1u, static_cast<uint32_t>(static_cast<float>(displayHeight) * scale));

                // Align to even dimensions for 8x8 thread group dispatch
                renderWidth = (renderWidth + 1u) & ~1u;
                renderHeight = (renderHeight + 1u) & ~1u;

                return {renderWidth, renderHeight};
            }

            // -------------------------------------------------------------------------
            // Shader Source Accessors
            // -------------------------------------------------------------------------

            /**
             * @brief Get the FSR 1.0 EASU compute shader HLSL source
             * @return Null-terminated HLSL string
             */
            const char* GetFSR1EASUShaderSource()
            {
                return Spark::UpscalingShaders::kFSR1_EASU_CS;
            }

            /**
             * @brief Get the FSR 1.0 RCAS compute shader HLSL source
             * @return Null-terminated HLSL string
             */
            const char* GetFSR1RCASShaderSource()
            {
                return Spark::UpscalingShaders::kFSR1_RCAS_CS;
            }

            /**
             * @brief Get the temporal upscaling compute shader HLSL source
             * @return Null-terminated HLSL string
             */
            const char* GetTemporalUpscalingShaderSource()
            {
                return Spark::UpscalingShaders::kTemporalUpscaling_CS;
            }

            /**
             * @brief Get the SparkSR temporal upscaling compute shader HLSL source
             * @return Null-terminated HLSL string
             */
            const char* GetSparkSRShaderSource()
            {
                return Spark::UpscalingShaders::kSparkSR_CS;
            }

            // -------------------------------------------------------------------------
            // Resolution Scaling Tables
            // -------------------------------------------------------------------------

            /**
             * @brief Get the human-readable name for an upscaling mode
             * @param mode The upscaling mode
             * @return String name (e.g. "FSR 1.0", "DLSS")
             */
            const char* GetUpscalingModeName(UpscalingMode mode)
            {
                switch (mode)
                {
                case UpscalingMode::None:
                    return "None";
                case UpscalingMode::FSR1:
                    return "FSR 1.0";
                case UpscalingMode::FSR2:
                    return "FSR 2.0";
                case UpscalingMode::DLSS:
                    return "DLSS";
                case UpscalingMode::XeSS:
                    return "XeSS";
                case UpscalingMode::SparkSR:
                    return "SparkSR";
                default:
                    return "Unknown";
                }
            }

            /**
             * @brief Get the human-readable name for a quality preset
             * @param quality The quality preset
             * @return String name (e.g. "Quality", "Performance")
             */
            const char* GetUpscalingQualityName(UpscalingQuality quality)
            {
                switch (quality)
                {
                case UpscalingQuality::UltraPerformance:
                    return "Ultra Performance";
                case UpscalingQuality::Performance:
                    return "Performance";
                case UpscalingQuality::Balanced:
                    return "Balanced";
                case UpscalingQuality::Quality:
                    return "Quality";
                case UpscalingQuality::UltraQuality:
                    return "Ultra Quality";
                case UpscalingQuality::Native:
                    return "Native";
                default:
                    return "Unknown";
                }
            }

            /**
             * @brief Check if a given upscaling mode requires temporal inputs
             *
             * Temporal modes need depth, motion vectors, and jitter offsets in
             * addition to the colour buffer.
             *
             * @param mode The upscaling mode to check
             * @return true if mode is temporal (FSR2, DLSS, XeSS, SparkSR)
             */
            bool IsTemporalMode(UpscalingMode mode)
            {
                switch (mode)
                {
                case UpscalingMode::FSR2:
                case UpscalingMode::DLSS:
                case UpscalingMode::XeSS:
                case UpscalingMode::SparkSR:
                    return true;
                default:
                    return false;
                }
            }

            /**
             * @brief Get the recommended jitter phase count for a quality preset
             *
             * Higher quality presets use fewer jitter phases because the render
             * resolution is closer to display and temporal convergence is faster.
             * Lower quality presets (higher upscale factor) need more samples to
             * reconstruct missing detail.
             *
             * @param quality Quality preset
             * @return Number of jitter phases in the Halton sequence
             */
            uint32_t GetRecommendedJitterPhaseCount(UpscalingQuality quality)
            {
                switch (quality)
                {
                case UpscalingQuality::UltraPerformance:
                    return 32;
                case UpscalingQuality::Performance:
                    return 23;
                case UpscalingQuality::Balanced:
                    return 18;
                case UpscalingQuality::Quality:
                    return 16;
                case UpscalingQuality::UltraQuality:
                    return 12;
                case UpscalingQuality::Native:
                    return 8;
                default:
                    return 16;
                }
            }

            /**
             * @brief Calculate a jitter offset using the recommended phase count
             *
             * Convenience wrapper that selects the appropriate Halton phase count
             * based on the quality preset and returns the pixel-space jitter offset.
             *
             * @param frameIndex   Current frame index
             * @param quality      Active quality preset
             * @param renderWidth  Internal render resolution width
             * @param renderHeight Internal render resolution height
             * @return Pair of (jitterX, jitterY) in pixel units
             */
            std::pair<float, float> CalculateJitterForQuality(uint32_t frameIndex, UpscalingQuality quality,
                                                              uint32_t renderWidth, uint32_t renderHeight)
            {
                uint32_t phaseCount = GetRecommendedJitterPhaseCount(quality);
                uint32_t phaseIndex = frameIndex % phaseCount;

                float haltonX = GenerateHaltonSequence(phaseIndex, 2) - 0.5f;
                float haltonY = GenerateHaltonSequence(phaseIndex, 3) - 0.5f;

                (void)renderWidth;
                (void)renderHeight;

                return {haltonX, haltonY};
            }

            /**
             * @brief Convert a pixel-space jitter offset to NDC for the projection
             * matrix
             *
             * The projection matrix jitter should be applied as a translation in
             * clip space:
             *   projection[2][0] += ndcJitterX;
             *   projection[2][1] += ndcJitterY;
             *
             * @param jitterX      Jitter X in render pixels
             * @param jitterY      Jitter Y in render pixels
             * @param renderWidth  Internal render resolution width
             * @param renderHeight Internal render resolution height
             * @return Pair of (ndcJitterX, ndcJitterY)
             */
            std::pair<float, float> JitterPixelsToNDC(float jitterX, float jitterY, uint32_t renderWidth,
                                                      uint32_t renderHeight)
            {
                float ndcX = jitterX * 2.0f / static_cast<float>(renderWidth);
                float ndcY = jitterY * 2.0f / static_cast<float>(renderHeight);
                return {ndcX, ndcY};
            }

        } // namespace UpscalingUtils
    } // namespace Graphics
} // namespace Spark

// =============================================================================
// UpscalingSystem — CPU-side out-of-line method definitions
// =============================================================================

void UpscalingSystem::DetectFeatures()
{
    // No vendor upscaling SDK is linked into this build: there is no FidelityFX
    // (FSR 2), NVIDIA NGX (DLSS) or Intel XeSS import library, so ExecuteFSR2 /
    // ExecuteDLSS / ExecuteXeSS cannot run a vendor upscaler no matter what the
    // machine has installed. Reporting them as "available" made the settings UI
    // and Console_GetStatus advertise upscalers that silently resolve to the
    // engine's own SparkSR temporal path — a false positive, not a capability.
    //
    // The runtime probes below stay: knowing that a DLSS/XeSS runtime is present
    // is useful diagnostics for whoever links the SDK, but presence of a DLL is
    // not availability of the feature.
    const bool dlssRuntimePresent = Spark::Graphics::UpscalingUtils::DetectDLSSAvailability();
    const bool xessRuntimePresent = Spark::Graphics::UpscalingUtils::DetectXeSSAvailability();

    if (dlssRuntimePresent || xessRuntimePresent)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                       "Vendor upscaler runtime detected (DLSS: %s, XeSS: %s) but no vendor SDK is linked — "
                       "reporting DLSS/XeSS/FSR2 as unavailable",
                       dlssRuntimePresent ? "yes" : "no", xessRuntimePresent ? "yes" : "no");
    }

    m_dlssFeatureInfo.isAvailable = false;
    m_xessFeatureInfo.isAvailable = false;
    m_fsr2Available = false;
}

// =============================================================================
// Non-Windows stubs — GPU methods are no-ops without D3D11
// =============================================================================

#ifndef SPARK_PLATFORM_WINDOWS

bool UpscalingSystem::CreateGPUResources()
{
    // No GPU resources needed on non-Windows platforms
    return true;
}

bool UpscalingSystem::CompileUpscalingShaders()
{
    return true;
}

void UpscalingSystem::RecreateUpscalingResources() {}

void UpscalingSystem::UnbindComputeResources() {}

void UpscalingSystem::ExecuteFSR1(ID3D11ShaderResourceView*, ID3D11UnorderedAccessView*) {}

void UpscalingSystem::ExecuteFSR2(const FSR2DispatchDescription&) {}

void UpscalingSystem::ExecuteDLSS(ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*,
                                  ID3D11ShaderResourceView*, ID3D11UnorderedAccessView*, const XMFLOAT2&, bool)
{
}

void UpscalingSystem::ExecuteXeSS(ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*,
                                  ID3D11ShaderResourceView*, ID3D11UnorderedAccessView*, const XMFLOAT2&)
{
}

void UpscalingSystem::ExecuteSparkSR(ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*,
                                     ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, ID3D11UnorderedAccessView*,
                                     const XMFLOAT2&, bool)
{
}

#endif // !SPARK_PLATFORM_WINDOWS
