#include "PostProcessingPipeline.h"
#include "../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Spark::Graphics
{

    // =============================================================================
    // Lifecycle
    // =============================================================================

    bool PostProcessingPipeline::Initialize(uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;

        if (m_device)
        {
            if (!CreatePingPongTargets())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                "PostProcessingPipeline: failed to create ping-pong targets");
                return false;
            }
            if (!CreatePostProcessShaders())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "PostProcessingPipeline: failed to create shaders");
                return false;
            }
        }

        // Phase J: activate the CPU-side temporal filter unconditionally —
        // it runs even when there is no D3D11 device (headless / NullRHI).
        // The history buffer grows with viewport size and is reset to match
        // the pipeline resolution.
        m_ssaoTemporalFilter.Initialize(width, height);

        // Phase K: activate the volume manager. It has no viewport dependency
        // so the call is a fixed clear + mark-initialised; from this point
        // forward Process() will call Update() and (if m_volumeBlendEnabled)
        // push the blended stack into the effect settings.
        m_volumeManager.Initialize();

        // Phase N: activate RTHandleSystem with the pipeline's reference
        // viewport. Pure CPU — runs on every platform.
        m_rtHandleSystem.Initialize(width, height);

#ifdef SPARK_PLATFORM_WINDOWS
        // Phase J: activate the D3D11-backed orphans when a device is available.
        // All three have no-op Initialize methods when the pipeline is
        // running headless (no m_device), so the non-device path is already
        // correct for tests and the NullRHI backend.
        if (m_device)
        {
            m_rtPool.Initialize(m_device, /*reclaimAfterFrames*/ 60);
            m_gpuTimer.Initialize(m_device, /*maxTimers*/ static_cast<uint32_t>(PostProcessPass::Count));
            // Phase N: a 2 MB ring is plenty for the PostProcessCB traffic
            // the pipeline generates today. Allocations above the capacity
            // fail gracefully (Allocate returns an invalid allocation) and
            // the existing Map/Unmap path continues to work for oversize
            // updates.
            m_cbRing.Initialize(m_device, /*capacityBytes*/ 2 * 1024 * 1024);
        }
        if (m_context)
        {
            m_gpuMarkers.Initialize(m_context);
        }
#endif

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "PostProcessingPipeline initialized (%ux%u)", width, height);
        return true;
    }

    void PostProcessingPipeline::Resize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return;
        if (m_width == width && m_height == height)
            return;

        m_width = width;
        m_height = height;

        if (m_initialized && m_device)
            CreatePingPongTargets();

        if (m_ssaoTemporalFilter.IsInitialized())
            m_ssaoTemporalFilter.Resize(width, height);

        if (m_rtHandleSystem.IsInitialized())
            m_rtHandleSystem.SetReferenceSize(width, height);
    }

    void PostProcessingPipeline::SetDevice(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        m_device = device;
        m_context = context;
    }

    void PostProcessingPipeline::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "PostProcessingPipeline shutting down");
        for (int i = 0; i < 2; ++i)
        {
            m_pingPongTextures[i].Reset();
            m_pingPongRTVs[i] = nullptr;
            m_pingPongSRVs[i] = nullptr;
        }
        m_fullscreenVS.Reset();
        m_bloomPS.Reset();
        m_gtaoPS.Reset();
        m_ssaoTemporalPS.Reset();
        m_autoExposurePS.Reset();
        m_tonemapPS.Reset();
        m_colorGradingPS.Reset();
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
        m_currentExposure = 1.0f;

        // Phase J: release orphan-owned resources alongside the rest of the
        // D3D11 state. Calling Shutdown on an uninitialised instance is safe —
        // each orphan guards its teardown on its own `m_initialized` flag.
        m_ssaoTemporalFilter.Shutdown();
        // Phase K: drop any volumes the caller registered; they will not
        // survive a pipeline restart.
        m_volumeManager.Shutdown();
        // Phase N: tear down the portable RTHandleSystem alongside the
        // other CPU-side orphans.
        m_rtHandleSystem.Shutdown();
#ifdef SPARK_PLATFORM_WINDOWS
        m_rtPool.Shutdown();
        m_gpuTimer.Shutdown();
        m_gpuMarkers.Shutdown();
        // Phase N: ConstantBufferRing follows the same teardown contract —
        // safe to call on an uninitialised ring.
        m_cbRing.Shutdown();
#endif

        m_initialized = false;
    }

    void PostProcessingPipeline::Process(float deltaTime)
    {
        if (!m_initialized)
            return;
        m_totalTime += deltaTime;
        m_activePassCount = 0;
        m_vsAlreadyBound = false; // Reset per-frame — VS/sampler/CB binding will be set on first pass

        // Phase K: evaluate all registered volumes against the live camera
        // position and push the blended stack into the pipeline settings
        // before any pass reads from them.
        m_volumeManager.Update(m_cameraPosition);
        if (m_volumeBlendEnabled)
        {
            ApplyVolumeStack();
        }

#ifdef SPARK_PLATFORM_WINDOWS
        // Phase J: age pooled transient RTs and begin a new timestamp frame
        // before the first pass runs. The RT pool's Tick() is cheap (one
        // pass over its entry vector) and the timestamp BeginFrame is a
        // no-op if Initialize() was never called.
        if (m_context)
        {
            m_gpuTimer.BeginFrame(m_context);
            // Phase N: open the constant-buffer ring for this frame. The
            // map is a WRITE_DISCARD so the driver renames the buffer
            // under the hood — no GPU stall. If the ring was never
            // initialised (headless pipeline) BeginFrame returns false
            // and the pipeline continues to use the per-pass Map/Unmap
            // path on m_constantBuffer.
            m_cbRing.BeginFrame(m_context);
        }
        m_rtPool.Tick();

        // Bracket the entire post-process chain in a single named event so
        // PIX / RenderDoc captures group the per-pass inner events.
        if (m_gpuMarkers.IsInitialized())
        {
            m_gpuMarkers.BeginEvent(L"PostProcessingPipeline");
        }
#endif

        for (int i = 0; i < static_cast<int>(PostProcessPass::Count); ++i)
        {
            auto pass = static_cast<PostProcessPass>(i);
            if (IsEffectEnabled(pass))
            {
                ProcessPass(pass, deltaTime);
                m_activePassCount++;
            }
        }

#ifdef SPARK_PLATFORM_WINDOWS
        if (m_gpuMarkers.IsInitialized())
        {
            m_gpuMarkers.EndEvent();
        }
        if (m_context)
        {
            m_gpuTimer.EndFrame(m_context);
            // Phase N: unmap the ring so the driver can rename on the
            // next BeginFrame. Safe to call when BeginFrame was never
            // issued — the ring guards on m_frameMapped internally.
            m_cbRing.EndFrame();
        }
#endif
    }

    void PostProcessingPipeline::Render()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_initialized || !m_context || !m_fullscreenVS || !m_sharpenPS || !m_constantBuffer)
            return;

        int src = GetSourceTarget();
        if (m_pingPongSRVs[src])
        {
            PostProcessCB cb = {};
            cb.params1 = {static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 0.0f};
            cb.params0 = {0.0f, 0.0f, 0.0f, 0.0f};
            if (m_sharpenPS)
            {
                m_context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
                m_context->PSSetShader(m_sharpenPS.Get(), nullptr, 0);

                D3D11_MAPPED_SUBRESOURCE mapped = {};
                if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) &&
                    mapped.pData)
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

    // =============================================================================
    // Metrics & Console
    // =============================================================================

    std::vector<PassMetrics> PostProcessingPipeline::GetPassMetrics() const
    {
        std::vector<PassMetrics> metrics;
        static const char* passNames[] = {
            "GTAO",           "SSAOTemporal", "Bloom",      "AutoExposure", "Tonemapping",         "ColorGrading",
            "FXAA",           "DepthOfField", "MotionBlur", "Vignette",     "ChromaticAberration", "FilmGrain",
            "LensDistortion", "LightShafts",  "LensFlare",  "Sharpen"};
        static_assert(sizeof(passNames) / sizeof(passNames[0]) == static_cast<size_t>(PostProcessPass::Count),
                      "passNames must stay aligned with PostProcessPass enum");
        for (int i = 0; i < static_cast<int>(PostProcessPass::Count); ++i)
        {
            PassMetrics pm;
            pm.name = passNames[i];
            pm.isEnabled = m_passEnabled[i];
            pm.timeMs = m_passTimings[i];
#ifdef SPARK_PLATFORM_WINDOWS
            // Phase J: prefer the GPU-side reading when one has been
            // collected this frame. GPU timestamps are at least two frames
            // behind (kFrameLatency = 2) so during warm-up the CPU value
            // remains authoritative.
            float gpuMs = m_gpuTimer.GetPassTimeMs(passNames[i]);
            if (gpuMs > 0.0f)
            {
                pm.timeMs = gpuMs;
            }
#endif
            metrics.push_back(pm);
        }
        return metrics;
    }

    std::string PostProcessingPipeline::Console_ListEffects() const
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

    // -----------------------------------------------------------------------------
    // Phase J: Tier 2 orphan accessors
    // -----------------------------------------------------------------------------

    uint32_t PostProcessingPipeline::GetRenderTargetPoolSize() const
    {
#ifdef SPARK_PLATFORM_WINDOWS
        return m_rtPool.GetMetrics().totalTargets;
#else
        return 0;
#endif
    }

    std::string PostProcessingPipeline::Console_RenderTargetPoolStatus() const
    {
#ifdef SPARK_PLATFORM_WINDOWS
        return m_rtPool.Console_GetStatus();
#else
        return "Render Target Pool:\n  (not compiled on this platform)\n";
#endif
    }

    uint32_t PostProcessingPipeline::GetGPUMarkerDepth() const
    {
#ifdef SPARK_PLATFORM_WINDOWS
        return m_gpuMarkers.GetEventDepth();
#else
        return 0;
#endif
    }

    // -----------------------------------------------------------------------------
    // Phase K: Volume manager → effect settings binding
    // -----------------------------------------------------------------------------

    void PostProcessingPipeline::ApplyVolumeStack()
    {
        // Pull the most recently evaluated blended stack and map each
        // component onto the pipeline's existing effect settings. Only
        // fields whose `overrideState` flag was set by a real volume
        // land on the pipeline settings — every other field keeps its
        // prior value so the caller can still tweak settings by hand
        // without a volume-less frame clobbering them back to defaults.
        const auto& stack = m_volumeManager.GetStack();

        // Exposure → auto-exposure compensation EV. The volume system
        // models exposure as a fractional EV shift on top of the
        // pipeline's own auto-exposure math, not as a replacement for it.
        if (stack.exposure.compensationEV.overrideState)
        {
            m_autoExposureSettings.compensationEV = stack.exposure.compensationEV.value;
        }

        // Bloom: intensity, threshold, soft-knee, scatter.
        if (stack.bloom.intensity.overrideState)
        {
            m_bloomSettings.intensity = stack.bloom.intensity.value;
        }
        if (stack.bloom.threshold.overrideState)
        {
            m_bloomSettings.threshold = stack.bloom.threshold.value;
        }
        if (stack.bloom.softKnee.overrideState)
        {
            m_bloomSettings.softThreshold = stack.bloom.softKnee.value;
        }
        if (stack.bloom.scatter.overrideState)
        {
            m_bloomSettings.scatter = stack.bloom.scatter.value;
        }

        // Color grading: lift / gain triplets + saturation / contrast /
        // temperature / tint. The volume system tracks gamma but the
        // pipeline does not expose a gamma curve control — those values
        // are intentionally dropped.
        if (stack.colorGrading.liftR.overrideState)
        {
            m_colorGradingSettings.lift.x = stack.colorGrading.liftR.value;
        }
        if (stack.colorGrading.liftG.overrideState)
        {
            m_colorGradingSettings.lift.y = stack.colorGrading.liftG.value;
        }
        if (stack.colorGrading.liftB.overrideState)
        {
            m_colorGradingSettings.lift.z = stack.colorGrading.liftB.value;
        }
        if (stack.colorGrading.gainR.overrideState)
        {
            m_colorGradingSettings.gain.x = stack.colorGrading.gainR.value;
        }
        if (stack.colorGrading.gainG.overrideState)
        {
            m_colorGradingSettings.gain.y = stack.colorGrading.gainG.value;
        }
        if (stack.colorGrading.gainB.overrideState)
        {
            m_colorGradingSettings.gain.z = stack.colorGrading.gainB.value;
        }
        if (stack.colorGrading.saturation.overrideState)
        {
            m_colorGradingSettings.saturation = stack.colorGrading.saturation.value;
        }
        if (stack.colorGrading.contrast.overrideState)
        {
            m_colorGradingSettings.contrast = stack.colorGrading.contrast.value;
        }
        if (stack.colorGrading.temperature.overrideState)
        {
            m_colorGradingSettings.temperature = stack.colorGrading.temperature.value;
        }
        if (stack.colorGrading.tint.overrideState)
        {
            m_colorGradingSettings.tint = stack.colorGrading.tint.value;
        }

        // Fog: no fog pass lives in PostProcessingPipeline yet. The stack's
        // fog component is still evaluated so GetVolumeManager().GetStack()
        // returns a meaningful value — a future fog pass can read it
        // without changing the binding here.
    }

    uint32_t PostProcessingPipeline::GetConstantBufferRingCapacity() const
    {
#ifdef SPARK_PLATFORM_WINDOWS
        return m_cbRing.GetMetrics().capacityBytes;
#else
        return 0;
#endif
    }

    uint32_t PostProcessingPipeline::GetConstantBufferRingPeakUsage() const
    {
#ifdef SPARK_PLATFORM_WINDOWS
        return m_cbRing.GetMetrics().peakUsageBytes;
#else
        return 0;
#endif
    }

    float PostProcessingPipeline::GetPassTimeMs(PostProcessPass pass) const
    {
        const int idx = static_cast<int>(pass);
        if (idx < 0 || idx >= static_cast<int>(PostProcessPass::Count))
        {
            return 0.0f;
        }
#ifdef SPARK_PLATFORM_WINDOWS
        // Pull the live reading from GetPassMetrics so we share the same
        // GPU-vs-CPU precedence rule with every other caller.
        auto metrics = GetPassMetrics();
        return metrics[idx].timeMs;
#else
        return m_passTimings[idx];
#endif
    }

    // =============================================================================
    // GPU Resource Creation
    // =============================================================================

// Windows D3D11 implementations of the six functions below live in
// PostProcessingPipelineWindows.cpp (CreatePingPongTargets, CreatePostProcessShaders,
// CompileEffectShaders) and PostProcessingPipelineWindowsPasses.cpp (BeginPass,
// DrawFullscreen, ProcessPass). Emitting them here on Windows too would produce
// LNK4006 duplicate-definition warnings.
#ifndef SPARK_PLATFORM_WINDOWS

    bool PostProcessingPipeline::CreatePingPongTargets()
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

            hr = m_device->CreateRenderTargetView(m_pingPongTextures[i].Get(), nullptr, &m_pingPongRTVs[i]);
            if (FAILED(hr))
                return false;

            hr = m_device->CreateShaderResourceView(m_pingPongTextures[i].Get(), nullptr, &m_pingPongSRVs[i]);
            if (FAILED(hr))
                return false;
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
        // Bloom pixel shader (threshold extract + single-pass Gaussian blur + composite)
        // Multi-pass downscale/upscale is done CPU-side; this shader handles one pass.
        const char* bloomPS = R"(
        Texture2D sceneTexture : register(t0);
        SamplerState linearSampler : register(s0);
        cbuffer PostProcessParams : register(b0) {
            float4 params0; // x=threshold, y=softThreshold, z=intensity, w=scatter
            float4 params1; // x=width, y=height, z=radius, w=iterations
            float4 params2;
            float4 params3;
        };
        float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
        float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
            float2 texelSize = 1.0 / float2(params1.x, params1.y);
            float3 scene = sceneTexture.Sample(linearSampler, uv).rgb;
            float lum = Luminance(scene);
            // Soft threshold knee curve
            float knee = params0.x * params0.y;
            float soft = lum - params0.x + knee;
            soft = clamp(soft, 0, 2.0 * knee);
            soft = soft * soft / (4.0 * knee + 0.00001);
            float contribution = max(soft, lum - params0.x) / max(lum, 0.00001);
            float3 bright = scene * max(0, contribution);
            // 9-tap Gaussian blur for bloom spread
            float3 blurred = float3(0, 0, 0);
            float totalWeight = 0;
            float r = params1.z;
            for (int x = -1; x <= 1; x++) {
                for (int y = -1; y <= 1; y++) {
                    float2 offset = float2(x, y) * texelSize * r;
                    float3 s = sceneTexture.Sample(linearSampler, uv + offset).rgb;
                    float sLum = Luminance(s);
                    float sc = max(0, sLum - params0.x) / max(sLum, 0.00001);
                    float w = exp(-0.5 * (x*x + y*y));
                    blurred += s * sc * w;
                    totalWeight += w;
                }
            }
            blurred /= max(totalWeight, 0.001);
            float3 result = scene + blurred * params0.z * params0.w;
            return float4(result, 1);
        }
    )";

        // Auto-exposure pixel shader (average luminance + exposure adaptation)
        // Computes scene average luminance and applies adapted exposure.
        const char* autoExposurePS = R"(
        Texture2D sceneTexture : register(t0);
        SamplerState linearSampler : register(s0);
        cbuffer PostProcessParams : register(b0) {
            float4 params0; // x=currentExposure, y=targetLuminance, z=compensationEV, w=unused
            float4 params1; // x=width, y=height, z=unused, w=unused
            float4 params2;
            float4 params3;
        };
        float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
        float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
            float3 scene = sceneTexture.Sample(linearSampler, uv).rgb;
            float exposure = params0.x * exp2(params0.z);
            return float4(scene * exposure, 1);
        }
    )";

        // Tonemapping pixel shader (ACES, Filmic, Neutral, Reinhard)
        const char* tonemapPS = R"(
        Texture2D sceneTexture : register(t0);
        SamplerState linearSampler : register(s0);
        cbuffer PostProcessParams : register(b0) {
            float4 params0; // x=exposure, y=whitePoint, z=operator, w=contrast
            float4 params1; // x=width, y=height, z=saturation, w=unused
            float4 params2;
            float4 params3;
        };
        float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
        float3 ACESFilm(float3 x) {
            float a = 2.51; float b = 0.03; float c = 2.43; float d = 0.59; float e = 0.14;
            return saturate((x*(a*x+b)) / (x*(c*x+d)+e));
        }
        float3 FilmicUncharted2(float3 x) {
            float A=0.15, B=0.50, C=0.10, D=0.20, E=0.02, F=0.30;
            return ((x*(A*x+C*B)+D*E) / (x*(A*x+B)+D*F)) - E/F;
        }
        float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
            float3 scene = sceneTexture.Sample(linearSampler, uv).rgb;
            scene *= params0.x;
            float3 mapped;
            int op = (int)params0.z;
            if (op == 0) { // ACES
                mapped = ACESFilm(scene);
            } else if (op == 1) { // Filmic
                float3 whiteScale = 1.0 / FilmicUncharted2(params0.y);
                mapped = FilmicUncharted2(scene) * whiteScale;
            } else if (op == 2) { // Neutral
                mapped = scene / (scene + 1.0);
                mapped = pow(mapped, 1.0 / 1.1);
            } else { // Reinhard
                float lum = Luminance(scene);
                float mappedLum = lum * (1.0 + lum / (params0.y * params0.y)) / (1.0 + lum);
                mapped = scene * (mappedLum / max(lum, 0.001));
            }
            // Apply contrast
            mapped = pow(abs(mapped), params0.w) * sign(mapped);
            // Apply saturation
            float grey = Luminance(mapped);
            mapped = lerp(grey, mapped, params1.z);
            return float4(saturate(mapped), 1);
        }
    )";

        // Color Grading pixel shader (Lift/Gamma/Gain + temperature/tint)
        const char* colorGradingPS = R"(
        Texture2D sceneTexture : register(t0);
        SamplerState linearSampler : register(s0);
        cbuffer PostProcessParams : register(b0) {
            float4 params0; // x=temperature, y=tint, z=hueShift, w=saturation
            float4 params1; // x=width, y=height, z=brightness, w=contrast
            float4 params2; // xyz=lift
            float4 params3; // xyz=gain, w=unused
        };
        float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
        float3 ApplyLiftGammaGain(float3 c, float3 lift, float3 gamma, float3 gain) {
            // Lift: shifts shadows (dark values)
            c = c * gain + lift * (1.0 - c);
            // Gamma: power curve on midtones (invert gamma so >1 brightens)
            c = pow(max(c, 0.0001), 1.0 / max(gamma, 0.01));
            return c;
        }
        float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
            float3 scene = sceneTexture.Sample(linearSampler, uv).rgb;
            // Temperature/tint: approximate by shifting R/B and G/M
            scene.r += params0.x * 0.1;
            scene.b -= params0.x * 0.1;
            scene.g += params0.y * 0.05;
            // Lift/Gamma/Gain (gamma passed via separate CB in real impl, using gain.w=1 placeholder)
            float3 lift = params2.xyz;
            float3 gain = params3.xyz;
            float3 gamma = float3(1, 1, 1); // Gamma encoded in gain.w area if needed
            scene = ApplyLiftGammaGain(scene, lift, gamma, gain);
            // Brightness and contrast
            scene += params1.z;
            float midpoint = 0.5;
            scene = (scene - midpoint) * params1.w + midpoint;
            // Saturation
            float lum = Luminance(scene);
            scene = lerp(lum, scene, params0.w);
            return float4(saturate(scene), 1);
        }
    )";

        // FXAA pixel shader
        const char* fxaaPS = R"(
        Texture2D sceneTexture : register(t0);
        SamplerState linearSampler : register(s0);
        cbuffer PostProcessParams : register(b0) {
            float4 params0;
            float4 params1;
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
            float4 params0;
            float4 params1;
            float4 params2;
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
            float4 params0;
            float4 params1;
            float4 params2;
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
            float4 params0;
            float4 params1;
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
            float4 params0;
            float4 params1;
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
            float4 params0;
            float4 params1;
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
            float4 params0;
            float4 params1;
            float4 params2;
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
            float4 params0;
            float4 params1;
            float4 params2;
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
            float4 params0;
            float4 params1;
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

        // Ground Truth Ambient Occlusion pixel shader
        // Horizon-based screen-space AO with per-pixel depth-derivative normal reconstruction.
        // Output multiplies the scene color by the computed AO so the pass is self-contained
        // and can be slotted anywhere in the pipeline without a separate composite stage.
        const char* gtaoPS = R"(
        Texture2D sceneTexture : register(t0);
        Texture2D depthTexture : register(t1);
        SamplerState linearSampler : register(s0);
        SamplerState pointSampler : register(s1);
        cbuffer PostProcessParams : register(b0) {
            float4 params0; // x=radius, y=power, z=projScale, w=directions
            float4 params1; // x=width, y=height, z=stepsPerDir, w=totalTime (unused)
            float4 params2; // x=falloffStart, y=falloffEnd, z=depthThreshold, w=normalBias
            float4 params3;
        };
        static const float GTAO_PI = 3.14159265;
        float3 ReconstructNormalFromDepth(float2 uv, float2 texelSize, float centerDepth) {
            float rd = depthTexture.Sample(pointSampler, uv + float2(texelSize.x, 0)).r;
            float dd = depthTexture.Sample(pointSampler, uv + float2(0, texelSize.y)).r;
            float3 dx = float3(texelSize.x, 0.0, rd - centerDepth);
            float3 dy = float3(0.0, texelSize.y, dd - centerDepth);
            return normalize(cross(dy, dx));
        }
        float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
            float3 scene = sceneTexture.Sample(linearSampler, uv).rgb;
            float depth = depthTexture.Sample(linearSampler, uv).r;
            if (depth <= 0.0001)
                return float4(scene, 1);
            float2 texelSize = 1.0 / float2(params1.x, params1.y);
            int directions = (int)params0.w;
            int steps = (int)params1.z;
            float radius = params0.x;
            float power = params0.y;
            float projScale = max(params0.z, 0.001);
            float falloffStart = params2.x * radius;
            float falloffEnd = params2.y * radius;
            float3 normal = ReconstructNormalFromDepth(uv, texelSize, depth);
            float screenRadius = clamp(radius * projScale / max(depth, 0.001), 1.0, 256.0);
            float totalAO = 0.0;
            [loop]
            for (int d = 0; d < directions; d++) {
                float angle = GTAO_PI * (float)d / max((float)directions, 1.0);
                float2 dir = float2(cos(angle), sin(angle));
                float maxH = -1.0;
                float maxHNeg = -1.0;
                [loop]
                for (int s = 1; s <= steps; s++) {
                    float stepFrac = (float)s / max((float)steps, 1.0);
                    float2 pixOffset = dir * stepFrac * screenRadius;
                    float2 uvStep = pixOffset * texelSize;
                    float2 spUV = uv + uvStep;
                    if (all(spUV >= 0.0) && all(spUV <= 1.0)) {
                        float sd = depthTexture.Sample(linearSampler, spUV).r;
                        if (sd > 0.0001) {
                            float3 delta = float3(pixOffset / projScale * depth, sd - depth);
                            float len = length(delta);
                            if (len > 0.0001) {
                                float hc = dot(delta, normal) / len;
                                float falloff = 1.0 - saturate((len - falloffStart) /
                                                               max(falloffEnd - falloffStart, 0.001));
                                maxH = max(maxH, hc * falloff);
                            }
                        }
                    }
                    float2 spUVneg = uv - uvStep;
                    if (all(spUVneg >= 0.0) && all(spUVneg <= 1.0)) {
                        float sdn = depthTexture.Sample(linearSampler, spUVneg).r;
                        if (sdn > 0.0001) {
                            float3 deltaN = float3(-pixOffset / projScale * depth, sdn - depth);
                            float lenN = length(deltaN);
                            if (lenN > 0.0001) {
                                float hcN = dot(deltaN, normal) / lenN;
                                float falloffN = 1.0 - saturate((lenN - falloffStart) /
                                                                max(falloffEnd - falloffStart, 0.001));
                                maxHNeg = max(maxHNeg, hcN * falloffN);
                            }
                        }
                    }
                }
                float h1 = acos(clamp(maxH, -1.0, 1.0));
                float h2 = acos(clamp(maxHNeg, -1.0, 1.0));
                float vis = 0.25 * (-cos(2.0 * h1) + 2.0 * h1 + -cos(2.0 * h2) + 2.0 * h2) / GTAO_PI;
                totalAO += saturate(vis);
            }
            float ao = pow(saturate(totalAO / max((float)directions, 1.0)), max(power, 0.01));
            return float4(scene * ao, 1);
        }
    )";

        // SSAO Temporal pixel shader
        // Variance-clipped spatial denoiser that reads the AO-modulated scene
        // from the previous pass and runs a bilateral 3x3 blur whose clamp
        // window is derived from the neighborhood's per-pixel luminance
        // statistics. Until a double-buffered AO history target lands, this
        // first cut serves as the "spatial fallback" that the temporal
        // filter degrades to on first frames anyway, so it matches the
        // `SSAOTemporalFilter::Apply` behaviour on history-miss pixels.
        const char* ssaoTemporalPS = R"(
        Texture2D sceneTexture : register(t0);
        Texture2D depthTexture : register(t1);
        SamplerState linearSampler : register(s0);
        SamplerState pointSampler : register(s1);
        cbuffer PostProcessParams : register(b0) {
            float4 params0; // x=blendFactor, y=motionRejectionScale, z=depthRejectionScale, w=varianceGamma
            float4 params1; // x=width, y=height, z=useVarianceClipping (0/1), w=totalTime
            float4 params2;
            float4 params3;
        };
        float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
        float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
            float3 center = sceneTexture.Sample(linearSampler, uv).rgb;
            float2 texelSize = 1.0 / float2(max(params1.x, 1.0), max(params1.y, 1.0));
            float centerDepth = depthTexture.Sample(pointSampler, uv).r;
            float gamma = max(params0.w, 0.001);
            // Gather a 3x3 neighborhood for luminance statistics and a
            // depth-weighted bilateral blend.
            float3 mean = 0;
            float3 m2 = 0;
            float3 sum = 0;
            float weightSum = 0;
            float depthSigma = 0.02;
            [unroll]
            for (int dy = -1; dy <= 1; ++dy) {
                [unroll]
                for (int dx = -1; dx <= 1; ++dx) {
                    float2 off = float2(dx, dy) * texelSize;
                    float3 s = sceneTexture.Sample(linearSampler, uv + off).rgb;
                    mean += s;
                    m2 += s * s;
                    float sd = depthTexture.Sample(pointSampler, uv + off).r;
                    float dDiff = abs(sd - centerDepth);
                    float w = exp(-dDiff * params0.z) * exp(-(float)(dx*dx + dy*dy) * 0.25);
                    sum += s * w;
                    weightSum += w;
                }
            }
            mean /= 9.0;
            float3 variance = max(m2 / 9.0 - mean * mean, 0);
            float3 stddev = sqrt(variance);
            float3 blurred = (weightSum > 0.0001) ? (sum / weightSum) : center;
            // Variance-clip the blurred result to the neighborhood band so
            // edges stay crisp even through the denoise.
            if (params1.z > 0.5) {
                float3 clampMin = mean - stddev * gamma;
                float3 clampMax = mean + stddev * gamma;
                blurred = clamp(blurred, clampMin, clampMax);
            }
            // Blend toward the blurred AO proxy using the SSAOTemporal
            // "blendFactor" — matches the CPU reference's history weight.
            float w = saturate(params0.x);
            return float4(lerp(center, blurred, w), 1);
        }
    )";

        // Motion Blur pixel shader
        const char* motionBlurPS = R"(
        Texture2D sceneTexture : register(t0);
        Texture2D depthTexture : register(t1);
        SamplerState linearSampler : register(s0);
        cbuffer PostProcessParams : register(b0) {
            float4 params0;
            float4 params1;
            float4 params2;
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

    // =============================================================================
    // Pass Execution
    // =============================================================================

    void PostProcessingPipeline::BeginPass(ID3D11PixelShader* ps, const PostProcessCB& cb)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_context || !ps)
            return;

        int src = GetSourceTarget();
        int dst = m_currentTarget;

        // Unbind current target as SRV before binding as RTV
        ID3D11ShaderResourceView* nullSRV = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSRV);

        // Set render target
        m_context->OMSetRenderTargets(1, &m_pingPongRTVs[dst], nullptr);

        // Only set VS on the first pass — it never changes between passes.
        // The PS changes per-pass, so always set it.
        if (!m_vsAlreadyBound)
        {
            m_context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
            m_context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
            ID3D11SamplerState* samplers[] = {m_linearSampler.Get(), m_pointSampler.Get()};
            m_context->PSSetSamplers(0, 2, samplers);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->IASetInputLayout(nullptr);
            m_vsAlreadyBound = true;
        }
        m_context->PSSetShader(ps, nullptr, 0);

        // Update constant buffer (WRITE_DISCARD triggers driver buffer rename — no stall)
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData)
        {
            memcpy(mapped.pData, &cb, sizeof(PostProcessCB));
            m_context->Unmap(m_constantBuffer.Get(), 0);
        }

        // Set source texture and optional depth
        m_context->PSSetShaderResources(0, 1, &m_pingPongSRVs[src]);
        if (m_depthSRV)
            m_context->PSSetShaderResources(1, 1, &m_depthSRV);
#else
        (void)ps;
        (void)cb;
#endif
    }

    void PostProcessingPipeline::DrawFullscreen()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_context)
            return;
        // Topology and input layout already set in BeginPass (first pass only)
        m_context->Draw(3, 0);
        SwapTargets();
#endif
    }

    void PostProcessingPipeline::ProcessPass(PostProcessPass pass, float deltaTime)
    {
        auto startTime = std::chrono::high_resolution_clock::now();

        PostProcessCB cb = {};
        cb.params1.x = static_cast<float>(m_width);
        cb.params1.y = static_cast<float>(m_height);
        cb.params1.z = m_totalTime;

        ID3D11PixelShader* ps = nullptr;

        switch (pass)
        {
        case PostProcessPass::GTAO:
            // params0: x=radius, y=power, z=projScale, w=directions
            // params1: x=width, y=height (set above), z=stepsPerDir, w=totalTime (unused by GTAO)
            // params2: x=falloffStart, y=falloffEnd, z=depthThreshold, w=normalBias
            // projScale is focal_length / viewport_height. With a default 60° vertical FOV,
            // projScale = 1 / tan(30°) ≈ 1.732. Use that as a conservative default until a
            // camera feed is wired; the existing DOF/MotionBlur passes use the same pattern.
            cb.params0 = {m_gtaoSettings.radius, m_gtaoSettings.power, 1.732f,
                          static_cast<float>(std::clamp(m_gtaoSettings.directions, 1, 16))};
            cb.params1.z = static_cast<float>(std::clamp(m_gtaoSettings.stepsPerDirection, 1, 16));
            cb.params2 = {m_gtaoSettings.falloffStart, m_gtaoSettings.falloffEnd, m_gtaoSettings.depthThreshold,
                          m_gtaoSettings.normalBias};
            ps = m_gtaoPS.Get();
            break;

        case PostProcessPass::SSAOTemporal:
        {
            const auto& s = m_ssaoTemporalFilter.GetSettings();
            cb.params0 = {std::clamp(s.blendFactor, 0.0f, 1.0f), s.motionRejectionScale, s.depthRejectionScale,
                          std::max(s.varianceGamma, 0.001f)};
            cb.params1.z = s.useVarianceClipping ? 1.0f : 0.0f;
            ps = m_ssaoTemporalPS.Get();
            break;
        }

        case PostProcessPass::Bloom:
            cb.params0 = {m_bloomSettings.threshold, m_bloomSettings.softThreshold, m_bloomSettings.intensity,
                          m_bloomSettings.scatter};
            cb.params1.z = m_bloomSettings.radius;
            cb.params1.w = static_cast<float>(m_bloomSettings.iterations);
            ps = m_bloomPS.Get();
            break;

        case PostProcessPass::AutoExposure:
        {
            // CPU-side luminance adaptation: estimate target exposure from previous frame state
            // and interpolate toward it over time for smooth eye adaptation
            float targetExposure = 1.0f / std::max(m_autoExposureSettings.targetLuminance, 0.001f);
            targetExposure *= std::exp2(m_autoExposureSettings.compensationEV);
            targetExposure =
                std::clamp(targetExposure, m_autoExposureSettings.minExposure, m_autoExposureSettings.maxExposure);

            float adaptSpeed = (targetExposure > m_currentExposure) ? m_autoExposureSettings.adaptSpeedUp
                                                                    : m_autoExposureSettings.adaptSpeedDown;
            float adaptFactor = 1.0f - std::exp(-deltaTime * adaptSpeed);
            m_currentExposure += (targetExposure - m_currentExposure) * adaptFactor;

            cb.params0 = {m_currentExposure, m_autoExposureSettings.targetLuminance,
                          m_autoExposureSettings.compensationEV, 0.0f};
            ps = m_autoExposurePS.Get();
            break;
        }

        case PostProcessPass::Tonemapping:
            cb.params0 = {m_tonemappingSettings.exposure, m_tonemappingSettings.whitePoint,
                          static_cast<float>(static_cast<int>(m_tonemappingSettings.op)),
                          m_tonemappingSettings.contrast};
            cb.params1.z = m_tonemappingSettings.saturation;
            ps = m_tonemapPS.Get();
            break;

        case PostProcessPass::ColorGrading:
            cb.params0 = {m_colorGradingSettings.temperature, m_colorGradingSettings.tint,
                          m_colorGradingSettings.hueShift, m_colorGradingSettings.saturation};
            cb.params1.z = m_colorGradingSettings.brightness;
            cb.params1.w = m_colorGradingSettings.contrast;
            cb.params2 = {m_colorGradingSettings.lift.x, m_colorGradingSettings.lift.y, m_colorGradingSettings.lift.z,
                          0.0f};
            cb.params3 = {m_colorGradingSettings.gain.x, m_colorGradingSettings.gain.y, m_colorGradingSettings.gain.z,
                          0.0f};
            ps = m_colorGradingPS.Get();
            break;

        case PostProcessPass::FXAA:
            cb.params0 = {m_fxaaSettings.edgeThreshold, m_fxaaSettings.edgeThresholdMin, m_fxaaSettings.subpixelQuality,
                          0.0f};
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
#ifdef SPARK_PLATFORM_WINDOWS
            // Phase J: bracket each pass with a PIX/RenderDoc event region
            // and a GPU timestamp scope so captures and profilers group
            // work by pass name. The convenience wrappers are safe to call
            // when either subsystem was initialised without a device —
            // they degrade to no-ops.
            static const char* kPassNames[] = {
                "GTAO",           "SSAOTemporal", "Bloom",      "AutoExposure", "Tonemapping",         "ColorGrading",
                "FXAA",           "DepthOfField", "MotionBlur", "Vignette",     "ChromaticAberration", "FilmGrain",
                "LensDistortion", "LightShafts",  "LensFlare",  "Sharpen"};
            static const wchar_t* kPassNamesW[] = {L"GTAO",           L"SSAOTemporal",        L"Bloom",
                                                   L"AutoExposure",   L"Tonemapping",         L"ColorGrading",
                                                   L"FXAA",           L"DepthOfField",        L"MotionBlur",
                                                   L"Vignette",       L"ChromaticAberration", L"FilmGrain",
                                                   L"LensDistortion", L"LightShafts",         L"LensFlare",
                                                   L"Sharpen"};
            static_assert(sizeof(kPassNames) / sizeof(kPassNames[0]) == static_cast<size_t>(PostProcessPass::Count),
                          "Phase J: kPassNames must stay aligned with PostProcessPass");
            static_assert(sizeof(kPassNamesW) / sizeof(kPassNamesW[0]) == static_cast<size_t>(PostProcessPass::Count),
                          "Phase J: kPassNamesW must stay aligned with PostProcessPass");

            ScopedGPUEvent gpuEvent(m_gpuMarkers, kPassNamesW[static_cast<int>(pass)]);
            ScopedTimestamp gpuTs(m_gpuTimer, m_context, kPassNames[static_cast<int>(pass)]);
            BeginPass(ps, cb);
            DrawFullscreen();
#else
            BeginPass(ps, cb);
            DrawFullscreen();
#endif
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        float ms = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count() / 1000.0f;
        m_passTimings[static_cast<int>(pass)] = ms;
    }

#endif // !SPARK_PLATFORM_WINDOWS

} // namespace Spark::Graphics
