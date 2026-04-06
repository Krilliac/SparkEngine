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

#include "UpscalingSystem.h"
#include "../Core/Platform.h"
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

// =============================================================================
// UpscalingSystem — Out-of-line method definitions
// =============================================================================

void UpscalingSystem::DetectFeatures()
{
    // DLSS detection — requires NVIDIA GPU + runtime DLL
    m_dlssFeatureInfo.isAvailable = Spark::Graphics::UpscalingUtils::DetectDLSSAvailability();

    // XeSS detection — works on any DP4a GPU, accelerated on Intel Arc
    m_xessFeatureInfo.isAvailable = Spark::Graphics::UpscalingUtils::DetectXeSSAvailability();

    // FSR 2.0 is a software solution that works on any GPU (no DLL required)
    m_fsr2Available = true;
}

bool UpscalingSystem::CreateGPUResources()
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_device)
    {
        return false;
    }

    // Create constant buffers for FSR 1.0
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbDesc.MiscFlags = 0;

    cbDesc.ByteWidth = sizeof(FSR1EASUConstants);
    HRESULT hr = m_device->CreateBuffer(&cbDesc, nullptr, m_fsr1EASUConstantBuffer.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return false;
    }

    cbDesc.ByteWidth = sizeof(FSR1RCASConstants);
    hr = m_device->CreateBuffer(&cbDesc, nullptr, m_fsr1RCASConstantBuffer.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return false;
    }

    // Create SparkSR constant buffer
    cbDesc.ByteWidth = sizeof(SparkSRConstants);
    hr = m_device->CreateBuffer(&cbDesc, nullptr, m_sparkSRConstantBuffer.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return false;
    }

    // Create linear clamp sampler for upscaling shaders
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = m_device->CreateSamplerState(&samplerDesc, m_linearClampSampler.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return false;
    }

    // Compile upscaling compute shaders
    if (!CompileUpscalingShaders())
    {
        return false;
    }

    // Create intermediate texture at display resolution
    RecreateUpscalingResources();

    return true;
#else
    // No GPU resources needed on non-Windows platforms
    return true;
#endif
}

bool UpscalingSystem::CompileUpscalingShaders()
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_device)
    {
        return false;
    }

    // FSR 1.0 shaders
    if (!Spark::Graphics::UpscalingUtils::CreateFSR1Shaders(m_device, m_fsr1EASUShader.ReleaseAndGetAddressOf(),
                                                            m_fsr1RCASShader.ReleaseAndGetAddressOf()))
    {
        return false;
    }

    // SparkSR temporal shader
    Spark::Graphics::UpscalingUtils::CreateSparkSRShader(m_device, m_sparkSRTemporalCS.ReleaseAndGetAddressOf());

    m_shadersCompiled = true;
    return true;
#else
    return true;
#endif
}

void UpscalingSystem::RecreateUpscalingResources()
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_device || m_displayWidth == 0 || m_displayHeight == 0)
    {
        return;
    }

    // Release existing intermediate resources
    m_intermediateTexture.Reset();
    m_intermediateSRV.Reset();
    m_intermediateUAV.Reset();

    // Create intermediate texture at display resolution for multi-pass upscaling
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_displayWidth;
    texDesc.Height = m_displayHeight;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, m_intermediateTexture.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return;
    }

    m_device->CreateShaderResourceView(m_intermediateTexture.Get(), nullptr,
                                       m_intermediateSRV.ReleaseAndGetAddressOf());
    m_device->CreateUnorderedAccessView(m_intermediateTexture.Get(), nullptr,
                                        m_intermediateUAV.ReleaseAndGetAddressOf());

    // Recreate temporal history texture for FSR2/SparkSR
    m_temporalHistoryTexture.Reset();
    m_temporalHistorySRV.Reset();
    m_temporalHistoryUAV.Reset();

    hr = m_device->CreateTexture2D(&texDesc, nullptr, m_temporalHistoryTexture.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr))
    {
        m_device->CreateShaderResourceView(m_temporalHistoryTexture.Get(), nullptr,
                                           m_temporalHistorySRV.ReleaseAndGetAddressOf());
        m_device->CreateUnorderedAccessView(m_temporalHistoryTexture.Get(), nullptr,
                                            m_temporalHistoryUAV.ReleaseAndGetAddressOf());
    }

    // Recreate lock texture for luminance locking
    m_lockTexture.Reset();
    m_lockSRV.Reset();
    m_lockUAV.Reset();

    texDesc.Format = DXGI_FORMAT_R16_FLOAT;
    hr = m_device->CreateTexture2D(&texDesc, nullptr, m_lockTexture.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr))
    {
        m_device->CreateShaderResourceView(m_lockTexture.Get(), nullptr, m_lockSRV.ReleaseAndGetAddressOf());
        m_device->CreateUnorderedAccessView(m_lockTexture.Get(), nullptr, m_lockUAV.ReleaseAndGetAddressOf());
    }
#endif
}

void UpscalingSystem::UnbindComputeResources()
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_context)
    {
        return;
    }

    ID3D11ShaderResourceView* nullSRVs[4] = {nullptr, nullptr, nullptr, nullptr};
    ID3D11UnorderedAccessView* nullUAVs[2] = {nullptr, nullptr};
    ID3D11Buffer* nullCBs[1] = {nullptr};
    ID3D11SamplerState* nullSamplers[1] = {nullptr};

    m_context->CSSetShaderResources(0, 4, nullSRVs);
    m_context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, nullCBs);
    m_context->CSSetSamplers(0, 1, nullSamplers);
    m_context->CSSetShader(nullptr, nullptr, 0);
#endif
}

// =============================================================================
// Execute methods — per-backend upscaling dispatch
// =============================================================================

// Intentional: params used only inside #ifdef SPARK_PLATFORM_WINDOWS
void UpscalingSystem::ExecuteFSR1([[maybe_unused]] ID3D11ShaderResourceView* inputColorSRV,
                                  [[maybe_unused]] ID3D11UnorderedAccessView* outputUAV)
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_context || !m_fsr1EASUShader || !m_fsr1RCASShader || !inputColorSRV || !outputUAV)
    {
        return;
    }

    // Calculate EASU constants
    auto easuConst = Spark::Graphics::UpscalingUtils::CalculateEASUConstants(m_renderWidth, m_renderHeight,
                                                                             m_displayWidth, m_displayHeight);
    auto rcasConst = Spark::Graphics::UpscalingUtils::CalculateRCASConstants(m_settings.sharpness);
    UpdateFSR1Constants(&easuConst, &rcasConst);

    auto [groupX, groupY] = Spark::Graphics::UpscalingUtils::CalculateDispatchGroups(m_displayWidth, m_displayHeight);

    // Pass 1: EASU — edge-adaptive spatial upsampling (input → intermediate)
    m_context->CSSetShader(m_fsr1EASUShader.Get(), nullptr, 0);
    m_context->CSSetShaderResources(0, 1, &inputColorSRV);
    ID3D11UnorderedAccessView* intermediateUAV = m_intermediateUAV.Get();
    m_context->CSSetUnorderedAccessViews(0, 1, &intermediateUAV, nullptr);
    ID3D11Buffer* easuCB = m_fsr1EASUConstantBuffer.Get();
    m_context->CSSetConstantBuffers(0, 1, &easuCB);
    ID3D11SamplerState* sampler = m_linearClampSampler.Get();
    m_context->CSSetSamplers(0, 1, &sampler);
    m_context->Dispatch(groupX, groupY, 1);

    // Unbind between passes
    UnbindComputeResources();

    // Pass 2: RCAS — robust contrast-adaptive sharpening (intermediate → output)
    m_context->CSSetShader(m_fsr1RCASShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* intermediateSRV = m_intermediateSRV.Get();
    m_context->CSSetShaderResources(0, 1, &intermediateSRV);
    m_context->CSSetUnorderedAccessViews(0, 1, &outputUAV, nullptr);
    ID3D11Buffer* rcasCB = m_fsr1RCASConstantBuffer.Get();
    m_context->CSSetConstantBuffers(0, 1, &rcasCB);
    m_context->CSSetSamplers(0, 1, &sampler);
    m_context->Dispatch(groupX, groupY, 1);

    UnbindComputeResources();
#endif
}

void UpscalingSystem::ExecuteFSR2(const FSR2DispatchDescription& desc)
{
#ifdef SPARK_PLATFORM_WINDOWS
    // FSR 2.0 requires the FidelityFX SDK which is not yet linked.
    // Fall through to the built-in temporal upscaler as a placeholder.
    if (!m_context || !desc.colorSRV || !desc.outputUAV)
    {
        return;
    }

    // Use SparkSR temporal path as FSR2 fallback
    ExecuteSparkSR(desc.colorSRV, desc.depthSRV, desc.motionVectorsSRV, desc.exposureSRV, desc.reactiveMaskSRV,
                   desc.outputUAV, desc.jitterOffset, desc.resetAccumulation);
#endif
}

// Intentional: params used only inside #ifdef SPARK_PLATFORM_WINDOWS
void UpscalingSystem::ExecuteDLSS([[maybe_unused]] ID3D11ShaderResourceView* colorSRV,
                                  [[maybe_unused]] ID3D11ShaderResourceView* depthSRV,
                                  [[maybe_unused]] ID3D11ShaderResourceView* motionVectorsSRV,
                                  [[maybe_unused]] ID3D11ShaderResourceView* exposureSRV,
                                  [[maybe_unused]] ID3D11UnorderedAccessView* outputUAV,
                                  [[maybe_unused]] const XMFLOAT2& jitterOffset, [[maybe_unused]] bool resetHistory)
{
#ifdef SPARK_PLATFORM_WINDOWS
    // DLSS requires the NVIDIA NGX SDK which is not yet linked.
    // Fall through to SparkSR temporal upscaler as a placeholder.
    if (!m_context || !colorSRV || !outputUAV)
    {
        return;
    }

    ExecuteSparkSR(colorSRV, depthSRV, motionVectorsSRV, exposureSRV, nullptr, outputUAV, jitterOffset, resetHistory);
#endif
}

// Intentional: params used only inside #ifdef SPARK_PLATFORM_WINDOWS
void UpscalingSystem::ExecuteXeSS([[maybe_unused]] ID3D11ShaderResourceView* colorSRV,
                                  [[maybe_unused]] ID3D11ShaderResourceView* depthSRV,
                                  [[maybe_unused]] ID3D11ShaderResourceView* motionVectorsSRV,
                                  [[maybe_unused]] ID3D11ShaderResourceView* exposureSRV,
                                  [[maybe_unused]] ID3D11UnorderedAccessView* outputUAV,
                                  [[maybe_unused]] const XMFLOAT2& jitterOffset)
{
#ifdef SPARK_PLATFORM_WINDOWS
    // XeSS requires the Intel XeSS SDK which is not yet linked.
    // Fall through to SparkSR temporal upscaler as a placeholder.
    if (!m_context || !colorSRV || !outputUAV)
    {
        return;
    }

    ExecuteSparkSR(colorSRV, depthSRV, motionVectorsSRV, exposureSRV, nullptr, outputUAV, jitterOffset);
#endif
}

// Intentional: params used only inside #ifdef SPARK_PLATFORM_WINDOWS
void UpscalingSystem::ExecuteSparkSR([[maybe_unused]] ID3D11ShaderResourceView* colorSRV,
                                     [[maybe_unused]] ID3D11ShaderResourceView* depthSRV,
                                     [[maybe_unused]] ID3D11ShaderResourceView* motionVectorsSRV,
                                     [[maybe_unused]] ID3D11ShaderResourceView* exposureSRV,
                                     [[maybe_unused]] ID3D11ShaderResourceView* reactiveMaskSRV,
                                     [[maybe_unused]] ID3D11UnorderedAccessView* outputUAV,
                                     [[maybe_unused]] const XMFLOAT2& jitterOffset, [[maybe_unused]] bool resetHistory)
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_context || !m_sparkSRTemporalCS || !colorSRV || !outputUAV)
    {
        return;
    }

    // Fill SparkSR constant buffer
    float rW = static_cast<float>(m_renderWidth);
    float rH = static_cast<float>(m_renderHeight);
    float dW = static_cast<float>(m_displayWidth);
    float dH = static_cast<float>(m_displayHeight);

    SparkSRConstants constants = {};
    constants.renderSize = {rW, rH, 1.0f / rW, 1.0f / rH};
    constants.displaySize = {dW, dH, 1.0f / dW, 1.0f / dH};
    constants.jitterOffset = {jitterOffset.x, jitterOffset.y, m_prevJitterX, m_prevJitterY};

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_context->Map(m_sparkSRConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr) && mapped.pData)
    {
        memcpy(mapped.pData, &constants, sizeof(SparkSRConstants));
        m_context->Unmap(m_sparkSRConstantBuffer.Get(), 0);
    }

    m_prevJitterX = jitterOffset.x;
    m_prevJitterY = jitterOffset.y;
    ++m_sparkSRFrameIndex;

    // Bind resources and dispatch temporal upscaling
    m_context->CSSetShader(m_sparkSRTemporalCS.Get(), nullptr, 0);

    ID3D11ShaderResourceView* srvs[4] = {colorSRV, depthSRV, motionVectorsSRV, m_temporalHistorySRV.Get()};
    m_context->CSSetShaderResources(0, 4, srvs);

    ID3D11UnorderedAccessView* uavs[2] = {outputUAV, m_temporalHistoryUAV.Get()};
    m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

    ID3D11Buffer* cb = m_sparkSRConstantBuffer.Get();
    m_context->CSSetConstantBuffers(0, 1, &cb);

    ID3D11SamplerState* sampler = m_linearClampSampler.Get();
    m_context->CSSetSamplers(0, 1, &sampler);

    auto [groupX, groupY] = Spark::Graphics::UpscalingUtils::CalculateDispatchGroups(m_displayWidth, m_displayHeight);
    m_context->Dispatch(groupX, groupY, 1);

    UnbindComputeResources();

    // Apply RCAS sharpening pass on the output if sharpness > 0
    if (m_settings.sharpness > 0.0f && m_fsr1RCASShader)
    {
        auto rcasConst = Spark::Graphics::UpscalingUtils::CalculateRCASConstants(m_settings.sharpness);
        UpdateFSR1Constants(nullptr, &rcasConst);

        // Copy output to intermediate for sharpening input
        m_context->CopyResource(m_intermediateTexture.Get(), nullptr);

        m_context->CSSetShader(m_fsr1RCASShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* sharpSRV = m_intermediateSRV.Get();
        m_context->CSSetShaderResources(0, 1, &sharpSRV);
        m_context->CSSetUnorderedAccessViews(0, 1, &outputUAV, nullptr);
        ID3D11Buffer* rcasCB = m_fsr1RCASConstantBuffer.Get();
        m_context->CSSetConstantBuffers(0, 1, &rcasCB);
        m_context->CSSetSamplers(0, 1, &sampler);
        m_context->Dispatch(groupX, groupY, 1);

        UnbindComputeResources();
    }
#endif
}
