/**
 * @file PostProcessingPipelineWindowsShadersFilter.h
 * @brief Inline HLSL sources for the filtering post-process pixel shaders
 *
 * FXAA, depth-of-field, sharpen (CAS) and motion-blur shader sources.
 * Split out of PostProcessingPipelineWindows.cpp, which compiles them in
 * PostProcessingPipeline::CompileEffectShaders().
 */
#pragma once
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

namespace Spark::Graphics::PostProcessShadersWin
{

    // FXAA pixel shader
    inline constexpr const char* fxaaPS = R"(
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
    inline constexpr const char* dofPS = R"(
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

    // Sharpen (CAS) pixel shader
    inline constexpr const char* sharpenPS = R"(
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

    // Motion Blur pixel shader
    inline constexpr const char* motionBlurPS = R"(
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

} // namespace Spark::Graphics::PostProcessShadersWin

#endif // SPARK_PLATFORM_WINDOWS
