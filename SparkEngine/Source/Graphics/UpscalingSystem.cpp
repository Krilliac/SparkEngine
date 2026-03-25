/**
 * @file UpscalingSystem.cpp
 * @brief Upscaling system implementation: FSR 1.0/2.0 HLSL shaders, DLSS/XeSS detection, jitter utilities
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides:
 *   - Inline HLSL compute shader source for FSR 1.0 EASU and RCAS passes
 *   - Inline HLSL compute shader source for a simplified FSR 2.0-style temporal upscaler
 *   - DLSS / XeSS runtime DLL detection helpers
 *   - Halton jitter sequence generation for temporal upscaling
 *   - Resolution calculation utilities
 *
 * The UpscalingSystem class itself is fully inline in the header; this translation
 * unit houses the companion UpscalingUtils namespace and shader source strings that
 * are referenced by the rest of the graphics pipeline.
 */

#include "../Core/Platform.h"
#include "UpscalingSystem.h"
#include "../Utils/Validate.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <algorithm>
#include <numbers>

#ifdef SPARK_PLATFORM_WINDOWS
#pragma comment(lib, "d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
using namespace DirectX;
#endif

// =============================================================================
// HLSL SHADER SOURCES
// =============================================================================

namespace
{

    // -------------------------------------------------------------------------
    // FSR 1.0 EASU — Edge Adaptive Spatial Upsampling (Compute Shader)
    //
    // Simplified approximation of AMD's FidelityFX EASU pass.  Performs a
    // 12-tap directional filter in a 3x3 neighbourhood, detecting local edges
    // to steer an anisotropic Lanczos-like kernel.  Thread group: 8x8.
    // -------------------------------------------------------------------------
    static const char* kFSR1_EASU_CS = R"(
// FSR 1.0 EASU — Edge Adaptive Spatial Upsampling
// Approximate implementation for SparkEngine

cbuffer EASUConstants : register(b0)
{
    float4 Const0; // (inputWidth, inputHeight, outputWidth, outputHeight)
    float4 Const1; // (inputOffsetX, inputOffsetY, inputRegionW, inputRegionH)
    float4 Const2; // (1/inputWidth, 1/inputHeight, 1/outputWidth, 1/outputHeight)
    float4 Const3; // reserved
};

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);
SamplerState LinearClamp : register(s0);

// Compute Lanczos2 weight
float Lanczos2(float x)
{
    if (abs(x) < 1e-5)
        return 1.0;
    if (abs(x) >= 2.0)
        return 0.0;

    float pi = 3.14159265358979;
    float piX = pi * x;
    float piXHalf = piX * 0.5;
    return (sin(piX) / piX) * (sin(piXHalf) / piXHalf);
}

// Bilinear fetch with offset in texels
float3 FetchTexel(float2 baseUV, float2 offsetTexels)
{
    float2 uv = baseUV + offsetTexels * Const2.xy;
    return InputTexture.SampleLevel(LinearClamp, uv, 0.0).rgb;
}

// Compute local edge direction from a 3x3 neighbourhood
float2 ComputeEdgeDirection(float3 n, float3 s, float3 e, float3 w,
                            float3 ne, float3 nw, float3 se, float3 sw)
{
    // Luminance approximation
    float lumN  = dot(n,  float3(0.299, 0.587, 0.114));
    float lumS  = dot(s,  float3(0.299, 0.587, 0.114));
    float lumE  = dot(e,  float3(0.299, 0.587, 0.114));
    float lumW  = dot(w,  float3(0.299, 0.587, 0.114));
    float lumNE = dot(ne, float3(0.299, 0.587, 0.114));
    float lumNW = dot(nw, float3(0.299, 0.587, 0.114));
    float lumSE = dot(se, float3(0.299, 0.587, 0.114));
    float lumSW = dot(sw, float3(0.299, 0.587, 0.114));

    // Sobel-like gradient
    float dx = -lumNW - 2.0 * lumW - lumSW + lumNE + 2.0 * lumE + lumSE;
    float dy = -lumNW - 2.0 * lumN - lumNE + lumSW + 2.0 * lumS + lumSE;

    float len = max(sqrt(dx * dx + dy * dy), 1e-6);
    return float2(dx / len, dy / len);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    float2 outputSize = Const0.zw;

    if (dtid.x >= (uint)outputSize.x || dtid.y >= (uint)outputSize.y)
        return;

    // Map output pixel to input UV
    float2 outputPixel = float2(dtid.x, dtid.y) + 0.5;
    float2 inputUV = outputPixel * Const2.zw; // output pixel / output size
    // Scale to input space
    float2 srcUV = inputUV; // maps [0,1] -> [0,1]

    // Gather 3x3 neighbourhood
    float3 c  = FetchTexel(srcUV, float2( 0.0,  0.0));
    float3 n  = FetchTexel(srcUV, float2( 0.0, -1.0));
    float3 s  = FetchTexel(srcUV, float2( 0.0,  1.0));
    float3 e  = FetchTexel(srcUV, float2( 1.0,  0.0));
    float3 w  = FetchTexel(srcUV, float2(-1.0,  0.0));
    float3 ne = FetchTexel(srcUV, float2( 1.0, -1.0));
    float3 nw = FetchTexel(srcUV, float2(-1.0, -1.0));
    float3 se = FetchTexel(srcUV, float2( 1.0,  1.0));
    float3 sw = FetchTexel(srcUV, float2(-1.0,  1.0));

    // Compute edge direction for anisotropic filtering
    float2 edgeDir = ComputeEdgeDirection(n, s, e, w, ne, nw, se, sw);

    // Fractional position within the source texel for sub-pixel offset
    float2 srcTexel = srcUV * Const0.xy; // UV -> texel coordinates
    float2 frac_pos = frac(srcTexel) - 0.5;

    // 12-tap directional Lanczos-like filter
    // Taps aligned along and perpendicular to the edge direction
    float2 along = edgeDir;
    float2 perp  = float2(-edgeDir.y, edgeDir.x);

    float3 result = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;

    // Sample offsets: 4 along edge, 4 perpendicular, 4 diagonal
    static const float2 offsets[12] = {
        float2( 0.0,  0.0),
        float2( 1.0,  0.0),
        float2(-1.0,  0.0),
        float2( 0.0,  1.0),
        float2( 0.0, -1.0),
        float2( 1.0,  1.0),
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 0.5,  0.0),
        float2(-0.5,  0.0),
        float2( 0.0,  0.5)
    };

    [unroll]
    for (int i = 0; i < 12; i++)
    {
        float2 offset = offsets[i];
        float2 samplePos = offset - frac_pos;

        // Project onto edge-aligned axes for anisotropic weighting
        float distAlong = abs(dot(samplePos, along));
        float distPerp  = abs(dot(samplePos, perp));

        // Lanczos weight with edge-aware stretch
        float wAlong = Lanczos2(distAlong);
        float wPerp  = Lanczos2(distPerp * 1.5); // Tighter filtering across edges
        float weight  = wAlong * wPerp;

        float3 tap = FetchTexel(srcUV, offset);
        result += tap * weight;
        totalWeight += weight;
    }

    result /= max(totalWeight, 1e-6);

    OutputTexture[dtid.xy] = float4(result, 1.0);
}
)";

    // -------------------------------------------------------------------------
    // FSR 1.0 RCAS — Robust Contrast Adaptive Sharpening (Compute Shader)
    //
    // Simplified CAS pass: applies contrast-adaptive sharpening that avoids
    // amplifying noise in low-contrast areas.  Thread group: 8x8.
    // -------------------------------------------------------------------------
    static const char* kFSR1_RCAS_CS = R"(
// FSR 1.0 RCAS — Robust Contrast Adaptive Sharpening
// Approximate implementation for SparkEngine

cbuffer RCASConstants : register(b0)
{
    float4 Const0; // (attenuation, 0, 0, 0)
};

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

float Luma(float3 color)
{
    return dot(color, float3(0.299, 0.587, 0.114));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint width, height;
    InputTexture.GetDimensions(width, height);

    if (dtid.x >= width || dtid.y >= height)
        return;

    int2 pos = int2(dtid.xy);

    // Load centre and 4-connected neighbours
    float3 c = InputTexture.Load(int3(pos, 0)).rgb;
    float3 n = InputTexture.Load(int3(pos + int2( 0, -1), 0)).rgb;
    float3 s = InputTexture.Load(int3(pos + int2( 0,  1), 0)).rgb;
    float3 e = InputTexture.Load(int3(pos + int2( 1,  0), 0)).rgb;
    float3 w = InputTexture.Load(int3(pos + int2(-1,  0), 0)).rgb;

    // Compute min / max luma in cross neighbourhood
    float lumC = Luma(c);
    float lumN = Luma(n);
    float lumS = Luma(s);
    float lumE = Luma(e);
    float lumW = Luma(w);

    float minLuma = min(lumC, min(min(lumN, lumS), min(lumE, lumW)));
    float maxLuma = max(lumC, max(max(lumN, lumS), max(lumE, lumW)));

    // Local contrast ratio drives sharpening strength
    float contrast = maxLuma - minLuma;
    float peakRange = max(maxLuma, 1e-6);
    float ratio = contrast / peakRange;

    // Attenuation maps [0,2]: 0 = full sharpen, 2 = no sharpen
    float attenuation = Const0.x;

    // Sharpening weight: high contrast => sharpen, attenuated by user control
    float sharpWeight = max(0.0, 1.0 - ratio * attenuation);
    sharpWeight = sharpWeight * sharpWeight; // Smooth falloff

    // Clamp to a reasonable range to avoid halos
    sharpWeight = min(sharpWeight, 0.5);

    // Compute unsharp mask: centre - average of neighbours
    float3 avg = (n + s + e + w) * 0.25;
    float3 detail = c - avg;

    // Apply sharpening
    float3 result = c + detail * sharpWeight;

    // Clamp to prevent ringing
    float3 minNeighbour = min(c, min(min(n, s), min(e, w)));
    float3 maxNeighbour = max(c, max(max(n, s), max(e, w)));
    result = clamp(result, minNeighbour, maxNeighbour);

    OutputTexture[dtid.xy] = float4(result, 1.0);
}
)";

    // -------------------------------------------------------------------------
    // Temporal Upscaling Compute Shader (simplified FSR 2.0-style)
    //
    // Single-pass temporal upscaler that reprojects the previous frame using
    // motion vectors, performs disocclusion detection via depth comparison,
    // and blends temporal history with the current frame.  Thread group: 8x8.
    // -------------------------------------------------------------------------
    static const char* kTemporalUpscaling_CS = R"(
// Simplified temporal upscaling (FSR 2.0 style)
// Single-pass approximation for SparkEngine

cbuffer TemporalConstants : register(b0)
{
    float4 InputOutputSize;    // (renderW, renderH, displayW, displayH)
    float4 InputSizeRcp;       // (1/renderW, 1/renderH, 1/displayW, 1/displayH)
    float4 JitterOffset;       // (jitterX, jitterY, motionScaleX, motionScaleY)
    float4 TemporalParams;     // (deltaTime, nearPlane, farPlane, verticalFOV)
    float4 Flags;              // (resetAccumulation, frameIndex, 0, 0)
};

Texture2D<float4> ColorInput        : register(t0);
Texture2D<float>  DepthInput        : register(t1);
Texture2D<float2> MotionVectors     : register(t2);
Texture2D<float>  ExposureInput     : register(t3);
Texture2D<float>  ReactiveMask      : register(t4);
Texture2D<float4> HistoryTexture    : register(t5);
RWTexture2D<float4> OutputTexture   : register(u0);
SamplerState LinearClamp            : register(s0);

float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float LinearizeDepth(float d, float nearZ, float farZ)
{
    return nearZ * farZ / (farZ - d * (farZ - nearZ));
}

// Catmull-Rom weight for high-quality history sampling
float CatmullRomWeight(float x)
{
    float ax = abs(x);
    if (ax < 1.0)
        return 0.5 * (2.0 + ax * ax * (-5.0 + ax * 3.0));
    if (ax < 2.0)
        return 0.5 * (4.0 + ax * (-8.0 + ax * (5.0 - ax)));
    return 0.0;
}

// Sample history with Catmull-Rom 4-tap (separable)
float3 SampleHistoryCatmullRom(float2 uv)
{
    float2 texelSize = InputSizeRcp.zw;
    float2 samplePos = uv / texelSize - 0.5;
    float2 f = frac(samplePos);
    float2 texelBase = (floor(samplePos) + 0.5) * texelSize;

    float3 result = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;

    [unroll]
    for (int y = -1; y <= 2; y++)
    {
        float wy = CatmullRomWeight(f.y - (float)y);
        [unroll]
        for (int x = -1; x <= 2; x++)
        {
            float wx = CatmullRomWeight(f.x - (float)x);
            float weight = wx * wy;
            float2 offset = float2((float)x, (float)y) * texelSize;
            float3 tap = HistoryTexture.SampleLevel(LinearClamp, texelBase + offset, 0.0).rgb;
            result += tap * weight;
            totalWeight += weight;
        }
    }

    return result / max(totalWeight, 1e-6);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    float displayW = InputOutputSize.z;
    float displayH = InputOutputSize.w;

    if (dtid.x >= (uint)displayW || dtid.y >= (uint)displayH)
        return;

    float2 outputUV = (float2(dtid.xy) + 0.5) * InputSizeRcp.zw;

    // Map output pixel back to input space (accounting for resolution difference)
    float2 inputUV = outputUV;
    float2 jitteredUV = inputUV - JitterOffset.xy * InputSizeRcp.xy;

    // Fetch current frame colour (with jitter removal)
    float3 currentColor = ColorInput.SampleLevel(LinearClamp, jitteredUV, 0.0).rgb;

    // Fetch motion vector at current position
    float2 motion = MotionVectors.SampleLevel(LinearClamp, jitteredUV, 0.0).xy;
    motion *= JitterOffset.zw; // Apply motion vector scale

    // Reproject to find where this pixel was in the previous frame
    float2 historyUV = outputUV - motion;

    // Fetch depth for disocclusion detection
    float depth = DepthInput.SampleLevel(LinearClamp, jitteredUV, 0.0).x;
    float linearDepth = LinearizeDepth(depth, TemporalParams.y, TemporalParams.z);

    // Fetch reactive mask (particles, transparency)
    float reactive = ReactiveMask.SampleLevel(LinearClamp, jitteredUV, 0.0).x;

    // Read exposure for luminance normalization
    float exposure = max(ExposureInput.SampleLevel(LinearClamp, float2(0.5, 0.5), 0.0).x, 0.001);
    currentColor *= exposure;

    // Check if history UV is valid (within screen bounds)
    bool historyValid = historyUV.x >= 0.0 && historyUV.x <= 1.0 &&
                        historyUV.y >= 0.0 && historyUV.y <= 1.0;

    // Reset temporal accumulation if requested
    bool resetAccum = Flags.x > 0.5;

    float3 result;

    if (!historyValid || resetAccum)
    {
        // No valid history — use current frame only
        result = currentColor;
    }
    else
    {
        // Sample history with high-quality Catmull-Rom filtering
        float3 historyColor = SampleHistoryCatmullRom(historyUV);

        // Neighbourhood colour clamping to prevent ghosting
        // Gather a 3x3 neighbourhood around the current input texel
        float3 minColor = currentColor;
        float3 maxColor = currentColor;

        [unroll]
        for (int dy = -1; dy <= 1; dy++)
        {
            [unroll]
            for (int dx = -1; dx <= 1; dx++)
            {
                if (dx == 0 && dy == 0)
                    continue;
                float2 offset = float2((float)dx, (float)dy) * InputSizeRcp.xy;
                float3 neighbour = ColorInput.SampleLevel(
                    LinearClamp, jitteredUV + offset, 0.0).rgb * exposure;
                minColor = min(minColor, neighbour);
                maxColor = max(maxColor, neighbour);
            }
        }

        // Expand clamp box slightly to reduce flickering
        float3 boxCenter = (minColor + maxColor) * 0.5;
        float3 boxExtent = (maxColor - minColor) * 0.5;
        minColor = boxCenter - boxExtent * 1.25;
        maxColor = boxCenter + boxExtent * 1.25;

        // Clamp history to the neighbourhood colour box
        historyColor = clamp(historyColor, minColor, maxColor);

        // Disocclusion detection: compare reprojected depth
        float historyDepth = DepthInput.SampleLevel(LinearClamp, historyUV, 0.0).x;
        float historyLinearDepth = LinearizeDepth(historyDepth, TemporalParams.y, TemporalParams.z);
        float depthDelta = abs(linearDepth - historyLinearDepth) / max(linearDepth, 0.001);
        float disocclusion = saturate(depthDelta * 10.0);

        // Blend factor: prefer history for stability, bias towards current on disocclusion
        float blendFactor = lerp(0.05, 1.0, disocclusion);

        // Reactive mask forces use of current colour (particles, alpha-blended objects)
        blendFactor = lerp(blendFactor, 1.0, reactive);

        // Luminance-based weight adjustment: reduce history when luminance changes sharply
        float lumCurrent = Luminance(currentColor);
        float lumHistory = Luminance(historyColor);
        float lumDelta = abs(lumCurrent - lumHistory) / max(lumCurrent + lumHistory, 0.001);
        blendFactor = max(blendFactor, lumDelta * 0.5);

        blendFactor = clamp(blendFactor, 0.03, 1.0);

        result = lerp(historyColor, currentColor, blendFactor);
    }

    // Remove exposure before output
    result /= exposure;

    OutputTexture[dtid.xy] = float4(max(result, 0.0), 1.0);
}
)";

    // -------------------------------------------------------------------------
    // SparkSR — SparkEngine Native Temporal Upscaling (Compute Shader)
    //
    // Engine-native temporal upscaler with no vendor SDK dependency.
    // Key improvements over the basic temporal upscaler:
    //   - YCoCg color space for perceptually accurate neighborhood clamping
    //   - Variance-based clip (mean ± gamma * stddev) instead of min/max AABB
    //   - Motion vector confidence weighting (fast motion = less history)
    //   - Combined depth + motion divergence disocclusion detection
    //   - Jitter delta tracking for improved sub-pixel reprojection
    //   - Catmull-Rom 4-tap history sampling
    // Thread group: 8x8.  Two-pass pipeline: this shader + RCAS sharpening.
    // -------------------------------------------------------------------------
    static const char* kSparkSR_CS = R"(
// SparkSR — SparkEngine Native Temporal Upscaling
// Vendor-independent temporal accumulation with YCoCg variance clip

cbuffer SparkSRConstants : register(b0)
{
    float4 RenderSize;     // (renderW, renderH, 1/renderW, 1/renderH)
    float4 DisplaySize;    // (displayW, displayH, 1/displayW, 1/displayH)
    float4 JitterOffset;   // (jitterX, jitterY, prevJitterX, prevJitterY)
    float4 TemporalParams; // (frameIndex, resetFlag, sharpness, blendMin)
    float4 MotionParams;   // (motionScaleX, motionScaleY, depthThreshold, varianceGamma)
};

Texture2D<float4> ColorInput        : register(t0);
Texture2D<float>  DepthInput        : register(t1);
Texture2D<float2> MotionVectors     : register(t2);
Texture2D<float>  ExposureInput     : register(t3);
Texture2D<float>  ReactiveMask      : register(t4);
Texture2D<float4> HistoryTexture    : register(t5);
RWTexture2D<float4> OutputTexture   : register(u0);
SamplerState LinearClamp            : register(s0);

// --- Color space conversion ---

float3 RGBToYCoCg(float3 rgb)
{
    float y  = dot(rgb, float3(0.25, 0.5, 0.25));
    float co = dot(rgb, float3(0.5, 0.0, -0.5));
    float cg = dot(rgb, float3(-0.25, 0.5, -0.25));
    return float3(y, co, cg);
}

float3 YCoCgToRGB(float3 ycocg)
{
    float y  = ycocg.x;
    float co = ycocg.y;
    float cg = ycocg.z;
    return float3(y + co - cg, y + cg, y - co - cg);
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float LinearizeDepth(float d, float nearZ, float farZ)
{
    return nearZ * farZ / (farZ - d * (farZ - nearZ));
}

// --- Variance-based AABB clip ---
// Clips the history sample toward the center of a variance-derived AABB
// This is tighter than min/max and reduces ghosting significantly

float3 ClipToVarianceAABB(float3 history, float3 aabbCenter, float3 aabbExtent)
{
    float3 offset = history - aabbCenter;
    float3 absExtent = max(aabbExtent, 0.0001);
    float3 ts = abs(offset) / absExtent;
    float maxT = max(ts.x, max(ts.y, ts.z));

    if (maxT > 1.0)
    {
        return aabbCenter + offset / maxT;
    }
    return history;
}

// --- Catmull-Rom 4-tap history sampling ---

float CatmullRomWeight(float x)
{
    float ax = abs(x);
    if (ax < 1.0)
        return 0.5 * (2.0 + ax * ax * (-5.0 + ax * 3.0));
    if (ax < 2.0)
        return 0.5 * (4.0 + ax * (-8.0 + ax * (5.0 - ax)));
    return 0.0;
}

float3 SampleHistoryCatmullRom(float2 uv)
{
    float2 texelSize = DisplaySize.zw;
    float2 samplePos = uv / texelSize - 0.5;
    float2 f = frac(samplePos);
    float2 texelBase = (floor(samplePos) + 0.5) * texelSize;

    float3 result = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;

    [unroll]
    for (int y = -1; y <= 2; y++)
    {
        float wy = CatmullRomWeight(f.y - (float)y);
        [unroll]
        for (int x = -1; x <= 2; x++)
        {
            float wx = CatmullRomWeight(f.x - (float)x);
            float weight = wx * wy;
            float2 offset = float2((float)x, (float)y) * texelSize;
            float3 tap = HistoryTexture.SampleLevel(LinearClamp, texelBase + offset, 0.0).rgb;
            result += tap * weight;
            totalWeight += weight;
        }
    }

    return result / max(totalWeight, 1e-6);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= (uint)DisplaySize.x || dtid.y >= (uint)DisplaySize.y)
        return;

    float2 outputUV = (float2(dtid.xy) + 0.5) * DisplaySize.zw;

    // De-jitter: remove current frame's jitter to get the true sample position
    float2 inputUV = outputUV;
    float2 jitteredUV = inputUV - JitterOffset.xy * RenderSize.zw;

    // Fetch current frame color
    float3 currentColor = ColorInput.SampleLevel(LinearClamp, jitteredUV, 0.0).rgb;

    // Apply exposure normalization
    float exposure = max(ExposureInput.SampleLevel(LinearClamp, float2(0.5, 0.5), 0.0).x, 0.001);
    currentColor *= exposure;

    // Fetch motion vector and apply scale
    float2 motion = MotionVectors.SampleLevel(LinearClamp, jitteredUV, 0.0).xy;
    motion *= MotionParams.xy;

    // Reproject to find previous frame position
    float2 historyUV = outputUV - motion;

    // Fetch depth and reactive mask
    float depth = DepthInput.SampleLevel(LinearClamp, jitteredUV, 0.0).x;
    float reactive = ReactiveMask.SampleLevel(LinearClamp, jitteredUV, 0.0).x;

    // Check history validity
    bool historyValid = historyUV.x >= 0.0 && historyUV.x <= 1.0 &&
                        historyUV.y >= 0.0 && historyUV.y <= 1.0;
    bool resetAccum = TemporalParams.y > 0.5;

    float3 result;

    if (!historyValid || resetAccum)
    {
        result = currentColor;
    }
    else
    {
        // --- Gather 3x3 neighborhood in YCoCg for variance clip ---
        float3 m1 = float3(0.0, 0.0, 0.0); // sum
        float3 m2 = float3(0.0, 0.0, 0.0); // sum of squares
        float3 currentYCoCg = RGBToYCoCg(currentColor);

        [unroll]
        for (int dy = -1; dy <= 1; dy++)
        {
            [unroll]
            for (int dx = -1; dx <= 1; dx++)
            {
                float2 offset = float2((float)dx, (float)dy) * RenderSize.zw;
                float3 s = ColorInput.SampleLevel(LinearClamp, jitteredUV + offset, 0.0).rgb * exposure;
                float3 sYCoCg = RGBToYCoCg(s);
                m1 += sYCoCg;
                m2 += sYCoCg * sYCoCg;
            }
        }

        // Compute mean and standard deviation
        float3 mean = m1 / 9.0;
        float3 variance = abs(m2 / 9.0 - mean * mean);
        float3 stddev = sqrt(variance);

        // Variance gamma controls how tight the clip box is
        // Lower gamma = tighter = less ghosting but more flickering
        float gamma = MotionParams.w; // typically 1.0 - 1.25

        // Sample history with Catmull-Rom for sub-pixel accuracy
        float3 historyColor = SampleHistoryCatmullRom(historyUV);
        historyColor *= exposure; // normalize history to current exposure
        float3 historyYCoCg = RGBToYCoCg(historyColor);

        // Clip history to variance-derived AABB in YCoCg space
        float3 aabbExtent = stddev * gamma;
        historyYCoCg = ClipToVarianceAABB(historyYCoCg, mean, aabbExtent);
        historyColor = YCoCgToRGB(historyYCoCg);

        // --- Disocclusion detection: depth + motion divergence ---
        float historyDepth = DepthInput.SampleLevel(LinearClamp, historyUV, 0.0).x;
        float depthDelta = abs(depth - historyDepth);
        float depthDisocclusion = saturate(depthDelta / max(MotionParams.z, 0.001));

        // Motion divergence: compare motion at current vs reprojected position
        float2 historyMotion = MotionVectors.SampleLevel(LinearClamp, historyUV, 0.0).xy * MotionParams.xy;
        float motionDivergence = length(motion - historyMotion);
        float motionDisocclusion = saturate(motionDivergence * 20.0);

        float disocclusion = max(depthDisocclusion, motionDisocclusion);

        // --- Motion confidence: fast motion = prefer current frame ---
        float motionLength = length(motion);
        float motionConfidence = saturate(motionLength * 10.0);

        // --- Compute blend factor ---
        float blendMin = TemporalParams.w; // minimum blend (e.g. 0.03)
        float blendFactor = blendMin;

        // Increase blend toward current on disocclusion
        blendFactor = lerp(blendFactor, 1.0, disocclusion);

        // Increase blend for fast-moving pixels
        blendFactor = lerp(blendFactor, max(blendFactor, 0.3), motionConfidence);

        // Reactive mask forces use of current color (particles, transparency)
        blendFactor = lerp(blendFactor, 1.0, reactive);

        // Luminance stability: large luminance shifts bias toward current
        float lumCurrent = Luminance(currentColor);
        float lumHistory = Luminance(historyColor);
        float lumDelta = abs(lumCurrent - lumHistory) / max(lumCurrent + lumHistory, 0.001);
        blendFactor = max(blendFactor, lumDelta * 0.4);

        blendFactor = clamp(blendFactor, blendMin, 1.0);

        result = lerp(historyColor, currentColor, blendFactor);
    }

    // Remove exposure normalization before output
    result /= exposure;

    OutputTexture[dtid.xy] = float4(max(result, 0.0), 1.0);
}
)";

} // anonymous namespace

// =============================================================================
// UPSCALING UTILITIES — Cross-platform helpers
// =============================================================================

namespace Spark
{
    namespace Graphics
    {
        namespace UpscalingUtils
        {

            // -------------------------------------------------------------------------
            // Halton Sequence
            // -------------------------------------------------------------------------

            /**
 * @brief Generate an element of a Halton low-discrepancy sequence
 *
 * Used to produce jitter offsets for temporal upscaling and TAA.
 * The Halton sequence provides well-distributed quasi-random sub-pixel
 * offsets across frames, minimising aliasing patterns.
 *
 * @param index  Frame index (0-based)
 * @param base   Prime base (use 2 for X, 3 for Y)
 * @return Value in [0, 1)
 */
            float GenerateHaltonSequence(uint32_t index, uint32_t base)
            {
                float result = 0.0f;
                float fraction = 1.0f / static_cast<float>(base);
                uint32_t i = index + 1; // Halton is 1-indexed

                while (i > 0)
                {
                    result += fraction * static_cast<float>(i % base);
                    i /= base;
                    fraction /= static_cast<float>(base);
                }

                return result;
            }

            // -------------------------------------------------------------------------
            // Jitter Offset Calculation
            // -------------------------------------------------------------------------

            /**
 * @brief Calculate sub-pixel jitter offset for temporal upscaling
 *
 * Returns a jitter offset in pixels using a Halton(2,3) sequence,
 * centred around zero (range [-0.5, 0.5] in render pixels).
 * This offset should be applied to the projection matrix before rendering
 * and provided to the upscaler for de-jittering.
 *
 * @param frameIndex   Current frame index
 * @param renderWidth  Internal render resolution width
 * @param renderHeight Internal render resolution height
 * @return Pair of (jitterX, jitterY) in pixel units
 */
            std::pair<float, float> CalculateJitterOffset(uint32_t frameIndex, uint32_t renderWidth,
                                                          uint32_t renderHeight)
            {
                // Use a 16-phase Halton sequence to keep the pattern periodic
                constexpr uint32_t kJitterPhaseCount = 16;
                uint32_t phaseIndex = frameIndex % kJitterPhaseCount;

                // Generate Halton values in [0, 1) and centre around zero
                float haltonX = GenerateHaltonSequence(phaseIndex, 2) - 0.5f;
                float haltonY = GenerateHaltonSequence(phaseIndex, 3) - 0.5f;

                // The jitter is in render-resolution pixel units
                // For projection matrix application, convert to NDC:
                //   ndcJitterX = jitterX * 2.0 / renderWidth
                //   ndcJitterY = jitterY * 2.0 / renderHeight
                (void)renderWidth;
                (void)renderHeight;

                return {haltonX, haltonY};
            }

            // -------------------------------------------------------------------------
            // DLL-based Feature Detection
            // -------------------------------------------------------------------------

            /**
 * @brief Check if NVIDIA DLSS is available on the system
 *
 * Checks for the presence of the NVIDIA NGX DLSS runtime DLL.
 * Actual DLSS evaluation also requires a compatible RTX GPU and
 * an appropriate driver version, but this serves as a first-pass gate.
 *
 * @return true if nvngx_dlss.dll is found
 */
            bool DetectDLSSAvailability()
            {
#ifdef SPARK_PLATFORM_WINDOWS
                // Try to load the DLSS DLL without executing DllMain entry point code
                HMODULE hModule = LoadLibraryExA("nvngx_dlss.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
                if (hModule != nullptr)
                {
                    FreeLibrary(hModule);
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DLSS runtime detected (nvngx_dlss.dll)");
                    return true;
                }

                // Also check the common NGX runtime path
                hModule = LoadLibraryExA("_nvngx.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
                if (hModule != nullptr)
                {
                    FreeLibrary(hModule);
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DLSS runtime detected (_nvngx.dll)");
                    return true;
                }

                SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "DLSS runtime not found");
                return false;
#else
                // DLSS is Windows + NVIDIA only
                return false;
#endif
            }

            /**
 * @brief Check if Intel XeSS is available on the system
 *
 * Checks for the presence of the Intel XeSS runtime DLL.
 * XeSS can run on any GPU that supports DP4a (most modern GPUs),
 * but is accelerated on Intel Arc GPUs with XMX hardware.
 *
 * @return true if libxess.dll is found
 */
            bool DetectXeSSAvailability()
            {
#ifdef SPARK_PLATFORM_WINDOWS
                HMODULE hModule = LoadLibraryExA("libxess.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
                if (hModule != nullptr)
                {
                    FreeLibrary(hModule);
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "XeSS runtime detected (libxess.dll)");
                    return true;
                }

                SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "XeSS runtime not found");
                return false;
#else
                // XeSS DX11/DX12 runtime is Windows only
                return false;
#endif
            }

            // -------------------------------------------------------------------------
            // Optimal Render Resolution
            // -------------------------------------------------------------------------

            /**
 * @brief Calculate the optimal render resolution for a given upscaling configuration
 *
 * Returns the internal render resolution that the scene should be rendered at
 * before upscaling to the target display resolution.  Ensures even dimensions
 * for compute shader dispatch alignment.
 *
 * @param mode          Active upscaling mode
 * @param quality       Quality preset
 * @param displayWidth  Target display width
 * @param displayHeight Target display height
 * @return Pair of (renderWidth, renderHeight)
 */
            std::pair<uint32_t, uint32_t> GetOptimalRenderResolution(UpscalingMode mode, UpscalingQuality quality,
                                                                     uint32_t displayWidth, uint32_t displayHeight)
            {
                if (mode == UpscalingMode::None)
                {
                    return {displayWidth, displayHeight};
                }

                float scale = UpscalingSettings::GetRenderScale(quality);

                uint32_t renderWidth = std::max(1u, static_cast<uint32_t>(static_cast<float>(displayWidth) * scale));
                uint32_t renderHeight = std::max(1u, static_cast<uint32_t>(static_cast<float>(displayHeight) * scale));

                // Align to even dimensions for 8x8 thread group dispatch
                renderWidth = (renderWidth + 1u) & ~1u;
                renderHeight = (renderHeight + 1u) & ~1u;

                return {renderWidth, renderHeight};
            }

            // -------------------------------------------------------------------------
            // FSR 1.0 Constant Buffer Helpers
            // -------------------------------------------------------------------------

#ifdef SPARK_PLATFORM_WINDOWS

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

#endif // SPARK_PLATFORM_WINDOWS

            // -------------------------------------------------------------------------
            // Shader Source Accessors
            // -------------------------------------------------------------------------

            /**
 * @brief Get the FSR 1.0 EASU compute shader HLSL source
 * @return Null-terminated HLSL string
 */
            const char* GetFSR1EASUShaderSource()
            {
                return kFSR1_EASU_CS;
            }

            /**
 * @brief Get the FSR 1.0 RCAS compute shader HLSL source
 * @return Null-terminated HLSL string
 */
            const char* GetFSR1RCASShaderSource()
            {
                return kFSR1_RCAS_CS;
            }

            /**
 * @brief Get the temporal upscaling compute shader HLSL source
 * @return Null-terminated HLSL string
 */
            const char* GetTemporalUpscalingShaderSource()
            {
                return kTemporalUpscaling_CS;
            }

            /**
             * @brief Get the SparkSR temporal upscaling compute shader HLSL source
             * @return Null-terminated HLSL string
             */
            const char* GetSparkSRShaderSource()
            {
                return kSparkSR_CS;
            }

            // -------------------------------------------------------------------------
            // Shader Compilation Helper
            // -------------------------------------------------------------------------

#ifdef SPARK_PLATFORM_WINDOWS

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

                if (!CompileComputeShader(device, kFSR1_EASU_CS, "CSMain", outEASU))
                {
                    return false;
                }

                if (!CompileComputeShader(device, kFSR1_RCAS_CS, "CSMain", outRCAS))
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

                return CompileComputeShader(device, kTemporalUpscaling_CS, "CSMain", outShader);
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

                return CompileComputeShader(device, kSparkSR_CS, "CSMain", outShader);
            }

#endif // SPARK_PLATFORM_WINDOWS

            // -------------------------------------------------------------------------
            // Resolution Scaling Tables
            // -------------------------------------------------------------------------

            /**
 * @brief Get the human-readable name for an upscaling mode
 * @param mode The upscaling mode
 * @return String name (e.g. "FSR 1.0", "DLSS")
 */
            const char* GetUpscalingModeName(UpscalingMode mode)
            {
                switch (mode)
                {
                case UpscalingMode::None:
                    return "None";
                case UpscalingMode::FSR1:
                    return "FSR 1.0";
                case UpscalingMode::FSR2:
                    return "FSR 2.0";
                case UpscalingMode::DLSS:
                    return "DLSS";
                case UpscalingMode::XeSS:
                    return "XeSS";
                case UpscalingMode::SparkSR:
                    return "SparkSR";
                default:
                    return "Unknown";
                }
            }

            /**
 * @brief Get the human-readable name for a quality preset
 * @param quality The quality preset
 * @return String name (e.g. "Quality", "Performance")
 */
            const char* GetUpscalingQualityName(UpscalingQuality quality)
            {
                switch (quality)
                {
                case UpscalingQuality::UltraPerformance:
                    return "Ultra Performance";
                case UpscalingQuality::Performance:
                    return "Performance";
                case UpscalingQuality::Balanced:
                    return "Balanced";
                case UpscalingQuality::Quality:
                    return "Quality";
                case UpscalingQuality::UltraQuality:
                    return "Ultra Quality";
                case UpscalingQuality::Native:
                    return "Native";
                default:
                    return "Unknown";
                }
            }

            /**
 * @brief Check if a given upscaling mode requires temporal inputs
 *
 * Temporal modes need depth, motion vectors, and jitter offsets in
 * addition to the colour buffer.
 *
 * @param mode The upscaling mode to check
 * @return true if mode is temporal (FSR2, DLSS, XeSS)
 */
            bool IsTemporalMode(UpscalingMode mode)
            {
                switch (mode)
                {
                case UpscalingMode::FSR2:
                case UpscalingMode::DLSS:
                case UpscalingMode::XeSS:
                case UpscalingMode::SparkSR:
                    return true;
                default:
                    return false;
                }
            }

            /**
 * @brief Get the recommended jitter phase count for a quality preset
 *
 * Higher quality presets use fewer jitter phases because the render
 * resolution is closer to display and temporal convergence is faster.
 * Lower quality presets (higher upscale factor) need more samples to
 * reconstruct missing detail.
 *
 * @param quality Quality preset
 * @return Number of jitter phases in the Halton sequence
 */
            uint32_t GetRecommendedJitterPhaseCount(UpscalingQuality quality)
            {
                switch (quality)
                {
                case UpscalingQuality::UltraPerformance:
                    return 32;
                case UpscalingQuality::Performance:
                    return 23;
                case UpscalingQuality::Balanced:
                    return 18;
                case UpscalingQuality::Quality:
                    return 16;
                case UpscalingQuality::UltraQuality:
                    return 12;
                case UpscalingQuality::Native:
                    return 8;
                default:
                    return 16;
                }
            }

            /**
 * @brief Calculate a jitter offset using the recommended phase count for quality
 *
 * Convenience wrapper that selects the appropriate Halton phase count based
 * on the quality preset and returns the pixel-space jitter offset.
 *
 * @param frameIndex   Current frame index
 * @param quality      Active quality preset
 * @param renderWidth  Internal render resolution width
 * @param renderHeight Internal render resolution height
 * @return Pair of (jitterX, jitterY) in pixel units
 */
            std::pair<float, float> CalculateJitterForQuality(uint32_t frameIndex, UpscalingQuality quality,
                                                              uint32_t renderWidth, uint32_t renderHeight)
            {
                uint32_t phaseCount = GetRecommendedJitterPhaseCount(quality);
                uint32_t phaseIndex = frameIndex % phaseCount;

                float haltonX = GenerateHaltonSequence(phaseIndex, 2) - 0.5f;
                float haltonY = GenerateHaltonSequence(phaseIndex, 3) - 0.5f;

                (void)renderWidth;
                (void)renderHeight;

                return {haltonX, haltonY};
            }

            /**
 * @brief Convert a pixel-space jitter offset to NDC jitter for the projection matrix
 *
 * The projection matrix jitter should be applied as a translation in clip space:
 *   projection[2][0] += ndcJitterX;
 *   projection[2][1] += ndcJitterY;
 *
 * @param jitterX      Jitter X in render pixels
 * @param jitterY      Jitter Y in render pixels
 * @param renderWidth  Internal render resolution width
 * @param renderHeight Internal render resolution height
 * @return Pair of (ndcJitterX, ndcJitterY)
 */
            std::pair<float, float> JitterPixelsToNDC(float jitterX, float jitterY, uint32_t renderWidth,
                                                      uint32_t renderHeight)
            {
                float ndcX = jitterX * 2.0f / static_cast<float>(renderWidth);
                float ndcY = jitterY * 2.0f / static_cast<float>(renderHeight);
                return {ndcX, ndcY};
            }

        } // namespace UpscalingUtils
    } // namespace Graphics
} // namespace Spark
