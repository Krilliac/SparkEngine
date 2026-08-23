/**
 * @file PostProcessingPipelineWindowsPasses.cpp
 * @brief D3D11 per-pass GPU execution for the post-processing pipeline
 *
 * BeginPass / DrawFullscreen / ProcessPass split out of
 * PostProcessingPipelineWindows.cpp (which keeps render target creation and
 * HLSL effect-shader compilation). The CPU orchestration (Initialize,
 * Process, Render, metrics, console, volume blending) remains in
 * PostProcessingPipeline.cpp.
 */

#include "PostProcessingPipeline.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "../Core/Platform.h"
#include "GPUDebugMarkers.h"
#include "GPUTimestampQuery.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace Spark::Graphics
{

    // =============================================================================
    // Pass Execution
    // =============================================================================

    bool PostProcessingPipeline::BeginPass(ID3D11PixelShader* ps, const PostProcessCB& cb)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_context || !ps)
            return false;

        const uint32_t passOrdinal = static_cast<uint32_t>(m_activePassCount);
        const int dst = PostProcessTargetRouting::DestinationForPass(passOrdinal);
        ID3D11ShaderResourceView* sourceSRV = passOrdinal == 0 ? m_inputSRV : m_pingPongSRVs[1 - dst].Get();
        ID3D11RenderTargetView* destinationRTV = m_pingPongRTVs[dst].Get();
        if (!sourceSRV || !destinationRTV)
            return false;

        // Remove both scene/depth inputs before changing outputs. This avoids
        // carrying a prior pass's SRV into a conflicting RTV binding.
        ID3D11ShaderResourceView* nullSRVs[2] = {};
        m_context->PSSetShaderResources(0, 2, nullSRVs);

        // Set render target
        m_context->OMSetRenderTargets(1, &destinationRTV, nullptr);

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
        m_context->PSSetShaderResources(0, 1, &sourceSRV);
        if (m_depthSRV)
            m_context->PSSetShaderResources(1, 1, &m_depthSRV);
        return true;
#else
        (void)ps;
        (void)cb;
        return false;
#endif
    }

    void PostProcessingPipeline::DrawFullscreen()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_context)
            return;
        // Topology and input layout already set in BeginPass (first pass only)
        m_context->Draw(3, 0);
#endif
    }

    bool PostProcessingPipeline::ProcessPass(PostProcessPass pass, float deltaTime)
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
            return false;
        }

        bool executed = false;
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
            if (BeginPass(ps, cb))
            {
                DrawFullscreen();
                executed = true;
            }
#else
            if (BeginPass(ps, cb))
            {
                DrawFullscreen();
                executed = true;
            }
#endif
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        float ms = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count() / 1000.0f;
        m_passTimings[static_cast<int>(pass)] = ms;
        return executed;
    }

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
