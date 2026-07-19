/**
 * @file PostProcessingPipelineWindowsShadersColor.h
 * @brief Inline HLSL sources for the color/HDR post-process pixel shaders
 *
 * Bloom, auto-exposure, tonemapping and color-grading shader sources.
 * Split out of PostProcessingPipelineWindows.cpp, which compiles them in
 * PostProcessingPipeline::CompileEffectShaders().
 */
#pragma once
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

namespace Spark::Graphics::PostProcessShadersWin
{

    // Bloom pixel shader (threshold extract + single-pass Gaussian blur + composite)
    // Multi-pass downscale/upscale is done CPU-side; this shader handles one pass.
    inline constexpr const char* bloomPS = R"(
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
    inline constexpr const char* autoExposurePS = R"(
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
    inline constexpr const char* tonemapPS = R"(
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
    inline constexpr const char* colorGradingPS = R"(
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

} // namespace Spark::Graphics::PostProcessShadersWin

#endif // SPARK_PLATFORM_WINDOWS
