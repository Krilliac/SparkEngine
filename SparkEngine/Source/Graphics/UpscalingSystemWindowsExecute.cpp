/**
 * @file UpscalingSystemWindowsExecute.cpp
 * @brief Per-backend upscaling Execute dispatches (FSR1, FSR2, DLSS, XeSS, SparkSR)
 * @author Spark Engine Team
 * @date 2026
 *
 * Execute methods split out of UpscalingSystemWindows.cpp (which keeps the
 * UpscalingUtils helpers and GPU resource creation / recreation / unbind).
 * Shared UpscalingUtils declarations live in UpscalingSystemWindowsInternal.h.
 */

#include "UpscalingSystem.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "UpscalingSystemWindowsInternal.h"

#include <d3d11.h>
#include "Core/Platform.h"
#include <windows.h>

#include <cstdint>
#include <cstring>

using namespace DirectX;

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
    if (m_settings.sharpness > 0.0f && m_fsr1RCASShader && m_intermediateTexture)
    {
        // RCAS reads its input through the intermediate SRV, so the temporal
        // result is copied there first. Resolve the texture behind the output
        // UAV; without it there is nothing to sharpen.
        Microsoft::WRL::ComPtr<ID3D11Resource> outputResource;
        outputUAV->GetResource(outputResource.GetAddressOf());
        if (!outputResource)
        {
            return;
        }

        auto rcasConst = Spark::Graphics::UpscalingUtils::CalculateRCASConstants(m_settings.sharpness);
        UpdateFSR1Constants(nullptr, &rcasConst);

        // Copy output to intermediate for sharpening input
        m_context->CopyResource(m_intermediateTexture.Get(), outputResource.Get());

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
