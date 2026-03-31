/**
 * @file UpscalingSystem.cpp
 * @brief Upscaling system implementation: FSR 1.0/2.0 HLSL shaders, DLSS/XeSS detection, jitter utilities
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides:
 *   - Inline HLSL compute shader source for FSR 1.0 EASU and RCAS passes
 *   - Inline HLSL compute shader source for a simplified FSR 2.0-style temporal upscaler
 *   - DLSS / XeSS runtime DLL detection helpers
 *   - Halton jitter sequence generation for temporal upscaling
 *   - Resolution calculation utilities
 *
 * The UpscalingSystem class itself is fully inline in the header; this translation
 * unit houses the companion UpscalingUtils namespace and shader source strings that
 * are referenced by the rest of the graphics pipeline.
 */

#include "../Core/Platform.h"
#include "UpscalingSystem.h"
#include "UpscalingShaders.h"
#include "../Utils/Validate.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <algorithm>
#include <numbers>

#ifdef SPARK_PLATFORM_WINDOWS
#pragma comment(lib, "d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
using namespace DirectX;
#endif

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
 * @brief Calculate the optimal render resolution for a given upscaling configuration
 *
 * Returns the internal render resolution that the scene should be rendered at
 * before upscaling to the target display resolution.  Ensures even dimensions
 * for compute shader dispatch alignment.
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
            // FSR 1.0 Constant Buffer Helpers
            // -------------------------------------------------------------------------

#ifdef SPARK_PLATFORM_WINDOWS

            /**
 * @brief Calculate FSR 1.0 EASU constants for the given resolution pair
 *
 * Fills the four float4 constants that the EASU shader requires:
 *   const0: input/output dimensions
 *   const1: input region (offset + size)
 *   const2: reciprocal dimensions
 *   const3: reserved
 *
 * @param inputWidth    Render resolution width
 * @param inputHeight   Render resolution height
 * @param outputWidth   Display resolution width
 * @param outputHeight  Display resolution height
 * @return Populated FSR1EASUConstants structure
 */
            FSR1EASUConstants CalculateEASUConstants(uint32_t inputWidth, uint32_t inputHeight, uint32_t outputWidth,
                                                     uint32_t outputHeight)
            {
                FSR1EASUConstants constants = {};

                float inW = static_cast<float>(inputWidth);
                float inH = static_cast<float>(inputHeight);
                float outW = static_cast<float>(outputWidth);
                float outH = static_cast<float>(outputHeight);

                // const0: (inputWidth, inputHeight, outputWidth, outputHeight)
                constants.const0 = {inW, inH, outW, outH};

                // const1: (inputRegionOffsetX, inputRegionOffsetY, inputRegionWidth, inputRegionHeight)
                // Full image — no sub-region
                constants.const1 = {0.0f, 0.0f, inW, inH};

                // const2: (1/inputWidth, 1/inputHeight, 1/outputWidth, 1/outputHeight)
                constants.const2 = {1.0f / inW, 1.0f / inH, 1.0f / outW, 1.0f / outH};

                // const3: reserved
                constants.const3 = {0.0f, 0.0f, 0.0f, 0.0f};

                return constants;
            }

            /**
 * @brief Calculate FSR 1.0 RCAS constants for a given sharpness level
 *
 * The RCAS pass uses a single float4 constant that controls the sharpening
 * attenuation.  An attenuation of 0 applies maximum sharpening; 2 disables it.
 *
 * @param sharpness  User-facing sharpness value in [0, 1]
 * @return Populated FSR1RCASConstants structure
 */
            FSR1RCASConstants CalculateRCASConstants(float sharpness)
            {
                FSR1RCASConstants constants = {};

                // Map user sharpness [0,1] to RCAS attenuation [2,0]
                // sharpness 0 => attenuation 2 (no sharpening)
                // sharpness 1 => attenuation 0 (max sharpening)
                float attenuation = 2.0f * (1.0f - std::clamp(sharpness, 0.0f, 1.0f));

                constants.const0 = {attenuation, 0.0f, 0.0f, 0.0f};

                return constants;
            }

            /**
 * @brief Compute dispatch group counts for an 8x8 thread group compute shader
 *
 * @param width   Output texture width
 * @param height  Output texture height
 * @return Pair of (groupCountX, groupCountY)
 */
            std::pair<uint32_t, uint32_t> CalculateDispatchGroups(uint32_t width, uint32_t height)
            {
                constexpr uint32_t kThreadGroupSize = 8;
                uint32_t groupCountX = (width + kThreadGroupSize - 1) / kThreadGroupSize;
                uint32_t groupCountY = (height + kThreadGroupSize - 1) / kThreadGroupSize;
                return {groupCountX, groupCountY};
            }

#endif // SPARK_PLATFORM_WINDOWS

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
            // Shader Compilation Helper
            // -------------------------------------------------------------------------

#ifdef SPARK_PLATFORM_WINDOWS

            /**
 * @brief Compile an HLSL compute shader from source string
 *
 * Uses D3DCompile to produce a compute shader blob from inline HLSL.
 * The compiled blob can be used with ID3D11Device::CreateComputeShader.
 *
 * @param device       D3D11 device for shader creation
 * @param hlslSource   Null-terminated HLSL source
 * @param entryPoint   Shader entry point name (e.g. "CSMain")
 * @param outShader    Receives the compiled compute shader
 * @return true on success
 */
            bool CompileComputeShader(ID3D11Device* device, const char* hlslSource, const char* entryPoint,
                                      ID3D11ComputeShader** outShader)
            {
                SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
                SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, device, false);
                SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, hlslSource, false);
                SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, entryPoint, false);
                SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, outShader, false);
                if (!device || !hlslSource || !entryPoint || !outShader)
                {
                    return false;
                }

                ComPtr<ID3DBlob> shaderBlob;
                ComPtr<ID3DBlob> errorBlob;

                UINT compileFlags = 0;
#ifdef _DEBUG
                compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
                compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

                HRESULT hr = D3DCompile(hlslSource, strlen(hlslSource), nullptr, nullptr, nullptr, entryPoint, "cs_5_0",
                                        compileFlags, 0, shaderBlob.GetAddressOf(), errorBlob.GetAddressOf());

                if (FAILED(hr))
                {
                    // Error information available in errorBlob if needed
                    return false;
                }

                hr = device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
                                                 outShader);

                return SUCCEEDED(hr);
            }

            /**
 * @brief Compile and create all FSR 1.0 compute shaders
 *
 * Compiles the EASU and RCAS shaders from their inline HLSL source.
 * Should be called during UpscalingSystem initialization.
 *
 * @param device    D3D11 device
 * @param outEASU   Receives the EASU compute shader
 * @param outRCAS   Receives the RCAS compute shader
 * @return true if both shaders compiled successfully
 */
            bool CreateFSR1Shaders(ID3D11Device* device, ID3D11ComputeShader** outEASU, ID3D11ComputeShader** outRCAS)
            {
                SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
                SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, device, false);
                SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, outEASU, false);
                SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, outRCAS, false);
                if (!device || !outEASU || !outRCAS)
                {
                    return false;
                }

                if (!CompileComputeShader(device, Spark::UpscalingShaders::kFSR1_EASU_CS, "CSMain", outEASU))
                {
                    return false;
                }

                if (!CompileComputeShader(device, Spark::UpscalingShaders::kFSR1_RCAS_CS, "CSMain", outRCAS))
                {
                    if (*outEASU)
                    {
                        (*outEASU)->Release();
                        *outEASU = nullptr;
                    }
                    return false;
                }

                return true;
            }

            /**
 * @brief Compile and create the temporal upscaling compute shader
 *
 * @param device    D3D11 device
 * @param outShader Receives the compiled compute shader
 * @return true on success
 */
            bool CreateTemporalUpscalingShader(ID3D11Device* device, ID3D11ComputeShader** outShader)
            {
                if (!device || !outShader)
                {
                    return false;
                }

                return CompileComputeShader(device, Spark::UpscalingShaders::kTemporalUpscaling_CS, "CSMain",
                                            outShader);
            }

            /**
             * @brief Compile and create the SparkSR temporal upscaling compute shader
             *
             * @param device    D3D11 device
             * @param outShader Receives the compiled compute shader
             * @return true on success
             */
            bool CreateSparkSRShader(ID3D11Device* device, ID3D11ComputeShader** outShader)
            {
                if (!device || !outShader)
                {
                    return false;
                }

                return CompileComputeShader(device, Spark::UpscalingShaders::kSparkSR_CS, "CSMain", outShader);
            }

#endif // SPARK_PLATFORM_WINDOWS

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
 * @return true if mode is temporal (FSR2, DLSS, XeSS)
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
 * @brief Calculate a jitter offset using the recommended phase count for quality
 *
 * Convenience wrapper that selects the appropriate Halton phase count based
 * on the quality preset and returns the pixel-space jitter offset.
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
 * @brief Convert a pixel-space jitter offset to NDC jitter for the projection matrix
 *
 * The projection matrix jitter should be applied as a translation in clip space:
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
