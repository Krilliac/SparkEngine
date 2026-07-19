/**
 * @file PostProcessingPipelineWindowsShadersAO.h
 * @brief Inline HLSL sources for the ambient-occlusion post-process pixel shaders
 *
 * GTAO and SSAO temporal-denoise shader sources.
 * Split out of PostProcessingPipelineWindows.cpp, which compiles them in
 * PostProcessingPipeline::CompileEffectShaders().
 */
#pragma once
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

namespace Spark::Graphics::PostProcessShadersWin
{

    // Ground Truth Ambient Occlusion pixel shader
    // Horizon-based screen-space AO with per-pixel depth-derivative normal reconstruction.
    // Output multiplies the scene color by the computed AO so the pass is self-contained
    // and can be slotted anywhere in the pipeline without a separate composite stage.
    inline constexpr const char* gtaoPS = R"(
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
    inline constexpr const char* ssaoTemporalPS = R"(
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

} // namespace Spark::Graphics::PostProcessShadersWin

#endif // SPARK_PLATFORM_WINDOWS
