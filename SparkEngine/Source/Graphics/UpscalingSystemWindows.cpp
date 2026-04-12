/**
 * @file UpscalingSystemWindows.cpp
 * @brief GPU-side upscaling implementation: D3D11 resource creation, shader compilation, execute dispatches
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains all methods that require D3D11/Windows headers:
 *   - FSR 1.0 constant buffer helpers (EASU/RCAS)
 *   - Compute shader dispatch group calculation
 *   - Shader compilation (D3DCompile)
 *   - GPU resource creation / recreation / unbind
 *   - Per-backend Execute methods (FSR1, FSR2, DLSS, XeSS, SparkSR)
 *
 * The companion UpscalingSystem.cpp houses cross-platform utilities
 * (Halton jitter, DLL detection, resolution calculation, mode/quality names).
 */

#include "UpscalingSystem.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "UpscalingShaders.h"
#include "../Utils/Validate.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#pragma comment(lib, "d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
using namespace DirectX;

// =============================================================================
// UPSCALING UTILITIES — Windows / D3D11 helpers
// =============================================================================

namespace Spark
{
    namespace Graphics
    {
        namespace UpscalingUtils
        {

            // -------------------------------------------------------------------------
            // FSR 1.0 Constant Buffer Helpers
            // -------------------------------------------------------------------------

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

            // -------------------------------------------------------------------------
            // Shader Compilation Helper
            // -------------------------------------------------------------------------

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

        } // namespace UpscalingUtils
    } // namespace Graphics
} // namespace Spark

// =============================================================================
// UpscalingSystem — GPU resource management
// =============================================================================

bool UpscalingSystem::CreateGPUResources()
{
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
}

bool UpscalingSystem::CompileUpscalingShaders()
{
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
}

void UpscalingSystem::RecreateUpscalingResources()
{
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
}

void UpscalingSystem::UnbindComputeResources()
{
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
}

// =============================================================================
// Execute methods — per-backend upscaling dispatch
// =============================================================================

void UpscalingSystem::ExecuteFSR1(ID3D11ShaderResourceView* inputColorSRV, ID3D11UnorderedAccessView* outputUAV)
{
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

    // Pass 1: EASU — edge-adaptive spatial upsampling (input -> intermediate)
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

    // Pass 2: RCAS — robust contrast-adaptive sharpening (intermediate -> output)
    m_context->CSSetShader(m_fsr1RCASShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* intermediateSRV = m_intermediateSRV.Get();
    m_context->CSSetShaderResources(0, 1, &intermediateSRV);
    m_context->CSSetUnorderedAccessViews(0, 1, &outputUAV, nullptr);
    ID3D11Buffer* rcasCB = m_fsr1RCASConstantBuffer.Get();
    m_context->CSSetConstantBuffers(0, 1, &rcasCB);
    m_context->CSSetSamplers(0, 1, &sampler);
    m_context->Dispatch(groupX, groupY, 1);

    UnbindComputeResources();
}

void UpscalingSystem::ExecuteFSR2(const FSR2DispatchDescription& desc)
{
    // FSR 2.0 requires the FidelityFX SDK which is not yet linked.
    // Fall through to the built-in temporal upscaler as a placeholder.
    if (!m_context || !desc.colorSRV || !desc.outputUAV)
    {
        return;
    }

    // Use SparkSR temporal path as FSR2 fallback
    ExecuteSparkSR(desc.colorSRV, desc.depthSRV, desc.motionVectorsSRV, desc.exposureSRV, desc.reactiveMaskSRV,
                   desc.outputUAV, desc.jitterOffset, desc.resetAccumulation);
}

void UpscalingSystem::ExecuteDLSS(ID3D11ShaderResourceView* colorSRV, ID3D11ShaderResourceView* depthSRV,
                                  ID3D11ShaderResourceView* motionVectorsSRV, ID3D11ShaderResourceView* exposureSRV,
                                  ID3D11UnorderedAccessView* outputUAV, const XMFLOAT2& jitterOffset, bool resetHistory)
{
    // DLSS requires the NVIDIA NGX SDK which is not yet linked.
    // Fall through to SparkSR temporal upscaler as a placeholder.
    if (!m_context || !colorSRV || !outputUAV)
    {
        return;
    }

    ExecuteSparkSR(colorSRV, depthSRV, motionVectorsSRV, exposureSRV, nullptr, outputUAV, jitterOffset, resetHistory);
}

void UpscalingSystem::ExecuteXeSS(ID3D11ShaderResourceView* colorSRV, ID3D11ShaderResourceView* depthSRV,
                                  ID3D11ShaderResourceView* motionVectorsSRV, ID3D11ShaderResourceView* exposureSRV,
                                  ID3D11UnorderedAccessView* outputUAV, const XMFLOAT2& jitterOffset)
{
    // XeSS requires the Intel XeSS SDK which is not yet linked.
    // Fall through to SparkSR temporal upscaler as a placeholder.
    if (!m_context || !colorSRV || !outputUAV)
    {
        return;
    }

    ExecuteSparkSR(colorSRV, depthSRV, motionVectorsSRV, exposureSRV, nullptr, outputUAV, jitterOffset);
}

void UpscalingSystem::ExecuteSparkSR(ID3D11ShaderResourceView* colorSRV, ID3D11ShaderResourceView* depthSRV,
                                     ID3D11ShaderResourceView* motionVectorsSRV, ID3D11ShaderResourceView* exposureSRV,
                                     ID3D11ShaderResourceView* reactiveMaskSRV, ID3D11UnorderedAccessView* outputUAV,
                                     const XMFLOAT2& jitterOffset, bool resetHistory)
{
    (void)exposureSRV;
    (void)reactiveMaskSRV;
    (void)resetHistory;

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
}

#endif // SPARK_PLATFORM_WINDOWS
