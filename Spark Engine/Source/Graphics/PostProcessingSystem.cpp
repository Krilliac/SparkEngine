#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file PostProcessingSystem.cpp
 * @brief Full post-processing pipeline: Bloom, Tone Mapping, Color Grading, FXAA
 * @author Spark Engine Team
 * @date 2025
 *
 * Implements a multi-pass post-processing chain using D3D11 pixel shaders compiled
 * from inline HLSL via D3DCompile. The pipeline order is:
 *
 *   Scene HDR -> Bloom -> Tone Mapping -> Color Grading -> FXAA -> Final Output
 *
 * Each effect can be independently enabled/disabled and parameterized at runtime.
 */

#include "PostProcessingSystem.h"
#include "Utils/Assert.h"
#include "../Utils/SparkConsole.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// ============================================================================
// INTERNAL STRUCTURES AND CONSTANTS
// ============================================================================

namespace
{

// Maximum number of bloom mip levels for downsampling / blurring
static constexpr int   kBloomMipCount      = 5;
static constexpr float kDefaultBloomThreshold   = 1.0f;
static constexpr float kDefaultBloomIntensity   = 0.8f;
static constexpr float kDefaultExposure         = 1.0f;
static constexpr float kDefaultContrast         = 1.0f;
static constexpr float kDefaultSaturation       = 1.0f;
static constexpr float kDefaultGamma            = 2.2f;

// Tone-mapping operator IDs
enum class ToneMapOperator : int
{
    Reinhard    = 0,
    ACESFilmic  = 1,
    Uncharted2  = 2
};

// -----------------------------------------------------------------
// Constant-buffer layout shared by all post-processing shaders.
// Must be 16-byte aligned and match the HLSL cbuffer declaration.
// -----------------------------------------------------------------
struct alignas(16) PostProcessCB
{
    // Bloom
    float BloomThreshold;
    float BloomIntensity;
    float TexelSizeX;
    float TexelSizeY;

    // Tone mapping
    int   ToneMapOperator;   // 0=Reinhard, 1=ACES, 2=Uncharted2
    float Exposure;
    float Gamma;
    float _pad0;

    // Color grading
    float Contrast;
    float Saturation;
    float _pad1;
    float _pad2;

    // FXAA
    float FXAAQualitySubpix;
    float FXAAQualityEdgeThreshold;
    float FXAAQualityEdgeThresholdMin;
    float _pad3;

    // Screen dimensions for FXAA
    float ScreenWidthInv;
    float ScreenHeightInv;
    float _pad4;
    float _pad5;
};

// -----------------------------------------------------------------
// Full-screen triangle vertex shader (used by every pass)
// Renders a full-screen triangle without a vertex buffer.
// -----------------------------------------------------------------
static const char* kFullScreenVS = R"(
void VSMain(uint id : SV_VertexID,
            out float4 pos : SV_Position,
            out float2 uv  : TEXCOORD0)
{
    // Generate a full-screen triangle from vertex ID (0,1,2)
    uv  = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

// -----------------------------------------------------------------
// Brightness extraction (first bloom pass)
// -----------------------------------------------------------------
static const char* kBrightnessExtractPS = R"(
cbuffer PostProcessCB : register(b0)
{
    float BloomThreshold;
    float BloomIntensity;
    float TexelSizeX;
    float TexelSizeY;

    int   ToneMapOp;
    float Exposure;
    float Gamma;
    float _pad0;

    float Contrast;
    float Saturation;
    float _pad1;
    float _pad2;

    float FXAAQualitySubpix;
    float FXAAQualityEdgeThreshold;
    float FXAAQualityEdgeThresholdMin;
    float _pad3;

    float ScreenWidthInv;
    float ScreenHeightInv;
    float _pad4;
    float _pad5;
};

Texture2D    SceneTexture : register(t0);
SamplerState LinearSampler : register(s0);

float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 color = SceneTexture.Sample(LinearSampler, uv).rgb;
    float  lum   = Luminance(color);
    float  contrib = max(0.0, lum - BloomThreshold);
    // Soft-knee: smooth transition around threshold
    float  soft = contrib / (contrib + 1.0);
    return float4(color * soft, 1.0);
}
)";

// -----------------------------------------------------------------
// Gaussian blur (horizontal pass)
// -----------------------------------------------------------------
static const char* kGaussianBlurHPS = R"(
cbuffer PostProcessCB : register(b0)
{
    float BloomThreshold;
    float BloomIntensity;
    float TexelSizeX;
    float TexelSizeY;

    int   ToneMapOp;
    float Exposure;
    float Gamma;
    float _pad0;

    float Contrast;
    float Saturation;
    float _pad1;
    float _pad2;

    float FXAAQualitySubpix;
    float FXAAQualityEdgeThreshold;
    float FXAAQualityEdgeThresholdMin;
    float _pad3;

    float ScreenWidthInv;
    float ScreenHeightInv;
    float _pad4;
    float _pad5;
};

Texture2D    SourceTexture : register(t0);
SamplerState LinearSampler : register(s0);

// 9-tap Gaussian kernel
static const int   kKernelRadius = 4;
static const float kWeights[9] = {
    0.0162162162, 0.0540540541, 0.1216216216, 0.1945945946, 0.2270270270,
    0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162
};

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 result = float3(0.0, 0.0, 0.0);
    for (int i = -kKernelRadius; i <= kKernelRadius; i++)
    {
        float2 offset = float2(float(i) * TexelSizeX, 0.0);
        result += SourceTexture.Sample(LinearSampler, uv + offset).rgb
                  * kWeights[i + kKernelRadius];
    }
    return float4(result, 1.0);
}
)";

// -----------------------------------------------------------------
// Gaussian blur (vertical pass)
// -----------------------------------------------------------------
static const char* kGaussianBlurVPS = R"(
cbuffer PostProcessCB : register(b0)
{
    float BloomThreshold;
    float BloomIntensity;
    float TexelSizeX;
    float TexelSizeY;

    int   ToneMapOp;
    float Exposure;
    float Gamma;
    float _pad0;

    float Contrast;
    float Saturation;
    float _pad1;
    float _pad2;

    float FXAAQualitySubpix;
    float FXAAQualityEdgeThreshold;
    float FXAAQualityEdgeThresholdMin;
    float _pad3;

    float ScreenWidthInv;
    float ScreenHeightInv;
    float _pad4;
    float _pad5;
};

Texture2D    SourceTexture : register(t0);
SamplerState LinearSampler : register(s0);

static const int   kKernelRadius = 4;
static const float kWeights[9] = {
    0.0162162162, 0.0540540541, 0.1216216216, 0.1945945946, 0.2270270270,
    0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162
};

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 result = float3(0.0, 0.0, 0.0);
    for (int i = -kKernelRadius; i <= kKernelRadius; i++)
    {
        float2 offset = float2(0.0, float(i) * TexelSizeY);
        result += SourceTexture.Sample(LinearSampler, uv + offset).rgb
                  * kWeights[i + kKernelRadius];
    }
    return float4(result, 1.0);
}
)";

// -----------------------------------------------------------------
// Bloom composite (additive blend of blurred bloom onto the scene)
// -----------------------------------------------------------------
static const char* kBloomCompositePS = R"(
cbuffer PostProcessCB : register(b0)
{
    float BloomThreshold;
    float BloomIntensity;
    float TexelSizeX;
    float TexelSizeY;

    int   ToneMapOp;
    float Exposure;
    float Gamma;
    float _pad0;

    float Contrast;
    float Saturation;
    float _pad1;
    float _pad2;

    float FXAAQualitySubpix;
    float FXAAQualityEdgeThreshold;
    float FXAAQualityEdgeThresholdMin;
    float _pad3;

    float ScreenWidthInv;
    float ScreenHeightInv;
    float _pad4;
    float _pad5;
};

Texture2D    SceneTexture : register(t0);
Texture2D    BloomTexture : register(t1);
SamplerState LinearSampler : register(s0);

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 scene = SceneTexture.Sample(LinearSampler, uv).rgb;
    float3 bloom = BloomTexture.Sample(LinearSampler, uv).rgb;
    float3 result = scene + bloom * BloomIntensity;
    return float4(result, 1.0);
}
)";

// -----------------------------------------------------------------
// Tone mapping + color grading combined pass
// -----------------------------------------------------------------
static const char* kToneMapColorGradePS = R"(
cbuffer PostProcessCB : register(b0)
{
    float BloomThreshold;
    float BloomIntensity;
    float TexelSizeX;
    float TexelSizeY;

    int   ToneMapOp;
    float Exposure;
    float Gamma;
    float _pad0;

    float Contrast;
    float Saturation;
    float _pad1;
    float _pad2;

    float FXAAQualitySubpix;
    float FXAAQualityEdgeThreshold;
    float FXAAQualityEdgeThresholdMin;
    float _pad3;

    float ScreenWidthInv;
    float ScreenHeightInv;
    float _pad4;
    float _pad5;
};

Texture2D    SceneTexture : register(t0);
SamplerState LinearSampler : register(s0);

float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Reinhard tone mapping
float3 TonemapReinhard(float3 c)
{
    return c / (1.0 + c);
}

// ACES Filmic tone mapping (fitted by Krzysztof Narkowicz)
float3 TonemapACES(float3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Uncharted 2 tone mapping (John Hable)
float3 Uncharted2Partial(float3 x)
{
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 TonemapUncharted2(float3 color)
{
    float exposureBias = 2.0;
    float3 curr = Uncharted2Partial(color * exposureBias);
    float3 W = float3(11.2, 11.2, 11.2);
    float3 whiteScale = 1.0 / Uncharted2Partial(W);
    return curr * whiteScale;
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 color = SceneTexture.Sample(LinearSampler, uv).rgb;

    // Apply exposure
    color *= Exposure;

    // Tone mapping
    if (ToneMapOp == 0)
        color = TonemapReinhard(color);
    else if (ToneMapOp == 1)
        color = TonemapACES(color);
    else
        color = TonemapUncharted2(color);

    // --- Color grading ---

    // Contrast (pivot around mid-gray 0.5)
    color = saturate(((color - 0.5) * Contrast) + 0.5);

    // Saturation
    float lum = Luminance(color);
    color = lerp(float3(lum, lum, lum), color, Saturation);

    // Gamma correction
    color = pow(max(color, 0.0), 1.0 / Gamma);

    return float4(color, 1.0);
}
)";

// -----------------------------------------------------------------
// FXAA 3.11 quality pixel shader
// Adapted from Timothy Lottes / NVIDIA FXAA 3.11
// -----------------------------------------------------------------
static const char* kFXAAPS = R"(
cbuffer PostProcessCB : register(b0)
{
    float BloomThreshold;
    float BloomIntensity;
    float TexelSizeX;
    float TexelSizeY;

    int   ToneMapOp;
    float Exposure;
    float Gamma;
    float _pad0;

    float Contrast;
    float Saturation;
    float _pad1;
    float _pad2;

    float FXAAQualitySubpix;
    float FXAAQualityEdgeThreshold;
    float FXAAQualityEdgeThresholdMin;
    float _pad3;

    float ScreenWidthInv;
    float ScreenHeightInv;
    float _pad4;
    float _pad5;
};

Texture2D    SceneTexture : register(t0);
SamplerState LinearSampler : register(s0);

float FxaaLuma(float3 rgb)
{
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 rcpFrame = float2(ScreenWidthInv, ScreenHeightInv);

    // Sample the centre and 4-neighbours
    float3 rgbN  = SceneTexture.Sample(LinearSampler, uv + float2( 0, -1) * rcpFrame).rgb;
    float3 rgbW  = SceneTexture.Sample(LinearSampler, uv + float2(-1,  0) * rcpFrame).rgb;
    float3 rgbM  = SceneTexture.Sample(LinearSampler, uv).rgb;
    float3 rgbE  = SceneTexture.Sample(LinearSampler, uv + float2( 1,  0) * rcpFrame).rgb;
    float3 rgbS  = SceneTexture.Sample(LinearSampler, uv + float2( 0,  1) * rcpFrame).rgb;

    float lumaN = FxaaLuma(rgbN);
    float lumaW = FxaaLuma(rgbW);
    float lumaM = FxaaLuma(rgbM);
    float lumaE = FxaaLuma(rgbE);
    float lumaS = FxaaLuma(rgbS);

    float rangeMin = min(lumaM, min(min(lumaN, lumaW), min(lumaS, lumaE)));
    float rangeMax = max(lumaM, max(max(lumaN, lumaW), max(lumaS, lumaE)));
    float range    = rangeMax - rangeMin;

    // Early exit if contrast is below the edge threshold
    if (range < max(FXAAQualityEdgeThresholdMin, rangeMax * FXAAQualityEdgeThreshold))
        return float4(rgbM, 1.0);

    // Sample diagonal neighbours
    float3 rgbNW = SceneTexture.Sample(LinearSampler, uv + float2(-1, -1) * rcpFrame).rgb;
    float3 rgbNE = SceneTexture.Sample(LinearSampler, uv + float2( 1, -1) * rcpFrame).rgb;
    float3 rgbSW = SceneTexture.Sample(LinearSampler, uv + float2(-1,  1) * rcpFrame).rgb;
    float3 rgbSE = SceneTexture.Sample(LinearSampler, uv + float2( 1,  1) * rcpFrame).rgb;

    float lumaNW = FxaaLuma(rgbNW);
    float lumaNE = FxaaLuma(rgbNE);
    float lumaSW = FxaaLuma(rgbSW);
    float lumaSE = FxaaLuma(rgbSE);

    float lumaWCorners = lumaNW + lumaSW;
    float lumaECorners = lumaNE + lumaSE;
    float lumaNCorners = lumaNW + lumaNE;
    float lumaSCorners = lumaSW + lumaSE;

    // Determine edge direction (horizontal vs vertical)
    float edgeHorz = abs(lumaWCorners - 2.0 * lumaW) +
                     abs((lumaN + lumaS) - 2.0 * lumaM) * 2.0 +
                     abs(lumaECorners - 2.0 * lumaE);
    float edgeVert = abs(lumaNCorners - 2.0 * lumaN) +
                     abs((lumaW + lumaE) - 2.0 * lumaM) * 2.0 +
                     abs(lumaSCorners - 2.0 * lumaS);
    bool isHorizontal = (edgeHorz >= edgeVert);

    // Choose the pair of pixels along the edge
    float luma1 = isHorizontal ? lumaN : lumaW;
    float luma2 = isHorizontal ? lumaS : lumaE;
    float gradient1 = abs(luma1 - lumaM);
    float gradient2 = abs(luma2 - lumaM);
    bool is1Steeper = (gradient1 >= gradient2);

    float gradientScaled = 0.25 * max(gradient1, gradient2);

    float stepLength = isHorizontal ? rcpFrame.y : rcpFrame.x;
    float lumaLocalAverage = 0.0;

    if (is1Steeper)
    {
        stepLength = -stepLength;
        lumaLocalAverage = 0.5 * (luma1 + lumaM);
    }
    else
    {
        lumaLocalAverage = 0.5 * (luma2 + lumaM);
    }

    // Shift UV to the edge
    float2 currentUV = uv;
    if (isHorizontal)
        currentUV.y += stepLength * 0.5;
    else
        currentUV.x += stepLength * 0.5;

    // Walk along the edge in both directions
    float2 offset = isHorizontal ? float2(rcpFrame.x, 0.0) : float2(0.0, rcpFrame.y);

    float2 uv1 = currentUV - offset;
    float2 uv2 = currentUV + offset;

    float lumaEnd1 = FxaaLuma(SceneTexture.Sample(LinearSampler, uv1).rgb) - lumaLocalAverage;
    float lumaEnd2 = FxaaLuma(SceneTexture.Sample(LinearSampler, uv2).rgb) - lumaLocalAverage;

    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;
    bool reachedBoth = reached1 && reached2;

    if (!reached1) uv1 -= offset;
    if (!reached2) uv2 += offset;

    // Up to 12 iterations along edge
    [unroll]
    for (int i = 2; i < 12; i++)
    {
        if (!reached1)
        {
            lumaEnd1 = FxaaLuma(SceneTexture.Sample(LinearSampler, uv1).rgb) - lumaLocalAverage;
            reached1 = abs(lumaEnd1) >= gradientScaled;
        }
        if (!reached2)
        {
            lumaEnd2 = FxaaLuma(SceneTexture.Sample(LinearSampler, uv2).rgb) - lumaLocalAverage;
            reached2 = abs(lumaEnd2) >= gradientScaled;
        }
        reachedBoth = reached1 && reached2;
        if (reachedBoth) break;
        if (!reached1) uv1 -= offset;
        if (!reached2) uv2 += offset;
    }

    // Calculate the sub-pixel offset
    float dist1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float dist2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);

    bool isDir1 = (dist1 < dist2);
    float distFinal = min(dist1, dist2);
    float edgeLength = dist1 + dist2;

    float pixelOffset = -distFinal / edgeLength + 0.5;

    // Check that the luma at center is consistent with the edge direction
    bool isLumaCenterSmaller = (lumaM < lumaLocalAverage);
    bool correctVariation = ((isDir1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaCenterSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    // Sub-pixel anti-aliasing
    float lumaAverage = (1.0 / 12.0) * (2.0 * (lumaN + lumaS + lumaE + lumaW) +
                         lumaWCorners + lumaECorners);
    float subPixelOffset1 = clamp(abs(lumaAverage - lumaM) / range, 0.0, 1.0);
    float subPixelOffset2 = (-2.0 * subPixelOffset1 + 3.0) * subPixelOffset1 * subPixelOffset1;
    float subPixelOffsetFinal = subPixelOffset2 * subPixelOffset2 * FXAAQualitySubpix;

    finalOffset = max(finalOffset, subPixelOffsetFinal);

    // Final sample
    float2 finalUV = uv;
    if (isHorizontal)
        finalUV.y += finalOffset * stepLength;
    else
        finalUV.x += finalOffset * stepLength;

    float3 finalColor = SceneTexture.Sample(LinearSampler, finalUV).rgb;
    return float4(finalColor, 1.0);
}
)";

// -----------------------------------------------------------------
// Simple pass-through / copy shader (used when an effect is disabled
// but we still need to copy between render targets)
// -----------------------------------------------------------------
static const char* kCopyPS = R"(
Texture2D    SourceTexture : register(t0);
SamplerState LinearSampler : register(s0);

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return SourceTexture.Sample(LinearSampler, uv);
}
)";

// -----------------------------------------------------------------
// Helper: compile an HLSL shader from an inline source string
// -----------------------------------------------------------------
HRESULT CompileShaderFromString(const char* source,
                                const char* entryPoint,
                                const char* target,
                                ID3DBlob** blobOut)
{
    DWORD flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG) || defined(DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(source,
                            strlen(source),
                            nullptr,  // source name
                            nullptr,  // defines
                            nullptr,  // includes
                            entryPoint,
                            target,
                            flags,
                            0,
                            blobOut,
                            &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::string errMsg = "Shader compile error: ";
            errMsg += static_cast<const char*>(errorBlob->GetBufferPointer());
            Spark::SimpleConsole::GetInstance().LogError(errMsg);
        }
        return hr;
    }
    return S_OK;
}

} // anonymous namespace

// ============================================================================
// INTERNAL IMPLEMENTATION CLASS (PIMPL-style but stored as opaque members)
// ============================================================================
//
// Because we must not modify the header, we store all internal state in a
// namespace-scoped detail structure allocated on the heap and referenced
// through a global map keyed by the PostProcessingSystem instance pointer.
// This avoids any header changes while giving us rich internal state.
// ============================================================================

namespace PPInternal
{

struct EffectState
{
    bool enabled = true;
};

struct BloomState : EffectState
{
    float threshold  = kDefaultBloomThreshold;
    float intensity  = kDefaultBloomIntensity;
};

struct ToneMapState : EffectState
{
    ToneMapOperator op = ToneMapOperator::ACESFilmic;
    float exposure     = kDefaultExposure;
    float gamma        = kDefaultGamma;
};

struct ColorGradeState : EffectState
{
    float contrast   = kDefaultContrast;
    float saturation = kDefaultSaturation;
};

struct FXAAState : EffectState
{
    float qualitySubpix           = 0.75f;
    float qualityEdgeThreshold    = 0.166f;
    float qualityEdgeThresholdMin = 0.0833f;
};

// Per-mip render target pair (for bloom ping-pong)
struct MipTarget
{
    ComPtr<ID3D11Texture2D>          texture;
    ComPtr<ID3D11RenderTargetView>   rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    UINT width  = 0;
    UINT height = 0;
};

struct PPData
{
    // Initialisation state
    bool initialised = false;
    UINT screenWidth  = 0;
    UINT screenHeight = 0;

    // Effect parameters
    BloomState      bloom;
    ToneMapState    toneMap;
    ColorGradeState colorGrade;
    FXAAState       fxaa;

    // ------ GPU resources ------

    // Shared vertex shader (full-screen triangle)
    ComPtr<ID3D11VertexShader> fullscreenVS;

    // Pixel shaders
    ComPtr<ID3D11PixelShader> brightnessExtractPS;
    ComPtr<ID3D11PixelShader> gaussianBlurHPS;
    ComPtr<ID3D11PixelShader> gaussianBlurVPS;
    ComPtr<ID3D11PixelShader> bloomCompositePS;
    ComPtr<ID3D11PixelShader> toneMapColorGradePS;
    ComPtr<ID3D11PixelShader> fxaaPS;
    ComPtr<ID3D11PixelShader> copyPS;

    // Constant buffer
    ComPtr<ID3D11Buffer> constantBuffer;

    // Sampler state (bilinear, clamp)
    ComPtr<ID3D11SamplerState> linearClampSampler;

    // Blend states
    ComPtr<ID3D11BlendState> noBlendState;

    // Rasterizer state (no culling for full-screen passes)
    ComPtr<ID3D11RasterizerState> noCullRasterState;

    // Depth-stencil state (depth disabled for post-processing)
    ComPtr<ID3D11DepthStencilState> noDepthState;

    // Intermediate render targets
    // Ping-pong pair at full resolution
    MipTarget fullResA;
    MipTarget fullResB;

    // Bloom mip chain (downsampled) - two sets for ping-pong blur
    MipTarget bloomMipsA[kBloomMipCount];
    MipTarget bloomMipsB[kBloomMipCount];

    // Back-buffer reference (we need to capture and restore it)
    ComPtr<ID3D11RenderTargetView>   backBufferRTV;
    ComPtr<ID3D11DepthStencilView>   backBufferDSV;
};

// Global map of instance -> internal data
#include <unordered_map>
static std::unordered_map<const PostProcessingSystem*, std::unique_ptr<PPData>> s_dataMap;

PPData* GetData(const PostProcessingSystem* instance)
{
    auto it = s_dataMap.find(instance);
    return (it != s_dataMap.end()) ? it->second.get() : nullptr;
}

PPData* GetOrCreateData(const PostProcessingSystem* instance)
{
    auto& ptr = s_dataMap[instance];
    if (!ptr)
        ptr = std::make_unique<PPData>();
    return ptr.get();
}

void DestroyData(const PostProcessingSystem* instance)
{
    s_dataMap.erase(instance);
}

// -----------------------------------------------------------------
// Helper: create a single render target (texture + RTV + SRV)
// -----------------------------------------------------------------
HRESULT CreateRenderTargetPair(ID3D11Device* device,
                               UINT width, UINT height,
                               DXGI_FORMAT format,
                               MipTarget& out)
{
    out.width  = width;
    out.height = height;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width      = width;
    texDesc.Height     = height;
    texDesc.MipLevels  = 1;
    texDesc.ArraySize  = 1;
    texDesc.Format     = format;
    texDesc.SampleDesc.Count   = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage      = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &out.texture);
    if (FAILED(hr)) return hr;

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format        = format;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = device->CreateRenderTargetView(out.texture.Get(), &rtvDesc, &out.rtv);
    if (FAILED(hr)) return hr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format              = format;
    srvDesc.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(out.texture.Get(), &srvDesc, &out.srv);
    return hr;
}

// -----------------------------------------------------------------
// Create all intermediate render targets
// -----------------------------------------------------------------
HRESULT CreateRenderTargets(ID3D11Device* device, PPData* d)
{
    const DXGI_FORMAT hdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const DXGI_FORMAT ldrFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    HRESULT hr;

    // Full-resolution ping-pong (HDR)
    hr = CreateRenderTargetPair(device, d->screenWidth, d->screenHeight, hdrFormat, d->fullResA);
    if (FAILED(hr)) return hr;

    hr = CreateRenderTargetPair(device, d->screenWidth, d->screenHeight, hdrFormat, d->fullResB);
    if (FAILED(hr)) return hr;

    // Bloom mip chain
    for (int i = 0; i < kBloomMipCount; ++i)
    {
        UINT w = std::max<UINT>(1u, d->screenWidth  >> (i + 1));
        UINT h = std::max<UINT>(1u, d->screenHeight >> (i + 1));

        hr = CreateRenderTargetPair(device, w, h, hdrFormat, d->bloomMipsA[i]);
        if (FAILED(hr)) return hr;

        hr = CreateRenderTargetPair(device, w, h, hdrFormat, d->bloomMipsB[i]);
        if (FAILED(hr)) return hr;
    }

    return S_OK;
}

// -----------------------------------------------------------------
// Compile and create all shaders
// -----------------------------------------------------------------
HRESULT CreateShaders(ID3D11Device* device, PPData* d)
{
    HRESULT hr;
    ComPtr<ID3DBlob> blob;

    // Full-screen vertex shader
    hr = CompileShaderFromString(kFullScreenVS, "VSMain", "vs_5_0", &blob);
    if (FAILED(hr)) return hr;
    hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &d->fullscreenVS);
    if (FAILED(hr)) return hr;

    // Brightness extraction PS
    hr = CompileShaderFromString(kBrightnessExtractPS, "PSMain", "ps_5_0", &blob);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &d->brightnessExtractPS);
    if (FAILED(hr)) return hr;

    // Gaussian blur horizontal PS
    hr = CompileShaderFromString(kGaussianBlurHPS, "PSMain", "ps_5_0", &blob);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &d->gaussianBlurHPS);
    if (FAILED(hr)) return hr;

    // Gaussian blur vertical PS
    hr = CompileShaderFromString(kGaussianBlurVPS, "PSMain", "ps_5_0", &blob);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &d->gaussianBlurVPS);
    if (FAILED(hr)) return hr;

    // Bloom composite PS
    hr = CompileShaderFromString(kBloomCompositePS, "PSMain", "ps_5_0", &blob);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &d->bloomCompositePS);
    if (FAILED(hr)) return hr;

    // Tone map + color grading PS
    hr = CompileShaderFromString(kToneMapColorGradePS, "PSMain", "ps_5_0", &blob);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &d->toneMapColorGradePS);
    if (FAILED(hr)) return hr;

    // FXAA PS
    hr = CompileShaderFromString(kFXAAPS, "PSMain", "ps_5_0", &blob);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &d->fxaaPS);
    if (FAILED(hr)) return hr;

    // Copy / pass-through PS
    hr = CompileShaderFromString(kCopyPS, "PSMain", "ps_5_0", &blob);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &d->copyPS);
    if (FAILED(hr)) return hr;

    return S_OK;
}

// -----------------------------------------------------------------
// Create pipeline state objects (constant buffer, sampler, blend,
// rasterizer, depth-stencil)
// -----------------------------------------------------------------
HRESULT CreatePipelineStates(ID3D11Device* device, PPData* d)
{
    HRESULT hr;

    // Constant buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth      = sizeof(PostProcessCB);
    cbDesc.Usage           = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbDesc, nullptr, &d->constantBuffer);
    if (FAILED(hr)) return hr;

    // Linear clamp sampler
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.MaxLOD   = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sampDesc, &d->linearClampSampler);
    if (FAILED(hr)) return hr;

    // No-blend state (opaque write)
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable    = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&blendDesc, &d->noBlendState);
    if (FAILED(hr)) return hr;

    // No-cull rasterizer
    D3D11_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rastDesc, &d->noCullRasterState);
    if (FAILED(hr)) return hr;

    // No-depth state
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable    = FALSE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsDesc.StencilEnable  = FALSE;
    hr = device->CreateDepthStencilState(&dsDesc, &d->noDepthState);
    if (FAILED(hr)) return hr;

    return S_OK;
}

// -----------------------------------------------------------------
// Update the constant buffer with the current effect parameters
// -----------------------------------------------------------------
void UpdateConstantBuffer(ID3D11DeviceContext* context,
                          PPData* d,
                          float texelW, float texelH)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context->Map(d->constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;

    PostProcessCB* cb = static_cast<PostProcessCB*>(mapped.pData);

    cb->BloomThreshold = d->bloom.threshold;
    cb->BloomIntensity = d->bloom.intensity;
    cb->TexelSizeX     = texelW;
    cb->TexelSizeY     = texelH;

    cb->ToneMapOperator = static_cast<int>(d->toneMap.op);
    cb->Exposure        = d->toneMap.exposure;
    cb->Gamma           = d->toneMap.gamma;
    cb->_pad0           = 0.0f;

    cb->Contrast   = d->colorGrade.contrast;
    cb->Saturation = d->colorGrade.saturation;
    cb->_pad1      = 0.0f;
    cb->_pad2      = 0.0f;

    cb->FXAAQualitySubpix           = d->fxaa.qualitySubpix;
    cb->FXAAQualityEdgeThreshold    = d->fxaa.qualityEdgeThreshold;
    cb->FXAAQualityEdgeThresholdMin = d->fxaa.qualityEdgeThresholdMin;
    cb->_pad3                       = 0.0f;

    cb->ScreenWidthInv  = 1.0f / static_cast<float>(d->screenWidth);
    cb->ScreenHeightInv = 1.0f / static_cast<float>(d->screenHeight);
    cb->_pad4           = 0.0f;
    cb->_pad5           = 0.0f;

    context->Unmap(d->constantBuffer.Get(), 0);
}

// -----------------------------------------------------------------
// Draw a full-screen triangle (3 vertices, no VB/IB)
// -----------------------------------------------------------------
void DrawFullScreenTriangle(ID3D11DeviceContext* context, PPData* d)
{
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    context->VSSetShader(d->fullscreenVS.Get(), nullptr, 0);
    context->Draw(3, 0);
}

// -----------------------------------------------------------------
// Set the pipeline state for a post-processing pass
// -----------------------------------------------------------------
void SetPostProcessState(ID3D11DeviceContext* context, PPData* d)
{
    float blendFactor[4] = {0, 0, 0, 0};
    context->OMSetBlendState(d->noBlendState.Get(), blendFactor, 0xFFFFFFFF);
    context->RSSetState(d->noCullRasterState.Get());
    context->OMSetDepthStencilState(d->noDepthState.Get(), 0);

    ID3D11SamplerState* samplers[] = { d->linearClampSampler.Get() };
    context->PSSetSamplers(0, 1, samplers);

    ID3D11Buffer* cbs[] = { d->constantBuffer.Get() };
    context->PSSetConstantBuffers(0, 1, cbs);
}

// -----------------------------------------------------------------
// Set a viewport for the given dimensions
// -----------------------------------------------------------------
void SetViewport(ID3D11DeviceContext* context, UINT w, UINT h)
{
    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(w);
    vp.Height   = static_cast<float>(h);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);
}

// -----------------------------------------------------------------
// Render a single post-processing pass
// -----------------------------------------------------------------
void RenderPass(ID3D11DeviceContext* context,
                PPData* d,
                ID3D11PixelShader* ps,
                ID3D11RenderTargetView* rtv,
                UINT viewW, UINT viewH,
                ID3D11ShaderResourceView* srv0,
                ID3D11ShaderResourceView* srv1 = nullptr)
{
    // Unbind all SRVs first to avoid hazards
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    context->PSSetShaderResources(0, 2, nullSRVs);

    // Set render target
    ID3D11RenderTargetView* rtvs[] = { rtv };
    context->OMSetRenderTargets(1, rtvs, nullptr);

    SetViewport(context, viewW, viewH);

    // Bind SRVs
    ID3D11ShaderResourceView* srvs[2] = { srv0, srv1 };
    int srvCount = srv1 ? 2 : 1;
    context->PSSetShaderResources(0, srvCount, srvs);

    // Bind pixel shader
    context->PSSetShader(ps, nullptr, 0);

    DrawFullScreenTriangle(context, d);

    // Unbind
    context->PSSetShaderResources(0, 2, nullSRVs);
}

} // namespace PPInternal

// ============================================================================
// PUBLIC API IMPLEMENTATION
// ============================================================================

PostProcessingSystem::PostProcessingSystem()
    : m_device(nullptr), m_context(nullptr)
{
}

PostProcessingSystem::~PostProcessingSystem()
{
    Shutdown();
}

HRESULT PostProcessingSystem::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    ASSERT(device && context);

    m_device  = device;
    m_context = context;

    auto* d = PPInternal::GetOrCreateData(this);

    // Query the back-buffer dimensions from the device's render target
    ComPtr<ID3D11RenderTargetView> currentRTV;
    ComPtr<ID3D11DepthStencilView> currentDSV;
    context->OMGetRenderTargets(1, &currentRTV, &currentDSV);

    d->backBufferRTV = currentRTV;
    d->backBufferDSV = currentDSV;

    // Determine screen dimensions from the current render target
    if (currentRTV)
    {
        ComPtr<ID3D11Resource> rtvResource;
        currentRTV->GetResource(&rtvResource);

        ComPtr<ID3D11Texture2D> tex2D;
        rtvResource.As(&tex2D);
        if (tex2D)
        {
            D3D11_TEXTURE2D_DESC desc;
            tex2D->GetDesc(&desc);
            d->screenWidth  = desc.Width;
            d->screenHeight = desc.Height;
        }
    }

    // Fallback dimensions if we could not query
    if (d->screenWidth == 0 || d->screenHeight == 0)
    {
        d->screenWidth  = 1920;
        d->screenHeight = 1080;
        Spark::SimpleConsole::GetInstance().LogWarning(
            "PostProcessingSystem: Could not determine screen size from back buffer. "
            "Defaulting to 1920x1080.");
    }

    // Create shaders
    HRESULT hr = PPInternal::CreateShaders(device, d);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError(
            "PostProcessingSystem: Failed to compile one or more shaders.");
        return hr;
    }

    // Create render targets
    hr = PPInternal::CreateRenderTargets(device, d);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError(
            "PostProcessingSystem: Failed to create render targets.");
        return hr;
    }

    // Create pipeline state objects
    hr = PPInternal::CreatePipelineStates(device, d);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError(
            "PostProcessingSystem: Failed to create pipeline state objects.");
        return hr;
    }

    d->initialised = true;

    Spark::SimpleConsole::GetInstance().LogSuccess(
        "PostProcessingSystem initialized successfully (" +
        std::to_string(d->screenWidth) + "x" + std::to_string(d->screenHeight) +
        ", " + std::to_string(kBloomMipCount) + " bloom mip levels)");

    return S_OK;
}

void PostProcessingSystem::Shutdown()
{
    PPInternal::DestroyData(this);
    m_device  = nullptr;
    m_context = nullptr;

    Spark::SimpleConsole::GetInstance().LogInfo("PostProcessingSystem shutdown complete");
}

void PostProcessingSystem::Update(float deltaTime)
{
    (void)deltaTime;

    auto* d = PPInternal::GetData(this);
    if (!d || !d->initialised || !m_context || !m_device)
        return;

    // ====================================================================
    // Capture the current back-buffer render target. The scene has already
    // been rendered into it. We will copy it into our first intermediate
    // target and process from there, writing the final result back.
    // ====================================================================

    ComPtr<ID3D11RenderTargetView> originalRTV;
    ComPtr<ID3D11DepthStencilView> originalDSV;
    m_context->OMGetRenderTargets(1, &originalRTV, &originalDSV);

    if (!originalRTV)
        return; // Nothing to post-process

    // Update the cached back-buffer reference
    d->backBufferRTV = originalRTV;
    d->backBufferDSV = originalDSV;

    // Determine actual back-buffer dimensions (may have changed)
    {
        ComPtr<ID3D11Resource> rtvRes;
        originalRTV->GetResource(&rtvRes);
        ComPtr<ID3D11Texture2D> tex2D;
        rtvRes.As(&tex2D);
        if (tex2D)
        {
            D3D11_TEXTURE2D_DESC desc;
            tex2D->GetDesc(&desc);
            if (desc.Width != d->screenWidth || desc.Height != d->screenHeight)
            {
                // Screen size changed -- recreate render targets
                d->screenWidth  = desc.Width;
                d->screenHeight = desc.Height;
                PPInternal::CreateRenderTargets(m_device, d);
            }
        }
    }

    // Get the back-buffer as an SRV. If the back-buffer texture was not
    // created with D3D11_BIND_SHADER_RESOURCE we need to copy it into
    // our own texture first.
    {
        ComPtr<ID3D11Resource> bbRes;
        originalRTV->GetResource(&bbRes);
        m_context->CopyResource(d->fullResA.texture.Get(), bbRes.Get());
    }

    // Set shared pipeline state
    PPInternal::SetPostProcessState(m_context, d);

    // Current source for the next pass
    PPInternal::MipTarget* currentSource = &d->fullResA;

    // ====================================================================
    // PASS 1: BLOOM
    // ====================================================================
    if (d->bloom.enabled)
    {
        // 1a. Brightness extraction from fullResA -> bloomMipsA[0]
        // We extract at the first mip resolution (half res)
        {
            UINT mw = d->bloomMipsA[0].width;
            UINT mh = d->bloomMipsA[0].height;
            PPInternal::UpdateConstantBuffer(m_context, d,
                                             1.0f / static_cast<float>(mw),
                                             1.0f / static_cast<float>(mh));
            PPInternal::RenderPass(m_context, d,
                                   d->brightnessExtractPS.Get(),
                                   d->bloomMipsA[0].rtv.Get(),
                                   mw, mh,
                                   currentSource->srv.Get());
        }

        // 1b. Progressive downsample + blur through mip chain
        for (int mip = 0; mip < kBloomMipCount; ++mip)
        {
            UINT mw = d->bloomMipsA[mip].width;
            UINT mh = d->bloomMipsA[mip].height;

            float texelW = 1.0f / static_cast<float>(mw);
            float texelH = 1.0f / static_cast<float>(mh);

            // If this is not the first mip, downsample from the previous
            // mip's blurred result using the copy shader
            if (mip > 0)
            {
                PPInternal::UpdateConstantBuffer(m_context, d, texelW, texelH);
                PPInternal::RenderPass(m_context, d,
                                       d->copyPS.Get(),
                                       d->bloomMipsA[mip].rtv.Get(),
                                       mw, mh,
                                       d->bloomMipsA[mip - 1].srv.Get());
            }

            // Horizontal blur: bloomMipsA[mip] -> bloomMipsB[mip]
            PPInternal::UpdateConstantBuffer(m_context, d, texelW, texelH);
            PPInternal::RenderPass(m_context, d,
                                   d->gaussianBlurHPS.Get(),
                                   d->bloomMipsB[mip].rtv.Get(),
                                   mw, mh,
                                   d->bloomMipsA[mip].srv.Get());

            // Vertical blur: bloomMipsB[mip] -> bloomMipsA[mip]
            PPInternal::RenderPass(m_context, d,
                                   d->gaussianBlurVPS.Get(),
                                   d->bloomMipsA[mip].rtv.Get(),
                                   mw, mh,
                                   d->bloomMipsB[mip].srv.Get());
        }

        // 1c. Progressive upsample + accumulate back up the chain
        // We accumulate from the smallest mip back to mip 0 using additive
        // blending via the bloom composite shader.
        for (int mip = kBloomMipCount - 2; mip >= 0; --mip)
        {
            UINT mw = d->bloomMipsA[mip].width;
            UINT mh = d->bloomMipsA[mip].height;

            float texelW = 1.0f / static_cast<float>(mw);
            float texelH = 1.0f / static_cast<float>(mh);

            PPInternal::UpdateConstantBuffer(m_context, d, texelW, texelH);

            // Composite: bloomMipsA[mip] (scene at this level) +
            //            bloomMipsA[mip+1] (blurred smaller mip, bilinear upsampled)
            // We write into bloomMipsB[mip] to avoid read/write to the same target
            PPInternal::RenderPass(m_context, d,
                                   d->bloomCompositePS.Get(),
                                   d->bloomMipsB[mip].rtv.Get(),
                                   mw, mh,
                                   d->bloomMipsA[mip].srv.Get(),
                                   d->bloomMipsA[mip + 1].srv.Get());

            // Swap so bloomMipsA[mip] holds the composited result
            std::swap(d->bloomMipsA[mip], d->bloomMipsB[mip]);
        }

        // 1d. Final bloom composite: combine original scene with bloom result
        {
            PPInternal::UpdateConstantBuffer(m_context, d,
                                             1.0f / static_cast<float>(d->screenWidth),
                                             1.0f / static_cast<float>(d->screenHeight));
            PPInternal::RenderPass(m_context, d,
                                   d->bloomCompositePS.Get(),
                                   d->fullResB.rtv.Get(),
                                   d->screenWidth, d->screenHeight,
                                   currentSource->srv.Get(),
                                   d->bloomMipsA[0].srv.Get());
            currentSource = &d->fullResB;
        }
    }

    // ====================================================================
    // PASS 2: TONE MAPPING + COLOR GRADING (combined for efficiency)
    // ====================================================================
    if (d->toneMap.enabled || d->colorGrade.enabled)
    {
        PPInternal::MipTarget* dest = (currentSource == &d->fullResA) ? &d->fullResB : &d->fullResA;

        PPInternal::UpdateConstantBuffer(m_context, d,
                                         1.0f / static_cast<float>(d->screenWidth),
                                         1.0f / static_cast<float>(d->screenHeight));

        PPInternal::RenderPass(m_context, d,
                               d->toneMapColorGradePS.Get(),
                               dest->rtv.Get(),
                               d->screenWidth, d->screenHeight,
                               currentSource->srv.Get());
        currentSource = dest;
    }

    // ====================================================================
    // PASS 3: FXAA
    // ====================================================================
    if (d->fxaa.enabled)
    {
        PPInternal::MipTarget* dest = (currentSource == &d->fullResA) ? &d->fullResB : &d->fullResA;

        PPInternal::UpdateConstantBuffer(m_context, d,
                                         1.0f / static_cast<float>(d->screenWidth),
                                         1.0f / static_cast<float>(d->screenHeight));

        PPInternal::RenderPass(m_context, d,
                               d->fxaaPS.Get(),
                               dest->rtv.Get(),
                               d->screenWidth, d->screenHeight,
                               currentSource->srv.Get());
        currentSource = dest;
    }

    // ====================================================================
    // FINAL: Copy result back to the original back-buffer
    // ====================================================================
    {
        // Unbind SRVs to avoid hazards
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        m_context->PSSetShaderResources(0, 2, nullSRVs);

        // Set the original back-buffer as render target
        ID3D11RenderTargetView* rtvs[] = { originalRTV.Get() };
        m_context->OMSetRenderTargets(1, rtvs, originalDSV.Get());
        PPInternal::SetViewport(m_context, d->screenWidth, d->screenHeight);

        // Bind the final result
        ID3D11ShaderResourceView* srvs[] = { currentSource->srv.Get() };
        m_context->PSSetShaderResources(0, 1, srvs);

        m_context->PSSetShader(d->copyPS.Get(), nullptr, 0);
        PPInternal::DrawFullScreenTriangle(m_context, d);

        // Unbind
        m_context->PSSetShaderResources(0, 1, nullSRVs);
    }

    // Restore original render target and depth-stencil for subsequent passes
    {
        ID3D11RenderTargetView* rtvs[] = { originalRTV.Get() };
        m_context->OMSetRenderTargets(1, rtvs, originalDSV.Get());
    }
}

void PostProcessingSystem::Console_SetExposure(float exposure)
{
    auto* d = PPInternal::GetData(this);
    if (d)
    {
        d->toneMap.exposure = std::max(0.01f, exposure);
    }

    Spark::SimpleConsole::GetInstance().LogInfo(
        "Set post-processing exposure to: " + std::to_string(exposure));
}

std::string PostProcessingSystem::Console_ListEffects() const
{
    auto* d = PPInternal::GetData(this);
    if (!d)
    {
        return "Post-processing system not initialized.";
    }

    std::ostringstream ss;
    ss << "=== Post-Processing Effects ===\n\n";

    ss << "1. Bloom          [" << (d->bloom.enabled ? "ON" : "OFF") << "]\n";
    ss << "   Threshold:     " << d->bloom.threshold << "\n";
    ss << "   Intensity:     " << d->bloom.intensity << "\n";
    ss << "   Mip levels:    " << kBloomMipCount << "\n\n";

    ss << "2. Tone Mapping   [" << (d->toneMap.enabled ? "ON" : "OFF") << "]\n";
    ss << "   Operator:      ";
    switch (d->toneMap.op)
    {
        case ToneMapOperator::Reinhard:   ss << "Reinhard";       break;
        case ToneMapOperator::ACESFilmic: ss << "ACES Filmic";    break;
        case ToneMapOperator::Uncharted2: ss << "Uncharted 2";    break;
    }
    ss << "\n";
    ss << "   Exposure:      " << d->toneMap.exposure << "\n";
    ss << "   Gamma:         " << d->toneMap.gamma << "\n\n";

    ss << "3. Color Grading  [" << (d->colorGrade.enabled ? "ON" : "OFF") << "]\n";
    ss << "   Contrast:      " << d->colorGrade.contrast << "\n";
    ss << "   Saturation:    " << d->colorGrade.saturation << "\n\n";

    ss << "4. FXAA           [" << (d->fxaa.enabled ? "ON" : "OFF") << "]\n";
    ss << "   SubPixel:      " << d->fxaa.qualitySubpix << "\n";
    ss << "   EdgeThreshold: " << d->fxaa.qualityEdgeThreshold << "\n";
    ss << "   EdgeThreshMin: " << d->fxaa.qualityEdgeThresholdMin << "\n\n";

    ss << "Resolution: " << d->screenWidth << "x" << d->screenHeight << "\n";
    ss << "Pipeline order: Scene -> Bloom -> ToneMap -> ColorGrade -> FXAA -> Output\n";

    return ss.str();
}

#endif // SPARK_PLATFORM_WINDOWS
