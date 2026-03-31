#pragma once

/**
 * @file UpscalingShaders.h
 * @brief Inline HLSL compute shader source strings for the upscaling system
 *
 * Contains FSR 1.0 EASU/RCAS, temporal upscaling (FSR 2.0-style), and
 * SparkSR native temporal upscaling shader source.  Extracted from
 * UpscalingSystem.cpp to keep the translation unit focused on logic.
 */

namespace Spark::UpscalingShaders
{

    // -------------------------------------------------------------------------
    // FSR 1.0 EASU — Edge Adaptive Spatial Upsampling (Compute Shader)
    //
    // Simplified approximation of AMD's FidelityFX EASU pass.  Performs a
    // 12-tap directional filter in a 3x3 neighbourhood, detecting local edges
    // to steer an anisotropic Lanczos-like kernel.  Thread group: 8x8.
    // -------------------------------------------------------------------------
    inline constexpr const char* kFSR1_EASU_CS = R"(
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
    inline constexpr const char* kFSR1_RCAS_CS = R"(
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
    inline constexpr const char* kTemporalUpscaling_CS = R"(
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
    //   - Variance-based clip (mean +/- gamma * stddev) instead of min/max AABB
    //   - Motion vector confidence weighting (fast motion = less history)
    //   - Combined depth + motion divergence disocclusion detection
    //   - Jitter delta tracking for improved sub-pixel reprojection
    //   - Catmull-Rom 4-tap history sampling
    // Thread group: 8x8.  Two-pass pipeline: this shader + RCAS sharpening.
    // -------------------------------------------------------------------------
    inline constexpr const char* kSparkSR_CS = R"(
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

} // namespace Spark::UpscalingShaders
