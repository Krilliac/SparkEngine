/**
 * @file PostProcessingPipeline.h
 * @brief Configurable post-processing effect chain with ordered passes
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a flexible pipeline for chaining post-processing effects in a
 * configurable order. Each effect is a self-contained pass with its own
 * settings, enable/disable state, and performance metrics. The pipeline
 * manages render target ping-ponging between passes.
 *
 * ## Usage
 * @code
 *   PostProcessingPipeline pipeline;
 *   pipeline.Initialize(1920, 1080);
 *
 *   pipeline.SetEffectEnabled(PostProcessPass::Vignette, true);
 *   pipeline.GetVignetteSettings().intensity = 0.4f;
 *
 *   pipeline.SetEffectEnabled(PostProcessPass::DepthOfField, true);
 *   pipeline.GetDOFSettings().focalDistance = 15.0f;
 *
 *   // Each frame after scene rendering
 *   pipeline.Process(deltaTime);
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace Spark::Graphics
{

    // =============================================================================
    // Post-Processing Pass Types
    // =============================================================================

    /**
 * @brief Individual post-processing passes in render order
 */
    enum class PostProcessPass
    {
        FXAA,                ///< Fast Approximate Anti-Aliasing
        DepthOfField,        ///< Bokeh depth of field
        MotionBlur,          ///< Per-pixel motion blur (uses TemporalEffects data)
        Vignette,            ///< Screen-edge darkening
        ChromaticAberration, ///< RGB channel separation
        FilmGrain,           ///< Cinematic film grain noise
        LensDistortion,      ///< Barrel/pincushion distortion
        LightShafts,         ///< God rays / volumetric light shafts
        LensFlare,           ///< Lens flare from bright light sources
        Sharpen,             ///< Contrast-adaptive sharpening
        Count
    };

    // =============================================================================
    // Effect Settings
    // =============================================================================

    /**
 * @brief FXAA (Fast Approximate Anti-Aliasing) settings
 */
    struct FXAASettings
    {
        bool enabled = false;
        float edgeThreshold = 0.166f;     ///< Min luminance edge detection [0.063, 0.333]
        float edgeThresholdMin = 0.0833f; ///< Darkest edge threshold
        float subpixelQuality = 0.75f;    ///< Sub-pixel AA quality [0=off, 1=max]
        int qualityPreset = 12;           ///< Quality iterations [10=low, 29=ultra]
    };

    /**
 * @brief Depth of Field settings
 */
    struct DepthOfFieldSettings
    {
        bool enabled = false;
        float focalDistance = 10.0f;  ///< Distance to focus plane in meters
        float focalLength = 50.0f;    ///< Lens focal length in mm
        float aperture = 2.8f;        ///< F-stop aperture (lower = more blur)
        float nearBlurStart = 0.5f;   ///< Near blur start distance
        float nearBlurEnd = 2.0f;     ///< Near blur full distance
        float farBlurStart = 20.0f;   ///< Far blur start distance
        float farBlurEnd = 100.0f;    ///< Far blur full distance
        float maxBokehSize = 8.0f;    ///< Maximum bokeh circle diameter in pixels
        int blurSamples = 16;         ///< Samples for blur kernel
        bool useCircularBokeh = true; ///< Circular vs hexagonal bokeh shape
        float bokehBrightness = 1.0f; ///< Brightness threshold for bokeh highlights
    };

    /**
 * @brief Vignette effect settings
 */
    struct VignetteSettings
    {
        bool enabled = false;
        float intensity = 0.3f;              ///< Darkening intensity [0, 1]
        float smoothness = 0.5f;             ///< Edge softness [0, 1]
        float roundness = 1.0f;              ///< Shape (1=circular, 0=rectangular)
        XMFLOAT3 color = {0.0f, 0.0f, 0.0f}; ///< Vignette color (default: black)
        XMFLOAT2 center = {0.5f, 0.5f};      ///< Vignette center in UV space
    };

    /**
 * @brief Chromatic aberration settings
 */
    struct ChromaticAberrationSettings
    {
        bool enabled = false;
        float intensity = 0.5f;                        ///< Separation amount [0, 3]
        float radialFalloff = 1.0f;                    ///< Stronger at edges [0=uniform, 2=strong edge]
        XMFLOAT3 channelOffsets = {1.0f, 0.0f, -1.0f}; ///< R, G, B offset multipliers
    };

    /**
 * @brief Film grain effect settings
 */
    struct FilmGrainSettings
    {
        bool enabled = false;
        float intensity = 0.15f;            ///< Grain visibility [0, 1]
        float size = 1.6f;                  ///< Grain particle size
        float speed = 1.0f;                 ///< Animation speed
        float luminanceContribution = 0.8f; ///< How much luminance affects grain [0, 1]
        bool colored = false;               ///< Color noise vs monochrome
    };

    /**
 * @brief Lens distortion settings
 */
    struct LensDistortionSettings
    {
        bool enabled = false;
        float barrelDistortion = 0.0f;  ///< Barrel/pincushion [-1=pincushion, 1=barrel]
        float zoomCompensation = 1.0f;  ///< Zoom to compensate for distortion
        XMFLOAT2 center = {0.5f, 0.5f}; ///< Distortion center in UV space
        float cubicDistortion = 0.0f;   ///< Higher-order distortion term
    };

    /**
 * @brief Light shafts (god rays) settings
 */
    struct LightShaftSettings
    {
        bool enabled = false;
        XMFLOAT2 lightScreenPos = {0.5f, 0.3f}; ///< Light source screen position
        float density = 1.0f;                   ///< Ray density [0, 1]
        float weight = 0.01f;                   ///< Intensity per sample
        float decay = 0.97f;                    ///< Intensity decay per step [0, 1]
        float exposure = 1.0f;                  ///< Final exposure multiplier
        int sampleCount = 64;                   ///< Ray marching samples
        XMFLOAT3 color = {1.0f, 0.95f, 0.8f};   ///< Shaft color
    };

    /**
 * @brief Lens flare settings
 */
    struct LensFlareSettings
    {
        bool enabled = false;
        float threshold = 0.8f;           ///< Brightness threshold for flare trigger
        float intensity = 0.5f;           ///< Flare overall intensity
        int ghostCount = 5;               ///< Number of ghost images
        float ghostSpacing = 0.3f;        ///< Distance between ghosts
        float ghostThreshold = 10.0f;     ///< Brightness for ghost generation
        float haloRadius = 0.6f;          ///< Halo ring radius
        float haloThickness = 0.1f;       ///< Halo ring width
        float chromaticDistortion = 2.5f; ///< Color separation in flare
    };

    /**
 * @brief Contrast-adaptive sharpening settings
 */
    struct SharpenSettings
    {
        bool enabled = false;
        float amount = 0.5f;            ///< Sharpening strength [0, 1]
        float threshold = 0.05f;        ///< Edge threshold to avoid noise amplification
        bool adaptiveSharpening = true; ///< CAS (AMD FidelityFX style)
    };

    // =============================================================================
    // Pass Metrics
    // =============================================================================

    /**
 * @brief Performance metrics for a single post-processing pass
 */
    struct PassMetrics
    {
        std::string name;
        float timeMs = 0.0f; ///< GPU time in milliseconds
        bool isEnabled = false;
    };

    // =============================================================================
    // Post-Processing Pipeline
    // =============================================================================

    /**
 * @class PostProcessingPipeline
 * @brief Manages an ordered chain of post-processing effects
 *
 * Effects are processed in a fixed order (the PostProcessPass enum order).
 * Each effect can be independently enabled/disabled. The pipeline handles
 * render target ping-ponging automatically.
 */
    class PostProcessingPipeline
    {
      public:
        PostProcessingPipeline() = default;
        ~PostProcessingPipeline() = default;

        /**
     * @brief Initialize the pipeline with render dimensions
     */
        bool Initialize(uint32_t width = 1920, uint32_t height = 1080)
        {
            m_width = width;
            m_height = height;

            if (m_device)
            {
                if (!CreatePingPongTargets())
                    return false;
                if (!CreatePostProcessShaders())
                    return false;
            }

            m_initialized = true;
            return true;
        }

        /** @brief Shutdown and release all resources */
        void Shutdown()
        {
            for (int i = 0; i < 2; ++i)
            {
                m_pingPongTextures[i].Reset();
                m_pingPongRTVs[i] = nullptr;
                m_pingPongSRVs[i] = nullptr;
            }
            m_fullscreenVS.Reset();
            m_fxaaPS.Reset();
            m_dofPS.Reset();
            m_motionBlurPS.Reset();
            m_vignettePS.Reset();
            m_chromAbPS.Reset();
            m_filmGrainPS.Reset();
            m_lensDistPS.Reset();
            m_lightShaftsPS.Reset();
            m_lensFlarePS.Reset();
            m_sharpenPS.Reset();
            m_constantBuffer.Reset();
            m_linearSampler.Reset();
            m_pointSampler.Reset();
            m_initialized = false;
        }

        /**
     * @brief Process all enabled effects in order
     * @param deltaTime Frame delta time for animated effects
     */
        void Process(float deltaTime = 0.0f)
        {
            if (!m_initialized)
                return;
            m_totalTime += deltaTime;
            m_activePassCount = 0;

            for (int i = 0; i < static_cast<int>(PostProcessPass::Count); ++i)
            {
                auto pass = static_cast<PostProcessPass>(i);
                if (IsEffectEnabled(pass))
                {
                    ProcessPass(pass, deltaTime);
                    m_activePassCount++;
                }
            }
        }

        /** @brief Render the final result to the currently bound render target */
        void Render()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            if (!m_initialized || !m_context || !m_fullscreenVS)
                return;

            // Copy the final result from the ping-pong target to the bound output
            int src = GetSourceTarget();
            if (m_pingPongSRVs[src])
            {
                // Draw fullscreen pass to copy result
                PostProcessCB cb = {};
                cb.params1 = {static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 0.0f};

                // Simple passthrough shader (use sharpen with amount=0 as passthrough)
                cb.params0 = {0.0f, 0.0f, 0.0f, 0.0f};
                if (m_sharpenPS)
                {
                    m_context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
                    m_context->PSSetShader(m_sharpenPS.Get(), nullptr, 0);

                    D3D11_MAPPED_SUBRESOURCE mapped;
                    if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                    {
                        memcpy(mapped.pData, &cb, sizeof(PostProcessCB));
                        m_context->Unmap(m_constantBuffer.Get(), 0);
                    }
                    m_context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
                    m_context->PSSetShaderResources(0, 1, &m_pingPongSRVs[src]);
                    ID3D11SamplerState* samplers[] = {m_linearSampler.Get(), m_pointSampler.Get()};
                    m_context->PSSetSamplers(0, 2, samplers);
                    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    m_context->IASetInputLayout(nullptr);
                    m_context->Draw(3, 0);
                }
            }
#endif
        }

        /**
     * @brief Handle viewport resize
     */
        void Resize(uint32_t width, uint32_t height)
        {
            m_width = width;
            m_height = height;
        }

        // ---- Effect Enable/Disable ----

        void SetEffectEnabled(PostProcessPass pass, bool enabled) { m_passEnabled[static_cast<int>(pass)] = enabled; }

        bool IsEffectEnabled(PostProcessPass pass) const { return m_passEnabled[static_cast<int>(pass)]; }

        int GetActivePassCount() const { return m_activePassCount; }

        // ---- Settings Accessors ----

        FXAASettings& GetFXAASettings() { return m_fxaaSettings; }
        const FXAASettings& GetFXAASettings() const { return m_fxaaSettings; }

        DepthOfFieldSettings& GetDOFSettings() { return m_dofSettings; }
        const DepthOfFieldSettings& GetDOFSettings() const { return m_dofSettings; }

        VignetteSettings& GetVignetteSettings() { return m_vignetteSettings; }
        const VignetteSettings& GetVignetteSettings() const { return m_vignetteSettings; }

        ChromaticAberrationSettings& GetChromaticAberrationSettings() { return m_chromAbSettings; }
        const ChromaticAberrationSettings& GetChromaticAberrationSettings() const { return m_chromAbSettings; }

        FilmGrainSettings& GetFilmGrainSettings() { return m_filmGrainSettings; }
        const FilmGrainSettings& GetFilmGrainSettings() const { return m_filmGrainSettings; }

        LensDistortionSettings& GetLensDistortionSettings() { return m_lensDistSettings; }
        const LensDistortionSettings& GetLensDistortionSettings() const { return m_lensDistSettings; }

        LightShaftSettings& GetLightShaftSettings() { return m_lightShaftSettings; }
        const LightShaftSettings& GetLightShaftSettings() const { return m_lightShaftSettings; }

        LensFlareSettings& GetLensFlareSettings() { return m_lensFlareSettings; }
        const LensFlareSettings& GetLensFlareSettings() const { return m_lensFlareSettings; }

        SharpenSettings& GetSharpenSettings() { return m_sharpenSettings; }
        const SharpenSettings& GetSharpenSettings() const { return m_sharpenSettings; }

        // ---- Metrics ----

        std::vector<PassMetrics> GetPassMetrics() const
        {
            std::vector<PassMetrics> metrics;
            static const char* passNames[] = {
                "FXAA",      "DepthOfField",   "MotionBlur",  "Vignette",  "ChromaticAberration",
                "FilmGrain", "LensDistortion", "LightShafts", "LensFlare", "Sharpen"};
            for (int i = 0; i < static_cast<int>(PostProcessPass::Count); ++i)
            {
                PassMetrics pm;
                pm.name = passNames[i];
                pm.isEnabled = m_passEnabled[i];
                pm.timeMs = m_passTimings[i];
                metrics.push_back(pm);
            }
            return metrics;
        }

        std::string Console_ListEffects() const
        {
            std::string result = "Post-Processing Pipeline:\n";
            auto metrics = GetPassMetrics();
            for (const auto& pm : metrics)
            {
                result += "  " + pm.name + ": " + (pm.isEnabled ? "ON" : "OFF") + "\n";
            }
            result += "Active: " + std::to_string(m_activePassCount) + "\n";
            return result;
        }

        /** @brief Set the D3D11 device and context for GPU execution */
        void SetDevice(ID3D11Device* device, ID3D11DeviceContext* context)
        {
            m_device = device;
            m_context = context;
        }

        /** @brief Set the scene depth SRV for depth-aware effects */
        void SetDepthSRV(ID3D11ShaderResourceView* depthSRV) { m_depthSRV = depthSRV; }

        /** @brief Set the input scene texture SRV */
        void SetInputSRV(ID3D11ShaderResourceView* inputSRV) { m_inputSRV = inputSRV; }

        /** @brief Get the output render target view after processing */
        ID3D11RenderTargetView* GetOutputRTV() const { return m_pingPongRTVs[m_currentTarget]; }

        /** @brief Get the output SRV after processing */
        ID3D11ShaderResourceView* GetOutputSRV() const { return m_pingPongSRVs[m_currentTarget]; }

      private:
        // ---- GPU Constant Buffer ----
        struct PostProcessCB
        {
            XMFLOAT4 params0;
            XMFLOAT4 params1;
            XMFLOAT4 params2;
            XMFLOAT4 params3;
        };

        // ---- GPU Resource Creation ----

        bool CreatePingPongTargets()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            if (!m_device)
                return false;

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

                HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_pingPongTextures[i]);
                if (FAILED(hr))
                    return false;

                hr = m_device->CreateRenderTargetView(m_pingPongTextures[i], nullptr, &m_pingPongRTVs[i]);
                if (FAILED(hr))
                    return false;

                hr = m_device->CreateShaderResourceView(m_pingPongTextures[i], nullptr, &m_pingPongSRVs[i]);
                if (FAILED(hr))
                    return false;
            }
            return true;
#else
            return false;
#endif
        }

        bool CreatePostProcessShaders()
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
            HRESULT hr = D3DCompile(vsSource, strlen(vsSource), "FullscreenVS", nullptr, nullptr, "main", "vs_5_0", 0,
                                    0, &vsBlob, &errorBlob);
            if (FAILED(hr))
                return false;

            hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                              &m_fullscreenVS);
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

            // Compile all effect pixel shaders
            CompileEffectShaders();

            return true;
#else
            return false;
#endif
        }

        void CompileEffectShaders()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            // FXAA pixel shader
            const char* fxaaPS = R"(
                Texture2D sceneTexture : register(t0);
                SamplerState linearSampler : register(s0);
                cbuffer PostProcessParams : register(b0) {
                    float4 params0; // x=edgeThreshold, y=edgeThresholdMin, z=subpixelQuality, w=unused
                    float4 params1; // x=screenWidth, y=screenHeight, z=time, w=intensity
                    float4 params2;
                    float4 params3;
                };
                float Luminance(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
                float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                    float2 texelSize = 1.0 / float2(params1.x, params1.y);
                    float3 rgbM = sceneTexture.Sample(linearSampler, uv).rgb;
                    float3 rgbNW = sceneTexture.Sample(linearSampler, uv + float2(-1, -1) * texelSize).rgb;
                    float3 rgbNE = sceneTexture.Sample(linearSampler, uv + float2(1, -1) * texelSize).rgb;
                    float3 rgbSW = sceneTexture.Sample(linearSampler, uv + float2(-1, 1) * texelSize).rgb;
                    float3 rgbSE = sceneTexture.Sample(linearSampler, uv + float2(1, 1) * texelSize).rgb;
                    float lumM = Luminance(rgbM);
                    float lumNW = Luminance(rgbNW); float lumNE = Luminance(rgbNE);
                    float lumSW = Luminance(rgbSW); float lumSE = Luminance(rgbSE);
                    float lumMin = min(lumM, min(min(lumNW, lumNE), min(lumSW, lumSE)));
                    float lumMax = max(lumM, max(max(lumNW, lumNE), max(lumSW, lumSE)));
                    float lumRange = lumMax - lumMin;
                    if (lumRange < max(params0.y, lumMax * params0.x))
                        return float4(rgbM, 1);
                    float2 dir;
                    dir.x = -((lumNW + lumNE) - (lumSW + lumSE));
                    dir.y = ((lumNW + lumSW) - (lumNE + lumSE));
                    float dirReduce = max((lumNW + lumNE + lumSW + lumSE) * 0.25 * 0.25, 1.0/128.0);
                    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
                    dir = clamp(dir * rcpDirMin, -8.0, 8.0) * texelSize;
                    float3 rgbA = 0.5 * (sceneTexture.Sample(linearSampler, uv + dir * (1.0/3.0 - 0.5)).rgb +
                                          sceneTexture.Sample(linearSampler, uv + dir * (2.0/3.0 - 0.5)).rgb);
                    float3 rgbB = rgbA * 0.5 + 0.25 * (sceneTexture.Sample(linearSampler, uv + dir * -0.5).rgb +
                                                         sceneTexture.Sample(linearSampler, uv + dir * 0.5).rgb);
                    float lumB = Luminance(rgbB);
                    float3 result = (lumB < lumMin || lumB > lumMax) ? rgbA : rgbB;
                    return float4(result, 1);
                }
            )";

            // Depth of Field pixel shader
            const char* dofPS = R"(
                Texture2D sceneTexture : register(t0);
                Texture2D depthTexture : register(t1);
                SamplerState linearSampler : register(s0);
                cbuffer PostProcessParams : register(b0) {
                    float4 params0; // x=focalDist, y=focalLen, z=aperture, w=maxBokeh
                    float4 params1; // x=screenW, y=screenH, z=nearStart, w=nearEnd
                    float4 params2; // x=farStart, y=farEnd, z=blurSamples, w=time
                    float4 params3;
                };
                static const float2 poissonDisk[16] = {
                    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
                    float2(-0.094184101, -0.92938870), float2(0.34495938, 0.29387760),
                    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
                    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
                    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
                    float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
                    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590),
                    float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)
                };
                float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                    float depth = depthTexture.Sample(linearSampler, uv).r;
                    float linearDepth = params0.y / (depth * (params2.y - params1.z) + params1.z);
                    float coc = abs(linearDepth - params0.x) / max(linearDepth, 0.001);
                    coc = saturate(coc) * params0.w;
                    float3 color = float3(0, 0, 0);
                    float totalWeight = 0;
                    int samples = (int)params2.z;
                    float2 texelSize = 1.0 / float2(params1.x, params1.y);
                    for (int i = 0; i < samples && i < 16; i++) {
                        float2 offset = poissonDisk[i] * coc * texelSize;
                        float3 s = sceneTexture.Sample(linearSampler, uv + offset).rgb;
                        float w = 1.0;
                        color += s * w;
                        totalWeight += w;
                    }
                    return float4(color / max(totalWeight, 0.001), 1);
                }
            )";

            // Vignette pixel shader
            const char* vignettePS = R"(
                Texture2D sceneTexture : register(t0);
                SamplerState linearSampler : register(s0);
                cbuffer PostProcessParams : register(b0) {
                    float4 params0; // x=intensity, y=smoothness, z=roundness, w=unused
                    float4 params1; // x=screenW, y=screenH, z=centerX, w=centerY
                    float4 params2; // x=colorR, y=colorG, z=colorB, w=unused
                    float4 params3;
                };
                float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                    float3 scene = sceneTexture.Sample(linearSampler, uv).rgb;
                    float2 center = float2(params1.z, params1.w);
                    float2 dist = (uv - center) * float2(params1.x / params1.y, 1.0);
                    float d = length(dist);
                    float vignette = smoothstep(params0.y, params0.y - params0.x, d * (2.0 - params0.z));
                    float3 vigColor = float3(params2.x, params2.y, params2.z);
                    float3 result = lerp(vigColor, scene, vignette);
                    return float4(result, 1);
                }
            )";

            // Chromatic Aberration pixel shader
            const char* chromAbPS = R"(
                Texture2D sceneTexture : register(t0);
                SamplerState linearSampler : register(s0);
                cbuffer PostProcessParams : register(b0) {
                    float4 params0; // x=intensity, y=radialFalloff, z=rOffset, w=gOffset
                    float4 params1; // x=screenW, y=screenH, z=bOffset, w=unused
                    float4 params2;
                    float4 params3;
                };
                float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                    float2 center = float2(0.5, 0.5);
                    float2 dir = uv - center;
                    float dist = length(dir);
                    float falloff = pow(dist, params0.y) * params0.x * 0.01;
                    float2 rUV = uv + dir * falloff * params0.z;
                    float2 gUV = uv + dir * falloff * params0.w;
                    float2 bUV = uv + dir * falloff * params1.z;
                    float r = sceneTexture.Sample(linearSampler, rUV).r;
                    float g = sceneTexture.Sample(linearSampler, gUV).g;
                    float b = sceneTexture.Sample(linearSampler, bUV).b;
                    return float4(r, g, b, 1);
                }
            )";

            // Film Grain pixel shader
            const char* filmGrainPS = R"(
                Texture2D sceneTexture : register(t0);
                SamplerState linearSampler : register(s0);
                cbuffer PostProcessParams : register(b0) {
                    float4 params0; // x=intensity, y=size, z=speed, w=lumContrib
                    float4 params1; // x=screenW, y=screenH, z=time, w=colored
                    float4 params2;
                    float4 params3;
                };
                float hash(float2 p) {
                    float3 p3 = frac(float3(p.xyx) * 0.1031);
                    p3 += dot(p3, p3.yzx + 33.33);
                    return frac((p3.x + p3.y) * p3.z);
                }
                float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                    float3 scene = sceneTexture.Sample(linearSampler, uv).rgb;
                    float2 grainUV = uv * float2(params1.x, params1.y) / params0.y;
                    float t = params1.z * params0.z;
                    float grain = hash(grainUV + t) * 2.0 - 1.0;
                    float lum = dot(scene, float3(0.299, 0.587, 0.114));
                    float grainAmount = params0.x * lerp(1.0, 1.0 - lum, params0.w);
                    float3 result;
                    if (params1.w > 0.5) {
                        float3 grainColor = float3(hash(grainUV + t), hash(grainUV + t + 1), hash(grainUV + t + 2)) * 2.0 - 1.0;
                        result = scene + grainColor * grainAmount;
                    } else {
                        result = scene + grain * grainAmount;
                    }
                    return float4(saturate(result), 1);
                }
            )";

            // Lens Distortion pixel shader
            const char* lensDistPS = R"(
                Texture2D sceneTexture : register(t0);
                SamplerState linearSampler : register(s0);
                cbuffer PostProcessParams : register(b0) {
                    float4 params0; // x=barrel, y=zoom, z=centerX, w=centerY
                    float4 params1; // x=screenW, y=screenH, z=cubic, w=unused
                    float4 params2;
                    float4 params3;
                };
                float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                    float2 center = float2(params0.z, params0.w);
                    float2 coord = (uv - center) * 2.0;
                    float r2 = dot(coord, coord);
                    float distortion = 1.0 + params0.x * r2 + params1.z * r2 * r2;
                    float2 distortedUV = center + coord * distortion * 0.5 / params0.y;
                    if (any(distortedUV < 0) || any(distortedUV > 1))
                        return float4(0, 0, 0, 1);
                    return sceneTexture.Sample(linearSampler, distortedUV);
                }
            )";

            // Light Shafts (God Rays) pixel shader
            const char* lightShaftsPS = R"(
                Texture2D sceneTexture : register(t0);
                SamplerState linearSampler : register(s0);
                cbuffer PostProcessParams : register(b0) {
                    float4 params0; // x=lightPosX, y=lightPosY, z=density, w=weight
                    float4 params1; // x=screenW, y=screenH, z=decay, w=exposure
                    float4 params2; // x=sampleCount, y=colorR, z=colorG, w=colorB
                    float4 params3;
                };
                float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                    float3 scene = sceneTexture.Sample(linearSampler, uv).rgb;
                    float2 lightPos = float2(params0.x, params0.y);
                    float2 deltaTexCoord = (uv - lightPos) * params0.z / params2.x;
                    float2 texCoord = uv;
                    float illuminationDecay = 1.0;
                    float3 godRays = float3(0, 0, 0);
                    int samples = (int)params2.x;
                    for (int i = 0; i < samples && i < 64; i++) {
                        texCoord -= deltaTexCoord;
                        float3 s = sceneTexture.Sample(linearSampler, saturate(texCoord)).rgb;
                        float lum = dot(s, float3(0.299, 0.587, 0.114));
                        s *= illuminationDecay * params0.w * lum;
                        godRays += s;
                        illuminationDecay *= params1.z;
                    }
                    float3 shaftColor = float3(params2.y, params2.z, params2.w);
                    return float4(scene + godRays * params1.w * shaftColor, 1);
                }
            )";

            // Lens Flare pixel shader
            const char* lensFlarePS = R"(
                Texture2D sceneTexture : register(t0);
                SamplerState linearSampler : register(s0);
                cbuffer PostProcessParams : register(b0) {
                    float4 params0; // x=threshold, y=intensity, z=ghostCount, w=ghostSpacing
                    float4 params1; // x=screenW, y=screenH, z=haloRadius, w=haloThickness
                    float4 params2; // x=chromDist, y=unused, z=unused, w=unused
                    float4 params3;
                };
                float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                    float3 scene = sceneTexture.Sample(linearSampler, uv).rgb;
                    float2 ghostVec = (float2(0.5, 0.5) - uv) * params0.w;
                    float3 flare = float3(0, 0, 0);
                    int ghosts = (int)params0.z;
                    for (int i = 0; i < ghosts && i < 8; i++) {
                        float2 suv = frac(uv + ghostVec * (float)(i + 1));
                        float3 s = sceneTexture.Sample(linearSampler, suv).rgb;
                        float lum = dot(s, float3(0.299, 0.587, 0.114));
                        float w = max(0, lum - params0.x);
                        float d = length(suv - float2(0.5, 0.5));
                        w *= 1.0 - smoothstep(0.0, 0.75, d);
                        flare += s * w;
                    }
                    float2 haloVec = normalize(float2(0.5, 0.5) - uv) * params1.z;
                    float2 haloUV = uv + haloVec;
                    float haloD = length(haloUV - float2(0.5, 0.5));
                    float haloW = smoothstep(params1.z - params1.w, params1.z, haloD) *
                                  (1.0 - smoothstep(params1.z, params1.z + params1.w, haloD));
                    float3 haloSample = sceneTexture.Sample(linearSampler, haloUV).rgb;
                    float haloLum = dot(haloSample, float3(0.299, 0.587, 0.114));
                    flare += haloSample * haloW * max(0, haloLum - params0.x);
                    return float4(scene + flare * params0.y, 1);
                }
            )";

            // Sharpen (CAS) pixel shader
            const char* sharpenPS = R"(
                Texture2D sceneTexture : register(t0);
                SamplerState pointSampler : register(s1);
                cbuffer PostProcessParams : register(b0) {
                    float4 params0; // x=amount, y=threshold, z=adaptive, w=unused
                    float4 params1; // x=screenW, y=screenH, z=unused, w=unused
                    float4 params2;
                    float4 params3;
                };
                float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                    float2 texelSize = 1.0 / float2(params1.x, params1.y);
                    float3 center = sceneTexture.Sample(pointSampler, uv).rgb;
                    float3 top = sceneTexture.Sample(pointSampler, uv + float2(0, -1) * texelSize).rgb;
                    float3 bottom = sceneTexture.Sample(pointSampler, uv + float2(0, 1) * texelSize).rgb;
                    float3 left = sceneTexture.Sample(pointSampler, uv + float2(-1, 0) * texelSize).rgb;
                    float3 right = sceneTexture.Sample(pointSampler, uv + float2(1, 0) * texelSize).rgb;
                    float3 blur = (top + bottom + left + right) * 0.25;
                    float3 diff = center - blur;
                    float sharpness = params0.x;
                    if (params0.z > 0.5) {
                        float lum = dot(center, float3(0.299, 0.587, 0.114));
                        sharpness *= smoothstep(0.0, 0.3, lum) * smoothstep(1.0, 0.7, lum);
                    }
                    float3 sharpened = center + diff * sharpness;
                    float edgeMag = length(diff);
                    if (edgeMag < params0.y) sharpened = center;
                    return float4(saturate(sharpened), 1);
                }
            )";

            // Motion Blur pixel shader
            const char* motionBlurPS = R"(
                Texture2D sceneTexture : register(t0);
                Texture2D depthTexture : register(t1);
                SamplerState linearSampler : register(s0);
                cbuffer PostProcessParams : register(b0) {
                    float4 params0; // x=intensity, y=sampleCount, z=maxRadius, w=velScale
                    float4 params1; // x=screenW, y=screenH, z=time, w=unused
                    float4 params2; // xy=velocity (camera-based fallback), z=minThreshold, w=unused
                    float4 params3;
                };
                float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                    float3 scene = sceneTexture.Sample(linearSampler, uv).rgb;
                    float2 velocity = params2.xy * params0.x;
                    float velMag = length(velocity * float2(params1.x, params1.y));
                    if (velMag < params2.z)
                        return float4(scene, 1);
                    float2 texelSize = 1.0 / float2(params1.x, params1.y);
                    float maxRad = params0.z * texelSize.x;
                    velocity = clamp(velocity, -maxRad, maxRad);
                    int samples = (int)params0.y;
                    float3 color = scene;
                    float totalWeight = 1.0;
                    for (int i = 1; i < samples && i < 16; i++) {
                        float t = (float)i / (float)(samples - 1) - 0.5;
                        float2 offset = velocity * t;
                        float3 s = sceneTexture.Sample(linearSampler, uv + offset).rgb;
                        color += s;
                        totalWeight += 1.0;
                    }
                    return float4(color / totalWeight, 1);
                }
            )";

            // Compile each effect shader
            struct ShaderDef
            {
                const char* source;
                ComPtr<ID3D11PixelShader>* target;
                const char* name;
            };
            ShaderDef shaders[] = {
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

            for (auto& sd : shaders)
            {
                ComPtr<ID3DBlob> psBlob, errBlob;
                HRESULT hr = D3DCompile(sd.source, strlen(sd.source), sd.name, nullptr, nullptr, "main", "ps_5_0", 0, 0,
                                        &psBlob, &errBlob);
                if (SUCCEEDED(hr))
                {
                    m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                                sd.target->GetAddressOf());
                }
            }
#endif
        }

        // ---- Pass Execution ----

        void SwapTargets() { m_currentTarget = 1 - m_currentTarget; }

        int GetSourceTarget() const { return 1 - m_currentTarget; }

        void BeginPass(ID3D11PixelShader* ps, const PostProcessCB& cb)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            if (!m_context || !ps)
                return;

            int src = GetSourceTarget();
            int dst = m_currentTarget;

            // Unbind current target as SRV
            ID3D11ShaderResourceView* nullSRV = nullptr;
            m_context->PSSetShaderResources(0, 1, &nullSRV);

            // Set render target
            m_context->OMSetRenderTargets(1, &m_pingPongRTVs[dst], nullptr);

            // Set shaders
            m_context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
            m_context->PSSetShader(ps, nullptr, 0);

            // Update constant buffer
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                memcpy(mapped.pData, &cb, sizeof(PostProcessCB));
                m_context->Unmap(m_constantBuffer.Get(), 0);
            }

            m_context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

            // Set source texture
            m_context->PSSetShaderResources(0, 1, &m_pingPongSRVs[src]);
            if (m_depthSRV)
                m_context->PSSetShaderResources(1, 1, &m_depthSRV);

            // Set samplers
            ID3D11SamplerState* samplers[] = {m_linearSampler.Get(), m_pointSampler.Get()};
            m_context->PSSetSamplers(0, 2, samplers);
#else
            (void)ps;
            (void)cb;
#endif
        }

        void DrawFullscreen()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            if (!m_context)
                return;
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->IASetInputLayout(nullptr);
            m_context->Draw(3, 0);
            SwapTargets();
#endif
        }

        void ProcessPass(PostProcessPass pass, float deltaTime)
        {
            auto startTime = std::chrono::high_resolution_clock::now();

            PostProcessCB cb = {};
            cb.params1.x = static_cast<float>(m_width);
            cb.params1.y = static_cast<float>(m_height);
            cb.params1.z = m_totalTime;

            ID3D11PixelShader* ps = nullptr;

            switch (pass)
            {
            case PostProcessPass::FXAA:
                cb.params0 = {m_fxaaSettings.edgeThreshold, m_fxaaSettings.edgeThresholdMin,
                              m_fxaaSettings.subpixelQuality, 0.0f};
                ps = m_fxaaPS.Get();
                break;

            case PostProcessPass::DepthOfField:
                cb.params0 = {m_dofSettings.focalDistance, m_dofSettings.focalLength, m_dofSettings.aperture,
                              m_dofSettings.maxBokehSize};
                cb.params1.z = m_dofSettings.nearBlurStart;
                cb.params1.w = m_dofSettings.nearBlurEnd;
                cb.params2 = {m_dofSettings.farBlurStart, m_dofSettings.farBlurEnd,
                              static_cast<float>(m_dofSettings.blurSamples), m_totalTime};
                ps = m_dofPS.Get();
                break;

            case PostProcessPass::MotionBlur:
                cb.params0 = {0.5f, 8.0f, 32.0f, 1.0f};
                cb.params2 = {0.0f, 0.0f, 0.5f, 0.0f};
                ps = m_motionBlurPS.Get();
                break;

            case PostProcessPass::Vignette:
                cb.params0 = {m_vignetteSettings.intensity, m_vignetteSettings.smoothness, m_vignetteSettings.roundness,
                              0.0f};
                cb.params1.z = m_vignetteSettings.center.x;
                cb.params1.w = m_vignetteSettings.center.y;
                cb.params2 = {m_vignetteSettings.color.x, m_vignetteSettings.color.y, m_vignetteSettings.color.z, 0.0f};
                ps = m_vignettePS.Get();
                break;

            case PostProcessPass::ChromaticAberration:
                cb.params0 = {m_chromAbSettings.intensity, m_chromAbSettings.radialFalloff,
                              m_chromAbSettings.channelOffsets.x, m_chromAbSettings.channelOffsets.y};
                cb.params1.z = m_chromAbSettings.channelOffsets.z;
                ps = m_chromAbPS.Get();
                break;

            case PostProcessPass::FilmGrain:
                cb.params0 = {m_filmGrainSettings.intensity, m_filmGrainSettings.size, m_filmGrainSettings.speed,
                              m_filmGrainSettings.luminanceContribution};
                cb.params1.z = m_totalTime;
                cb.params1.w = m_filmGrainSettings.colored ? 1.0f : 0.0f;
                ps = m_filmGrainPS.Get();
                break;

            case PostProcessPass::LensDistortion:
                cb.params0 = {m_lensDistSettings.barrelDistortion, m_lensDistSettings.zoomCompensation,
                              m_lensDistSettings.center.x, m_lensDistSettings.center.y};
                cb.params1.z = m_lensDistSettings.cubicDistortion;
                ps = m_lensDistPS.Get();
                break;

            case PostProcessPass::LightShafts:
                cb.params0 = {m_lightShaftSettings.lightScreenPos.x, m_lightShaftSettings.lightScreenPos.y,
                              m_lightShaftSettings.density, m_lightShaftSettings.weight};
                cb.params1.z = m_lightShaftSettings.decay;
                cb.params1.w = m_lightShaftSettings.exposure;
                cb.params2 = {static_cast<float>(m_lightShaftSettings.sampleCount), m_lightShaftSettings.color.x,
                              m_lightShaftSettings.color.y, m_lightShaftSettings.color.z};
                ps = m_lightShaftsPS.Get();
                break;

            case PostProcessPass::LensFlare:
                cb.params0 = {m_lensFlareSettings.threshold, m_lensFlareSettings.intensity,
                              static_cast<float>(m_lensFlareSettings.ghostCount), m_lensFlareSettings.ghostSpacing};
                cb.params1.z = m_lensFlareSettings.haloRadius;
                cb.params1.w = m_lensFlareSettings.haloThickness;
                cb.params2 = {m_lensFlareSettings.chromaticDistortion, 0.0f, 0.0f, 0.0f};
                ps = m_lensFlarePS.Get();
                break;

            case PostProcessPass::Sharpen:
                cb.params0 = {m_sharpenSettings.amount, m_sharpenSettings.threshold,
                              m_sharpenSettings.adaptiveSharpening ? 1.0f : 0.0f, 0.0f};
                ps = m_sharpenPS.Get();
                break;

            default:
                return;
            }

            if (ps)
            {
                BeginPass(ps, cb);
                DrawFullscreen();
            }

            auto endTime = std::chrono::high_resolution_clock::now();
            float ms = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count() / 1000.0f;
            m_passTimings[static_cast<int>(pass)] = ms;
        }

        // ---- State ----
        bool m_initialized = false;
        uint32_t m_width = 1920;
        uint32_t m_height = 1080;
        float m_totalTime = 0.0f;
        int m_activePassCount = 0;
        int m_currentTarget = 0;

        bool m_passEnabled[static_cast<int>(PostProcessPass::Count)] = {};
        float m_passTimings[static_cast<int>(PostProcessPass::Count)] = {};

        // Settings
        FXAASettings m_fxaaSettings;
        DepthOfFieldSettings m_dofSettings;
        VignetteSettings m_vignetteSettings;
        ChromaticAberrationSettings m_chromAbSettings;
        FilmGrainSettings m_filmGrainSettings;
        LensDistortionSettings m_lensDistSettings;
        LightShaftSettings m_lightShaftSettings;
        LensFlareSettings m_lensFlareSettings;
        SharpenSettings m_sharpenSettings;

        // GPU Resources
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        ID3D11ShaderResourceView* m_depthSRV = nullptr;
        ID3D11ShaderResourceView* m_inputSRV = nullptr;

        // Ping-pong render targets
        ComPtr<ID3D11Texture2D> m_pingPongTextures[2];
        ID3D11RenderTargetView* m_pingPongRTVs[2] = {};
        ID3D11ShaderResourceView* m_pingPongSRVs[2] = {};

        // Shaders
        ComPtr<ID3D11VertexShader> m_fullscreenVS;
        ComPtr<ID3D11PixelShader> m_fxaaPS;
        ComPtr<ID3D11PixelShader> m_dofPS;
        ComPtr<ID3D11PixelShader> m_motionBlurPS;
        ComPtr<ID3D11PixelShader> m_vignettePS;
        ComPtr<ID3D11PixelShader> m_chromAbPS;
        ComPtr<ID3D11PixelShader> m_filmGrainPS;
        ComPtr<ID3D11PixelShader> m_lensDistPS;
        ComPtr<ID3D11PixelShader> m_lightShaftsPS;
        ComPtr<ID3D11PixelShader> m_lensFlarePS;
        ComPtr<ID3D11PixelShader> m_sharpenPS;

        // Resources
        ComPtr<ID3D11Buffer> m_constantBuffer;
        ComPtr<ID3D11SamplerState> m_linearSampler;
        ComPtr<ID3D11SamplerState> m_pointSampler;
    };

} // namespace Spark::Graphics
