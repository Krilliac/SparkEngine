/**
 * @file PostProcessingPipelineWindows.cpp
 * @brief D3D11 GPU resource creation and effect-shader compilation
 *
 * Split from PostProcessingPipeline.cpp. Contains render target creation and
 * HLSL effect-shader compilation. The inline HLSL sources live in the
 * PostProcessingPipelineWindowsShaders*.h headers; per-pass GPU execution
 * (BeginPass / DrawFullscreen / ProcessPass) lives in
 * PostProcessingPipelineWindowsPasses.cpp.
 *
 * The CPU orchestration (Initialize, Process, Render, metrics, console,
 * volume blending) remains in PostProcessingPipeline.cpp.
 */

#include "PostProcessingPipeline.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "../Core/Platform.h"
#include "../Utils/Validate.h"
#include "PostProcessingPipelineWindowsShadersAO.h"
#include "PostProcessingPipelineWindowsShadersColor.h"
#include "PostProcessingPipelineWindowsShadersFilter.h"
#include "PostProcessingPipelineWindowsShadersLens.h"
#include <d3dcompiler.h>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace Spark::Graphics
{

    bool PostProcessingPipeline::CreatePingPongTargets()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_device)
            return false;

        ComPtr<ID3D11Texture2D> textures[2];
        ComPtr<ID3D11RenderTargetView> rtvs[2];
        ComPtr<ID3D11ShaderResourceView> srvs[2];

        for (int i = 0; i < 2; ++i)
        {
            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width = m_width;
            texDesc.Height = m_height;
            texDesc.MipLevels = 1;
            texDesc.ArraySize = 1;
            texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_DEFAULT;
            texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, textures[i].GetAddressOf());
            if (FAILED(hr))
                return false;

            hr = m_device->CreateRenderTargetView(textures[i].Get(), nullptr, rtvs[i].GetAddressOf());
            if (FAILED(hr))
                return false;

            hr = m_device->CreateShaderResourceView(textures[i].Get(), nullptr, srvs[i].GetAddressOf());
            if (FAILED(hr))
                return false;
        }

        // Commit only after the complete replacement set exists. This keeps a
        // failed resize from leaving a mixture of old and new views.
        for (int i = 0; i < 2; ++i)
        {
            m_pingPongSRVs[i] = std::move(srvs[i]);
            m_pingPongRTVs[i] = std::move(rtvs[i]);
            m_pingPongTextures[i] = std::move(textures[i]);
        }
        return true;
#else
        return false;
#endif
    }

    bool PostProcessingPipeline::CreatePostProcessShaders()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_device)
            return false;

        // Full-screen triangle vertex shader (no vertex buffer needed)
        const char* vsSource = R"(
        struct VSOutput {
            float4 position : SV_Position;
            float2 texCoord : TEXCOORD0;
        };
        VSOutput main(uint vertexID : SV_VertexID) {
            VSOutput output;
            output.texCoord = float2((vertexID << 1) & 2, vertexID & 2);
            output.position = float4(output.texCoord * float2(2, -2) + float2(-1, 1), 0, 1);
            return output;
        }
    )";

        ComPtr<ID3DBlob> vsBlob, errorBlob;
        HRESULT hr = D3DCompile(vsSource, strlen(vsSource), "FullscreenVS", nullptr, nullptr, "main", "vs_5_0", 0, 0,
                                &vsBlob, &errorBlob);
        if (FAILED(hr))
            return false;

        hr =
            m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_fullscreenVS);
        if (FAILED(hr))
            return false;

        // Create constant buffer for post-process parameters
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(PostProcessCB);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
        if (FAILED(hr))
            return false;

        // Create sampler states
        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

        hr = m_device->CreateSamplerState(&sampDesc, &m_linearSampler);
        if (FAILED(hr))
            return false;

        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        hr = m_device->CreateSamplerState(&sampDesc, &m_pointSampler);
        if (FAILED(hr))
            return false;

        CompileEffectShaders();

        return true;
#else
        return false;
#endif
    }

    void PostProcessingPipeline::CompileEffectShaders()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        using namespace PostProcessShadersWin;

        // Compile each effect shader
        struct ShaderDef
        {
            const char* source;
            ComPtr<ID3D11PixelShader>* target;
            const char* name;
        };
        ShaderDef shaders[] = {
            {gtaoPS, &m_gtaoPS, "GTAO"},
            {ssaoTemporalPS, &m_ssaoTemporalPS, "SSAOTemporal"},
            {bloomPS, &m_bloomPS, "Bloom"},
            {autoExposurePS, &m_autoExposurePS, "AutoExposure"},
            {tonemapPS, &m_tonemapPS, "Tonemap"},
            {colorGradingPS, &m_colorGradingPS, "ColorGrading"},
            {fxaaPS, &m_fxaaPS, "FXAA"},
            {dofPS, &m_dofPS, "DOF"},
            {motionBlurPS, &m_motionBlurPS, "MotionBlur"},
            {vignettePS, &m_vignettePS, "Vignette"},
            {chromAbPS, &m_chromAbPS, "ChromAb"},
            {filmGrainPS, &m_filmGrainPS, "FilmGrain"},
            {lensDistPS, &m_lensDistPS, "LensDist"},
            {lightShaftsPS, &m_lightShaftsPS, "LightShafts"},
            {lensFlarePS, &m_lensFlarePS, "LensFlare"},
            {sharpenPS, &m_sharpenPS, "Sharpen"},
        };

        int compiledCount = 0;
        int failedCount = 0;
        for (auto& sd : shaders)
        {
            ComPtr<ID3DBlob> psBlob, errBlob;
            HRESULT hr = D3DCompile(sd.source, strlen(sd.source), sd.name, nullptr, nullptr, "main", "ps_5_0", 0, 0,
                                    &psBlob, &errBlob);
            if (SUCCEEDED(hr))
            {
                hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                                 sd.target->GetAddressOf());
                if (SUCCEEDED(hr))
                {
                    compiledCount++;
                }
                else
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "PostProcess: CreatePixelShader failed for '%s' (HRESULT=0x%08X)", sd.name,
                                    static_cast<unsigned>(hr));
                    failedCount++;
                }
            }
            else
            {
                const char* errMsg = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "unknown error";
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "PostProcess: shader compilation failed for '%s': %s",
                                sd.name, errMsg);
                failedCount++;
            }
        }

        if (failedCount > 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "PostProcess: %d/%d effect shaders failed to compile",
                           failedCount, compiledCount + failedCount);
        }
#endif
    }

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
