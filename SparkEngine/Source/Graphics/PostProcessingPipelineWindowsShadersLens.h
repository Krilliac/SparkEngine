/**
 * @file PostProcessingPipelineWindowsShadersLens.h
 * @brief Inline HLSL sources for the lens/camera post-process pixel shaders
 *
 * Vignette, chromatic aberration, film grain, lens distortion, light
 * shafts and lens flare shader sources.
 * Split out of PostProcessingPipelineWindows.cpp, which compiles them in
 * PostProcessingPipeline::CompileEffectShaders().
 */
#pragma once
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

namespace Spark::Graphics::PostProcessShadersWin
{

    // Vignette pixel shader
    inline constexpr const char* vignettePS = R"(
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
    inline constexpr const char* chromAbPS = R"(
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
    inline constexpr const char* filmGrainPS = R"(
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
    inline constexpr const char* lensDistPS = R"(
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
    inline constexpr const char* lightShaftsPS = R"(
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
    inline constexpr const char* lensFlarePS = R"(
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

} // namespace Spark::Graphics::PostProcessShadersWin

#endif // SPARK_PLATFORM_WINDOWS
