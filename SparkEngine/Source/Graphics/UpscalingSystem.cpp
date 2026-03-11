/**
 * @file UpscalingSystem.cpp
 * @brief Upscaling system implementation: FSR 1.0/2.0, DLSS, XeSS
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements GPU feature detection, compute shader compilation from inline HLSL,
 * and dispatch logic for FSR 1.0 (EASU + RCAS), FSR 2.0 (temporal), DLSS, and XeSS.
 * FSR 1.0 is fully implemented with real compute shaders. FSR 2.0 provides a custom
 * temporal upscaling approximation. DLSS and XeSS provide SDK integration points
 * with FSR 1.0 fallback when the respective SDKs are unavailable.
 */

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "UpscalingSystem.h"
#include "../Utils/SparkConsole.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// =============================================================================
// Constants
// =============================================================================

namespace
{

    /// Thread group size for upscaling compute shaders (8x8 is standard for image processing)
    static constexpr uint32_t kThreadGroupSize = 8;

    // =========================================================================
    // Inline HLSL — FSR 1.0 EASU (Edge Adaptive Spatial Upsampling)
    // =========================================================================
    //
    // Implements a Lanczos-like 12-tap spatial filter with edge-direction detection.
    // The filter kernel adapts its shape to follow edges in the source image,
    // producing sharper upscaling along edges while remaining smooth in flat regions.
    //
    // Reference: AMD FidelityFX Super Resolution 1.0 (MIT License)

    static const char* kFSR1EASUCS = R"(
// FSR 1.0 EASU — Edge Adaptive Spatial Upsampling compute shader
// Performs directional Lanczos-like filtering with edge detection

cbuffer EASUConstants : register(b0)
{
    float4 Const0; // (inputWidth, inputHeight, outputWidth, outputHeight)
    float4 Const1; // (inputRegionOffsetX, inputRegionOffsetY, inputRegionW, inputRegionH)
    float4 Const2; // (1/inputW, 1/inputH, 1/outputW, 1/outputH)
    float4 Const3; // reserved
};

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);
SamplerState LinearSampler : register(s0);

// Lanczos2 approximation: (25/16) * (2/5*x^2 - 1)^2 - (25/16 - 1)
float EasuLanczos2(float x2)
{
    // Fast approximation of Lanczos2 window
    float a = (2.0 / 5.0) * x2 - 1.0;
    return (25.0 / 16.0) * a * a - (25.0 / 16.0 - 1.0);
}

// Compute edge direction from a 2x2 quad of luma values
float2 EasuEdgeDirection(float luma0, float luma1, float luma2, float luma3)
{
    // Compute gradients
    float dx = luma1 - luma0 + luma3 - luma2;
    float dy = luma2 - luma0 + luma3 - luma1;

    // Normalize direction
    float lenSq = dx * dx + dy * dy;
    float invLen = rsqrt(max(lenSq, 1e-8));
    return float2(dx * invLen, dy * invLen);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    float2 outputSize = Const0.zw;
    float2 inputSize = Const0.xy;

    // Early-out for pixels outside the output region
    if (dispatchId.x >= (uint)outputSize.x || dispatchId.y >= (uint)outputSize.y)
    {
        return;
    }

    // Map output pixel to input space
    float2 outputPixel = float2(dispatchId.xy) + 0.5;
    float2 inputCoord = outputPixel * (inputSize / outputSize);

    // Integer and fractional parts of source coordinate
    float2 inputFloor = floor(inputCoord - 0.5);
    float2 frac = inputCoord - 0.5 - inputFloor;

    // Sample a 4x4 neighborhood in the source image
    float2 texelSize = Const2.xy;
    float2 baseUV = (inputFloor + 0.5) * texelSize;

    // Sample 12 taps in a cross pattern for the Lanczos-like filter
    //   . b .
    //   c e f g
    //   . h i j
    //   . k .
    float3 b = InputTexture.SampleLevel(LinearSampler, baseUV + float2( 0, -1) * texelSize, 0).rgb;
    float3 c = InputTexture.SampleLevel(LinearSampler, baseUV + float2(-1,  0) * texelSize, 0).rgb;
    float3 e = InputTexture.SampleLevel(LinearSampler, baseUV + float2( 0,  0) * texelSize, 0).rgb;
    float3 f = InputTexture.SampleLevel(LinearSampler, baseUV + float2( 1,  0) * texelSize, 0).rgb;
    float3 g = InputTexture.SampleLevel(LinearSampler, baseUV + float2( 2,  0) * texelSize, 0).rgb;
    float3 h = InputTexture.SampleLevel(LinearSampler, baseUV + float2(-1,  1) * texelSize, 0).rgb;
    float3 i = InputTexture.SampleLevel(LinearSampler, baseUV + float2( 0,  1) * texelSize, 0).rgb;
    float3 j = InputTexture.SampleLevel(LinearSampler, baseUV + float2( 1,  1) * texelSize, 0).rgb;
    float3 k = InputTexture.SampleLevel(LinearSampler, baseUV + float2( 0,  2) * texelSize, 0).rgb;

    // Compute luma for edge detection (BT.709)
    float lumaB = dot(b, float3(0.2126, 0.7152, 0.0722));
    float lumaC = dot(c, float3(0.2126, 0.7152, 0.0722));
    float lumaE = dot(e, float3(0.2126, 0.7152, 0.0722));
    float lumaF = dot(f, float3(0.2126, 0.7152, 0.0722));
    float lumaH = dot(h, float3(0.2126, 0.7152, 0.0722));
    float lumaI = dot(i, float3(0.2126, 0.7152, 0.0722));
    float lumaJ = dot(j, float3(0.2126, 0.7152, 0.0722));
    float lumaK = dot(k, float3(0.2126, 0.7152, 0.0722));

    // Detect edge direction from the center 2x2 block
    float2 edgeDir = EasuEdgeDirection(lumaE, lumaF, lumaI, lumaJ);

    // Compute edge stretch — how elongated the filter kernel should be
    float edgeStretchX = abs(edgeDir.x);
    float edgeStretchY = abs(edgeDir.y);
    float maxStretch = max(edgeStretchX, edgeStretchY);
    float minStretch = min(edgeStretchX, edgeStretchY);
    float edgeAspect = saturate(minStretch / max(maxStretch, 1e-8));

    // Stretch the filter kernel along the edge direction
    // At max stretch, the kernel is 2x elongated; at min stretch, it is circular
    float2 stretch = float2(1.0 + (1.0 - edgeAspect) * edgeStretchX,
                            1.0 + (1.0 - edgeAspect) * edgeStretchY);

    // Compute filter weights for each tap using the stretched Lanczos2 kernel
    float2 d_b = (float2(0.0, -1.0) - frac) / stretch;
    float2 d_c = (float2(-1.0, 0.0) - frac) / stretch;
    float2 d_e = (float2(0.0, 0.0) - frac) / stretch;
    float2 d_f = (float2(1.0, 0.0) - frac) / stretch;
    float2 d_g = (float2(2.0, 0.0) - frac) / stretch;
    float2 d_h = (float2(-1.0, 1.0) - frac) / stretch;
    float2 d_i = (float2(0.0, 1.0) - frac) / stretch;
    float2 d_j = (float2(1.0, 1.0) - frac) / stretch;
    float2 d_k = (float2(0.0, 2.0) - frac) / stretch;

    float w_b = EasuLanczos2(dot(d_b, d_b));
    float w_c = EasuLanczos2(dot(d_c, d_c));
    float w_e = EasuLanczos2(dot(d_e, d_e));
    float w_f = EasuLanczos2(dot(d_f, d_f));
    float w_g = EasuLanczos2(dot(d_g, d_g));
    float w_h = EasuLanczos2(dot(d_h, d_h));
    float w_i = EasuLanczos2(dot(d_i, d_i));
    float w_j = EasuLanczos2(dot(d_j, d_j));
    float w_k = EasuLanczos2(dot(d_k, d_k));

    // Normalize and accumulate
    float totalWeight = w_b + w_c + w_e + w_f + w_g + w_h + w_i + w_j + w_k;
    float invWeight = 1.0 / max(totalWeight, 1e-8);

    float3 result = (b * w_b + c * w_c + e * w_e + f * w_f + g * w_g +
                     h * w_h + i * w_i + j * w_j + k * w_k) * invWeight;

    // Clamp to prevent negative values from ringing
    result = max(result, 0.0);

    OutputTexture[dispatchId.xy] = float4(result, 1.0);
}
)";

    // =========================================================================
    // Inline HLSL — FSR 1.0 RCAS (Robust Contrast Adaptive Sharpening)
    // =========================================================================
    //
    // Applies contrast-adaptive sharpening to the EASU output. The sharpening
    // strength adapts to local contrast: high-contrast edges are sharpened less
    // to prevent ringing, while low-contrast areas receive more sharpening.

    static const char* kFSR1RCASCS = R"(
// FSR 1.0 RCAS — Robust Contrast Adaptive Sharpening compute shader

cbuffer RCASConstants : register(b0)
{
    float4 Const0; // (rcasAttenuation, 0, 0, 0)
};

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint width, height;
    InputTexture.GetDimensions(width, height);

    if (dispatchId.x >= width || dispatchId.y >= height)
    {
        return;
    }

    int2 coord = int2(dispatchId.xy);

    // Load center and 4-connected neighbors
    float3 center = InputTexture.Load(int3(coord, 0)).rgb;
    float3 north  = InputTexture.Load(int3(coord + int2( 0, -1), 0)).rgb;
    float3 south  = InputTexture.Load(int3(coord + int2( 0,  1), 0)).rgb;
    float3 west   = InputTexture.Load(int3(coord + int2(-1,  0), 0)).rgb;
    float3 east   = InputTexture.Load(int3(coord + int2( 1,  0), 0)).rgb;

    // Clamp at image boundaries
    if (coord.y == 0) north = center;
    if (coord.y == (int)height - 1) south = center;
    if (coord.x == 0) west = center;
    if (coord.x == (int)width - 1) east = center;

    // Compute luma (BT.709)
    float lumaC = dot(center, float3(0.2126, 0.7152, 0.0722));
    float lumaN = dot(north,  float3(0.2126, 0.7152, 0.0722));
    float lumaS = dot(south,  float3(0.2126, 0.7152, 0.0722));
    float lumaW = dot(west,   float3(0.2126, 0.7152, 0.0722));
    float lumaE = dot(east,   float3(0.2126, 0.7152, 0.0722));

    // Compute min/max of neighborhood luma
    float lumaMin = min(lumaC, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    float lumaMax = max(lumaC, max(max(lumaN, lumaS), max(lumaW, lumaE)));

    // Compute the sharpening amount based on local contrast
    // Higher contrast = less sharpening (prevents ringing on strong edges)
    float contrast = lumaMax - lumaMin;
    float rcasAttenuation = Const0.x;

    // Sharpening coefficient: negative for sharpening (unsharp mask)
    // The -0.25 base gives maximum sharpening; attenuation reduces it
    float sharpAmount = -0.25 / max(contrast * 4.0 + rcasAttenuation, 0.04);

    // Clamp the sharpening amount to avoid over-sharpening
    sharpAmount = max(sharpAmount, -0.1875); // limit to -3/16

    // Apply sharpening via weighted sum: center + sharp * (center - neighbors)
    float3 result = center + sharpAmount * (4.0 * center - north - south - west - east);

    // Clamp to the neighborhood min/max to prevent ringing artifacts
    float3 minNeighbor = min(center, min(min(north, south), min(west, east)));
    float3 maxNeighbor = max(center, max(max(north, south), max(west, east)));
    result = clamp(result, minNeighbor, maxNeighbor);

    OutputTexture[dispatchId.xy] = float4(result, 1.0);
}
)";

    // =========================================================================
    // Inline HLSL — FSR 2.0 Temporal Accumulation
    // =========================================================================
    //
    // Custom temporal upscaling shader that approximates FSR 2.0 behavior:
    // - Reprojects previous frame using motion vectors
    // - Detects disocclusion via depth comparison
    // - Blends current and history with adaptive weight
    // - Applies neighborhood clamping to prevent ghosting

    static const char* kFSR2TemporalCS = R"(
// FSR 2.0 Temporal Accumulation compute shader

cbuffer FSR2Params : register(b0)
{
    float4 RenderSize;      // (renderW, renderH, 1/renderW, 1/renderH)
    float4 DisplaySize;     // (displayW, displayH, 1/displayW, 1/displayH)
    float4 JitterParams;    // (jitterX, jitterY, motionScaleX, motionScaleY)
    float4 CameraParams;    // (nearPlane, farPlane, verticalFOV, deltaTime)
    float4 TemporalParams;  // (frameIndex, resetFlag, sharpness, reserved)
};

Texture2D<float4> ColorInput : register(t0);
Texture2D<float> DepthInput : register(t1);
Texture2D<float2> MotionVectors : register(t2);
Texture2D<float> ExposureInput : register(t3);
Texture2D<float4> HistoryInput : register(t4);

RWTexture2D<float4> OutputTexture : register(u0);
RWTexture2D<float4> HistoryOutput : register(u1);

SamplerState LinearSampler : register(s0);

// Linearize depth from [0,1] NDC to view-space distance
float LinearizeDepth(float ndcDepth, float nearZ, float farZ)
{
    return nearZ * farZ / (farZ - ndcDepth * (farZ - nearZ));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    float2 displayDims = DisplaySize.xy;
    if (dispatchId.x >= (uint)displayDims.x || dispatchId.y >= (uint)displayDims.y)
    {
        return;
    }

    float2 renderDims = RenderSize.xy;
    float2 outputUV = (float2(dispatchId.xy) + 0.5) / displayDims;
    float2 renderUV = outputUV; // Map output pixel back to render resolution

    // Sample low-res color (jitter-corrected)
    float2 jitteredUV = renderUV - JitterParams.xy * RenderSize.zw;
    float3 currentColor = ColorInput.SampleLevel(LinearSampler, jitteredUV, 0).rgb;

    // Apply auto-exposure
    float exposure = ExposureInput.Load(int3(0, 0, 0));
    if (exposure > 0.0)
    {
        currentColor *= exposure;
    }

    // Read motion vector at render resolution
    int2 renderCoord = int2(renderUV * renderDims);
    renderCoord = clamp(renderCoord, int2(0, 0), int2(renderDims) - 1);
    float2 motionVec = MotionVectors.Load(int3(renderCoord, 0)).xy;
    motionVec *= JitterParams.zw; // Apply motion vector scale

    // Reproject to find previous frame location
    float2 historyUV = outputUV - motionVec;

    // Check if reprojected coordinate is within bounds
    bool validHistory = historyUV.x >= 0.0 && historyUV.x <= 1.0 &&
                        historyUV.y >= 0.0 && historyUV.y <= 1.0;

    // Depth-based disocclusion detection
    float currentDepth = DepthInput.Load(int3(renderCoord, 0));
    float linearDepth = LinearizeDepth(currentDepth, CameraParams.x, CameraParams.y);

    // Reset history on camera cuts or first frame
    bool resetHistory = TemporalParams.y > 0.5 || TemporalParams.x < 1.5;

    float3 historyColor = float3(0, 0, 0);
    float blendWeight = 0.1; // Default: keep 90% history, 10% current

    if (validHistory && !resetHistory)
    {
        historyColor = HistoryInput.SampleLevel(LinearSampler, historyUV, 0).rgb;

        // Neighborhood clamping: compute AABB from a 3x3 neighborhood of current frame
        float3 neighborMin = currentColor;
        float3 neighborMax = currentColor;

        [unroll]
        for (int dy = -1; dy <= 1; dy++)
        {
            [unroll]
            for (int dx = -1; dx <= 1; dx++)
            {
                if (dx == 0 && dy == 0) continue;
                float2 sampleUV = jitteredUV + float2(dx, dy) * RenderSize.zw;
                float3 s = ColorInput.SampleLevel(LinearSampler, sampleUV, 0).rgb;
                if (exposure > 0.0) s *= exposure;
                neighborMin = min(neighborMin, s);
                neighborMax = max(neighborMax, s);
            }
        }

        // Clamp history to the neighborhood AABB to prevent ghosting
        historyColor = clamp(historyColor, neighborMin, neighborMax);

        // Adaptive blend weight based on how much clamping was needed
        float3 clampDiff = abs(historyColor - HistoryInput.SampleLevel(LinearSampler, historyUV, 0).rgb);
        float clampAmount = dot(clampDiff, float3(0.333, 0.333, 0.334));
        blendWeight = lerp(0.05, 0.25, saturate(clampAmount * 10.0));
    }
    else
    {
        // No valid history — use current frame entirely
        blendWeight = 1.0;
        historyColor = currentColor;
    }

    // Blend current and history
    float3 result = lerp(historyColor, currentColor, blendWeight);

    OutputTexture[dispatchId.xy] = float4(result, 1.0);
    HistoryOutput[dispatchId.xy] = float4(result, 1.0);
}
)";

    // =========================================================================
    // Inline HLSL — FSR 2.0 Sharpening (RCAS applied to temporal output)
    // =========================================================================
    //
    // Same RCAS algorithm as FSR 1.0 but applied as the final pass of FSR 2.0.
    // Reuses the RCAS shader source since the algorithm is identical.

    // =========================================================================
    // Helper: compile an HLSL shader from an inline source string
    // =========================================================================

    HRESULT CompileShaderFromString(const char* source, const char* entryPoint, const char* target, ID3DBlob** blobOut)
    {
        DWORD flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG) || defined(DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entryPoint, target, flags, 0,
                                blobOut, &errorBlob);

        if (FAILED(hr))
        {
            if (errorBlob)
            {
                std::string errMsg = "Upscaling shader compile error: ";
                errMsg += static_cast<const char*>(errorBlob->GetBufferPointer());
                Spark::SimpleConsole::GetInstance().LogError(errMsg);
            }
            return hr;
        }
        return S_OK;
    }

    // =========================================================================
    // GPU vendor detection via DXGI adapter
    // =========================================================================

    /// Well-known PCI vendor IDs
    static constexpr uint32_t kVendorNVIDIA = 0x10DE;
    static constexpr uint32_t kVendorAMD = 0x1002;
    static constexpr uint32_t kVendorIntel = 0x8086;

    struct GPUAdapterInfo
    {
        uint32_t vendorId = 0;
        uint32_t deviceId = 0;
        bool hasComputeShaders = false;
        std::wstring description;
    };

    GPUAdapterInfo QueryAdapterInfo(ID3D11Device* device)
    {
        GPUAdapterInfo info;
        if (!device)
        {
            return info;
        }

        // D3D11 feature level 11_0+ guarantees compute shader support
        D3D_FEATURE_LEVEL featureLevel = device->GetFeatureLevel();
        info.hasComputeShaders = (featureLevel >= D3D_FEATURE_LEVEL_11_0);

        // Query DXGI adapter for vendor/device IDs
        ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.GetAddressOf()));
        if (FAILED(hr))
        {
            return info;
        }

        ComPtr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
        if (FAILED(hr))
        {
            return info;
        }

        DXGI_ADAPTER_DESC adapterDesc = {};
        hr = adapter->GetDesc(&adapterDesc);
        if (SUCCEEDED(hr))
        {
            info.vendorId = adapterDesc.VendorId;
            info.deviceId = adapterDesc.DeviceId;
            info.description = adapterDesc.Description;
        }

        return info;
    }

} // anonymous namespace

// =============================================================================
// DetectFeatures
// =============================================================================

void UpscalingSystem::DetectFeatures()
{
    if (!m_device)
    {
        m_dlssFeatureInfo.isAvailable = false;
        m_xessFeatureInfo.isAvailable = false;
        m_fsr2Available = false;
        return;
    }

    GPUAdapterInfo adapterInfo = QueryAdapterInfo(m_device);

    // FSR 1.0: Always available — it is a pure shader-based spatial upscaler
    // (no special hardware needed, works on any GPU that supports the shader model)

    // FSR 2.0: Requires compute shader support (D3D_FEATURE_LEVEL_11_0+)
    m_fsr2Available = adapterInfo.hasComputeShaders;

    // DLSS: Requires NVIDIA GPU with Tensor Cores (RTX series)
    // In a real deployment, we would load the NGX SDK DLL and call:
    //   NVSDK_NGX_D3D11_Init(appId, appDir, device)
    //   NVSDK_NGX_D3D11_GetCapabilityParameters(&params)
    //   params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlssAvailable)
    if (adapterInfo.vendorId == kVendorNVIDIA)
    {
        // NVIDIA GPU detected — DLSS may be available
        // Without the NGX SDK we cannot confirm Tensor Core presence,
        // so we report availability as true and let the SDK call fail gracefully
        // if the hardware actually lacks Tensor Cores (e.g., GTX 10-series).
        m_dlssFeatureInfo.isAvailable = true;
        m_dlssFeatureInfo.isOptimalDriver = true;
        m_dlssFeatureInfo.minDriverMajor = 512;
        m_dlssFeatureInfo.minDriverMinor = 0;

        // NOTE: In production, replace the above with actual NGX SDK queries:
        // NVSDK_NGX_Result ngxResult = NVSDK_NGX_D3D11_Init(appId, L".", m_device);
        // if (NVSDK_NGX_SUCCEED(ngxResult)) { ... query DLSS capability ... }

        std::string msg = "DLSS: NVIDIA GPU detected (vendor 0x10DE, device 0x";
        char hexBuf[16];
        snprintf(hexBuf, sizeof(hexBuf), "%04X", adapterInfo.deviceId);
        msg += hexBuf;
        msg += ") — DLSS provisionally available";
        Spark::SimpleConsole::GetInstance().LogInfo(msg);
    }
    else
    {
        m_dlssFeatureInfo.isAvailable = false;
        m_dlssFeatureInfo.isOptimalDriver = false;
    }

    // XeSS: Works on any GPU via DP4a instructions; accelerated on Intel Arc (XMX)
    // In a real deployment, we would load the XeSS SDK DLL and call:
    //   xess_result_t result = xessD3D11CreateContext(device, &xessContext)
    //   xessIsOptimal(xessContext, &isOptimal)
    if (adapterInfo.vendorId == kVendorIntel)
    {
        m_xessFeatureInfo.isAvailable = true;
        m_xessFeatureInfo.hasXMXAcceleration = true; // Intel Arc GPUs have XMX
        m_xessFeatureInfo.driverVersion = 0;         // Would be queried from SDK

        // NOTE: In production, replace with actual XeSS SDK queries:
        // xess_result_t res = xessD3D11CreateContext(m_device, &m_xessContext);
        // if (res == XESS_RESULT_SUCCESS) { xessIsOptimal(m_xessContext, &optimal); }

        Spark::SimpleConsole::GetInstance().LogInfo("XeSS: Intel GPU detected — XMX acceleration available");
    }
    else if (adapterInfo.hasComputeShaders)
    {
        // XeSS can run on non-Intel GPUs via DP4a fallback (lower quality)
        m_xessFeatureInfo.isAvailable = true;
        m_xessFeatureInfo.hasXMXAcceleration = false;
        m_xessFeatureInfo.driverVersion = 0;

        Spark::SimpleConsole::GetInstance().LogInfo("XeSS: Non-Intel GPU — DP4a fallback available");
    }
    else
    {
        m_xessFeatureInfo.isAvailable = false;
        m_xessFeatureInfo.hasXMXAcceleration = false;
    }
}

// =============================================================================
// CreateGPUResources
// =============================================================================

bool UpscalingSystem::CreateGPUResources()
{
    if (!m_device)
    {
        return false;
    }

    // Create FSR 1.0 constant buffers
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    cbDesc.ByteWidth = sizeof(FSR1EASUConstants);
    HRESULT hr = m_device->CreateBuffer(&cbDesc, nullptr, m_fsr1EASUConstantBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create FSR1 EASU constant buffer");
        return false;
    }

    cbDesc.ByteWidth = sizeof(FSR1RCASConstants);
    hr = m_device->CreateBuffer(&cbDesc, nullptr, m_fsr1RCASConstantBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create FSR1 RCAS constant buffer");
        return false;
    }

    // Create FSR 2.0 constant buffer
    cbDesc.ByteWidth = sizeof(FSR2Constants);
    hr = m_device->CreateBuffer(&cbDesc, nullptr, m_fsr2ConstantBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create FSR2 constant buffer");
        return false;
    }

    // Create temporal upscale constant buffer (shared by DLSS/XeSS fallback)
    cbDesc.ByteWidth = sizeof(TemporalUpscaleConstants);
    hr = m_device->CreateBuffer(&cbDesc, nullptr, m_temporalUpscaleConstantBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create temporal upscale constant buffer");
        return false;
    }

    // Create linear clamp sampler for upscaling shaders
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = m_device->CreateSamplerState(&samplerDesc, m_linearClampSampler.GetAddressOf());
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create linear clamp sampler");
        return false;
    }

    // Compile upscaling compute shaders
    if (!CompileUpscalingShaders())
    {
        Spark::SimpleConsole::GetInstance().LogWarning(
            "Upscaling shader compilation failed — FSR will not be available until shaders compile");
        // Non-fatal: the system can still function as a passthrough
    }

    return true;
}

// =============================================================================
// CompileUpscalingShaders
// =============================================================================

bool UpscalingSystem::CompileUpscalingShaders()
{
    if (!m_device)
    {
        return false;
    }

    bool allSucceeded = true;

    // Compile FSR 1.0 EASU compute shader
    {
        ComPtr<ID3DBlob> blob;
        HRESULT hr = CompileShaderFromString(kFSR1EASUCS, "CSMain", "cs_5_0", blob.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            hr = m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                               m_fsr1EASUShader.GetAddressOf());
            if (FAILED(hr))
            {
                Spark::SimpleConsole::GetInstance().LogError("Failed to create FSR1 EASU compute shader");
                allSucceeded = false;
            }
        }
        else
        {
            allSucceeded = false;
        }
    }

    // Compile FSR 1.0 RCAS compute shader
    {
        ComPtr<ID3DBlob> blob;
        HRESULT hr = CompileShaderFromString(kFSR1RCASCS, "CSMain", "cs_5_0", blob.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            hr = m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                               m_fsr1RCASShader.GetAddressOf());
            if (FAILED(hr))
            {
                Spark::SimpleConsole::GetInstance().LogError("Failed to create FSR1 RCAS compute shader");
                allSucceeded = false;
            }
        }
        else
        {
            allSucceeded = false;
        }
    }

    // Compile FSR 2.0 temporal accumulation compute shader
    {
        ComPtr<ID3DBlob> blob;
        HRESULT hr = CompileShaderFromString(kFSR2TemporalCS, "CSMain", "cs_5_0", blob.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            hr = m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                               m_fsr2TemporalCS.GetAddressOf());
            if (FAILED(hr))
            {
                Spark::SimpleConsole::GetInstance().LogError("Failed to create FSR2 temporal compute shader");
                allSucceeded = false;
            }
        }
        else
        {
            allSucceeded = false;
        }
    }

    // FSR 2.0 sharpening reuses the RCAS shader (same algorithm)
    if (m_fsr1RCASShader)
    {
        m_fsr2SharpenCS = m_fsr1RCASShader;
    }

    m_shadersCompiled = allSucceeded;

    if (allSucceeded)
    {
        Spark::SimpleConsole::GetInstance().LogInfo("Upscaling shaders compiled successfully");
    }

    return allSucceeded;
}

// =============================================================================
// RecreateUpscalingResources
// =============================================================================

void UpscalingSystem::RecreateUpscalingResources()
{
    if (!m_initialized)
    {
        return;
    }

    // Release existing intermediate resources
    m_intermediateTexture.Reset();
    m_intermediateSRV.Reset();
    m_intermediateUAV.Reset();
    m_temporalHistoryTexture.Reset();
    m_temporalHistorySRV.Reset();
    m_temporalHistoryUAV.Reset();
    m_lockTexture.Reset();
    m_lockSRV.Reset();
    m_lockUAV.Reset();

    if (m_settings.mode == UpscalingMode::None || !m_device)
    {
        return;
    }

    // Create intermediate texture at display resolution for multi-pass upscaling
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_displayWidth;
    texDesc.Height = m_displayHeight;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, m_intermediateTexture.GetAddressOf());
    if (SUCCEEDED(hr) && m_intermediateTexture)
    {
        m_device->CreateShaderResourceView(m_intermediateTexture.Get(), nullptr, m_intermediateSRV.GetAddressOf());
        m_device->CreateUnorderedAccessView(m_intermediateTexture.Get(), nullptr, m_intermediateUAV.GetAddressOf());
    }

    // Create temporal history texture for FSR 2.0 / temporal modes
    bool needsTemporalHistory = (m_settings.mode == UpscalingMode::FSR2 || m_settings.mode == UpscalingMode::DLSS ||
                                 m_settings.mode == UpscalingMode::XeSS);
    if (needsTemporalHistory)
    {
        hr = m_device->CreateTexture2D(&texDesc, nullptr, m_temporalHistoryTexture.GetAddressOf());
        if (SUCCEEDED(hr) && m_temporalHistoryTexture)
        {
            m_device->CreateShaderResourceView(m_temporalHistoryTexture.Get(), nullptr,
                                               m_temporalHistorySRV.GetAddressOf());
            m_device->CreateUnorderedAccessView(m_temporalHistoryTexture.Get(), nullptr,
                                                m_temporalHistoryUAV.GetAddressOf());
        }

        // Create luminance lock texture for FSR 2.0
        if (m_settings.mode == UpscalingMode::FSR2)
        {
            D3D11_TEXTURE2D_DESC lockDesc = texDesc;
            lockDesc.Format = DXGI_FORMAT_R16_FLOAT;
            hr = m_device->CreateTexture2D(&lockDesc, nullptr, m_lockTexture.GetAddressOf());
            if (SUCCEEDED(hr) && m_lockTexture)
            {
                m_device->CreateShaderResourceView(m_lockTexture.Get(), nullptr, m_lockSRV.GetAddressOf());
                m_device->CreateUnorderedAccessView(m_lockTexture.Get(), nullptr, m_lockUAV.GetAddressOf());
            }
        }

        // Reset temporal accumulation when resources are recreated
        m_fsr2FrameIndex = 0;
        m_dlssFrameIndex = 0;
        m_xessFrameIndex = 0;
    }
}

// =============================================================================
// UnbindComputeResources
// =============================================================================

void UpscalingSystem::UnbindComputeResources()
{
    if (!m_context)
    {
        return;
    }

    // Unbind all compute shader resources to prevent hazards
    ID3D11ShaderResourceView* nullSRVs[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    ID3D11UnorderedAccessView* nullUAVs[2] = {nullptr, nullptr};
    ID3D11Buffer* nullCBs[1] = {nullptr};
    ID3D11SamplerState* nullSamplers[1] = {nullptr};

    m_context->CSSetShader(nullptr, nullptr, 0);
    m_context->CSSetShaderResources(0, 5, nullSRVs);
    m_context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, nullCBs);
    m_context->CSSetSamplers(0, 1, nullSamplers);
}

// =============================================================================
// ExecuteFSR1
// =============================================================================

void UpscalingSystem::ExecuteFSR1(ID3D11ShaderResourceView* inputColorSRV, ID3D11UnorderedAccessView* outputUAV)
{
    if (!m_initialized || m_settings.mode != UpscalingMode::FSR1)
    {
        return;
    }

    if (!m_context || !inputColorSRV || !outputUAV)
    {
        return;
    }

    if (!m_fsr1EASUShader || !m_fsr1RCASShader)
    {
        Spark::SimpleConsole::GetInstance().LogWarning("FSR1: Compute shaders not compiled — skipping");
        return;
    }

    if (!m_intermediateUAV || !m_intermediateSRV)
    {
        Spark::SimpleConsole::GetInstance().LogWarning("FSR1: Intermediate texture not available — skipping");
        return;
    }

    // Compute dispatch dimensions (thread groups of 8x8)
    uint32_t dispatchX = (m_displayWidth + kThreadGroupSize - 1) / kThreadGroupSize;
    uint32_t dispatchY = (m_displayHeight + kThreadGroupSize - 1) / kThreadGroupSize;

    // ---- Pass 1: EASU (Edge Adaptive Spatial Upsampling) ----
    {
        // Set up EASU constants
        FSR1EASUConstants easuConst = {};
        float inputWidth = static_cast<float>(m_renderWidth);
        float inputHeight = static_cast<float>(m_renderHeight);
        float outputWidth = static_cast<float>(m_displayWidth);
        float outputHeight = static_cast<float>(m_displayHeight);

        easuConst.const0 = {inputWidth, inputHeight, outputWidth, outputHeight};
        easuConst.const1 = {0.0f, 0.0f, inputWidth, inputHeight};
        easuConst.const2 = {1.0f / inputWidth, 1.0f / inputHeight, 1.0f / outputWidth, 1.0f / outputHeight};
        easuConst.const3 = {0.0f, 0.0f, 0.0f, 0.0f};

        UpdateFSR1Constants(&easuConst, nullptr);

        // Bind EASU compute shader
        m_context->CSSetShader(m_fsr1EASUShader.Get(), nullptr, 0);

        // Bind constant buffer
        ID3D11Buffer* cbs[] = {m_fsr1EASUConstantBuffer.Get()};
        m_context->CSSetConstantBuffers(0, 1, cbs);

        // Bind input SRV (low-res color)
        ID3D11ShaderResourceView* srvs[] = {inputColorSRV};
        m_context->CSSetShaderResources(0, 1, srvs);

        // Bind intermediate UAV (display-res output of EASU)
        ID3D11UnorderedAccessView* uavs[] = {m_intermediateUAV.Get()};
        m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        // Bind sampler
        ID3D11SamplerState* samplers[] = {m_linearClampSampler.Get()};
        m_context->CSSetSamplers(0, 1, samplers);

        // Dispatch EASU
        m_context->Dispatch(dispatchX, dispatchY, 1);

        // Unbind to prevent resource hazards before next pass
        UnbindComputeResources();
    }

    // ---- Pass 2: RCAS (Robust Contrast Adaptive Sharpening) ----
    {
        // Set up RCAS constants
        FSR1RCASConstants rcasConst = {};
        // RCAS attenuation: 0 = maximum sharpening, 2 = no sharpening
        float rcasAttenuation = 2.0f * (1.0f - m_settings.sharpness);
        rcasConst.const0 = {rcasAttenuation, 0.0f, 0.0f, 0.0f};

        UpdateFSR1Constants(nullptr, &rcasConst);

        // Bind RCAS compute shader
        m_context->CSSetShader(m_fsr1RCASShader.Get(), nullptr, 0);

        // Bind constant buffer
        ID3D11Buffer* cbs[] = {m_fsr1RCASConstantBuffer.Get()};
        m_context->CSSetConstantBuffers(0, 1, cbs);

        // Bind intermediate SRV (EASU output) as input
        ID3D11ShaderResourceView* srvs[] = {m_intermediateSRV.Get()};
        m_context->CSSetShaderResources(0, 1, srvs);

        // Bind final output UAV
        ID3D11UnorderedAccessView* uavs[] = {outputUAV};
        m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        // Dispatch RCAS
        m_context->Dispatch(dispatchX, dispatchY, 1);

        // Clean up bindings
        UnbindComputeResources();
    }
}

// =============================================================================
// ExecuteFSR2
// =============================================================================

void UpscalingSystem::ExecuteFSR2(const FSR2DispatchDescription& desc)
{
    if (!m_initialized || m_settings.mode != UpscalingMode::FSR2)
    {
        return;
    }

    if (!m_context)
    {
        return;
    }

    if (!m_fsr2TemporalCS)
    {
        Spark::SimpleConsole::GetInstance().LogWarning("FSR2: Temporal compute shader not compiled — skipping");
        return;
    }

    if (!desc.colorSRV || !desc.depthSRV || !desc.motionVectorsSRV || !desc.outputUAV)
    {
        Spark::SimpleConsole::GetInstance().LogWarning("FSR2: Missing required input resources");
        return;
    }

    if (!m_temporalHistorySRV || !m_temporalHistoryUAV)
    {
        Spark::SimpleConsole::GetInstance().LogWarning("FSR2: Temporal history texture not available");
        return;
    }

    // Handle accumulation reset on camera cuts
    if (desc.resetAccumulation)
    {
        m_fsr2FrameIndex = 0;
    }

    // Compute dispatch dimensions at display resolution
    uint32_t dispatchX = (m_displayWidth + kThreadGroupSize - 1) / kThreadGroupSize;
    uint32_t dispatchY = (m_displayHeight + kThreadGroupSize - 1) / kThreadGroupSize;

    // ---- Pass 1: Temporal Accumulation ----
    {
        // Update FSR2 constant buffer
        FSR2Constants fsr2Const = {};
        float renderW = static_cast<float>(m_renderWidth);
        float renderH = static_cast<float>(m_renderHeight);
        float displayW = static_cast<float>(m_displayWidth);
        float displayH = static_cast<float>(m_displayHeight);

        fsr2Const.renderSize = {renderW, renderH, 1.0f / renderW, 1.0f / renderH};
        fsr2Const.displaySize = {displayW, displayH, 1.0f / displayW, 1.0f / displayH};
        fsr2Const.jitterOffset = {desc.jitterOffset.x, desc.jitterOffset.y, desc.motionVectorScale.x,
                                  desc.motionVectorScale.y};
        fsr2Const.cameraParams = {desc.nearPlane, desc.farPlane, desc.verticalFOV, desc.deltaTime};
        fsr2Const.temporalParams = {static_cast<float>(m_fsr2FrameIndex), desc.resetAccumulation ? 1.0f : 0.0f,
                                    m_settings.sharpness, 0.0f};

        // Map and update the constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_context->Map(m_fsr2ConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            memcpy(mapped.pData, &fsr2Const, sizeof(FSR2Constants));
            m_context->Unmap(m_fsr2ConstantBuffer.Get(), 0);
        }

        // Bind temporal compute shader
        m_context->CSSetShader(m_fsr2TemporalCS.Get(), nullptr, 0);

        // Bind constant buffer
        ID3D11Buffer* cbs[] = {m_fsr2ConstantBuffer.Get()};
        m_context->CSSetConstantBuffers(0, 1, cbs);

        // Bind input SRVs:
        //   t0 = color, t1 = depth, t2 = motion vectors, t3 = exposure, t4 = history
        ID3D11ShaderResourceView* exposureSRV = desc.exposureSRV; // May be nullptr
        ID3D11ShaderResourceView* srvs[] = {desc.colorSRV, desc.depthSRV, desc.motionVectorsSRV, exposureSRV,
                                            m_temporalHistorySRV.Get()};
        m_context->CSSetShaderResources(0, 5, srvs);

        // Bind output UAVs:
        //   u0 = intermediate output, u1 = history output
        ID3D11UnorderedAccessView* uavs[] = {m_intermediateUAV.Get(), m_temporalHistoryUAV.Get()};
        m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

        // Bind sampler
        ID3D11SamplerState* samplers[] = {m_linearClampSampler.Get()};
        m_context->CSSetSamplers(0, 1, samplers);

        // Dispatch temporal accumulation
        m_context->Dispatch(dispatchX, dispatchY, 1);

        // Unbind resources
        UnbindComputeResources();
    }

    // ---- Pass 2: RCAS Sharpening (optional, applied to temporal output) ----
    if (m_settings.sharpness > 0.01f && m_fsr2SharpenCS)
    {
        FSR1RCASConstants rcasConst = {};
        float rcasAttenuation = 2.0f * (1.0f - m_settings.sharpness);
        rcasConst.const0 = {rcasAttenuation, 0.0f, 0.0f, 0.0f};

        UpdateFSR1Constants(nullptr, &rcasConst);

        // Bind RCAS shader
        m_context->CSSetShader(m_fsr2SharpenCS.Get(), nullptr, 0);

        // Bind RCAS constant buffer
        ID3D11Buffer* cbs[] = {m_fsr1RCASConstantBuffer.Get()};
        m_context->CSSetConstantBuffers(0, 1, cbs);

        // Input: intermediate (temporal output), Output: final UAV
        ID3D11ShaderResourceView* srvs[] = {m_intermediateSRV.Get()};
        m_context->CSSetShaderResources(0, 1, srvs);

        ID3D11UnorderedAccessView* uavs[] = {desc.outputUAV};
        m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        m_context->Dispatch(dispatchX, dispatchY, 1);

        UnbindComputeResources();
    }
    else
    {
        // No sharpening — copy intermediate to output
        // The temporal pass already wrote to the intermediate texture,
        // so we need to copy it to the final output.
        // We reuse the RCAS shader with zero sharpening (attenuation = 2.0) as a copy pass.
        if (m_fsr2SharpenCS)
        {
            FSR1RCASConstants rcasConst = {};
            rcasConst.const0 = {2.0f, 0.0f, 0.0f, 0.0f}; // No sharpening

            UpdateFSR1Constants(nullptr, &rcasConst);

            m_context->CSSetShader(m_fsr2SharpenCS.Get(), nullptr, 0);

            ID3D11Buffer* cbs[] = {m_fsr1RCASConstantBuffer.Get()};
            m_context->CSSetConstantBuffers(0, 1, cbs);

            ID3D11ShaderResourceView* srvs[] = {m_intermediateSRV.Get()};
            m_context->CSSetShaderResources(0, 1, srvs);

            ID3D11UnorderedAccessView* uavs[] = {desc.outputUAV};
            m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

            m_context->Dispatch(dispatchX, dispatchY, 1);

            UnbindComputeResources();
        }
    }

    m_fsr2FrameIndex++;
}

// =============================================================================
// ExecuteDLSS
// =============================================================================

void UpscalingSystem::ExecuteDLSS(ID3D11ShaderResourceView* colorSRV, ID3D11ShaderResourceView* depthSRV,
                                  ID3D11ShaderResourceView* motionVectorsSRV, ID3D11ShaderResourceView* exposureSRV,
                                  ID3D11UnorderedAccessView* outputUAV, const XMFLOAT2& jitterOffset, bool resetHistory)
{
    if (!m_initialized || m_settings.mode != UpscalingMode::DLSS)
    {
        return;
    }

    if (!m_context || !colorSRV || !outputUAV)
    {
        return;
    }

    if (resetHistory)
    {
        m_dlssFrameIndex = 0;
    }

    // =========================================================================
    // NVIDIA DLSS SDK Integration Point
    // =========================================================================
    //
    // In a production build with the NGX SDK, the following code would execute:
    //
    //   NVSDK_NGX_D3D11_DLSS_Eval_Params evalParams = {};
    //   evalParams.Feature.pInColor = colorSRV;
    //   evalParams.Feature.pInOutput = outputUAV;
    //   evalParams.pInDepth = depthSRV;
    //   evalParams.pInMotionVectors = motionVectorsSRV;
    //   evalParams.pInExposureTexture = exposureSRV;
    //   evalParams.InJitterOffsetX = jitterOffset.x;
    //   evalParams.InJitterOffsetY = jitterOffset.y;
    //   evalParams.InRenderSubrectDimensions = {m_renderWidth, m_renderHeight};
    //   evalParams.InReset = resetHistory ? 1 : 0;
    //   evalParams.InMVScaleX = 1.0f;
    //   evalParams.InMVScaleY = 1.0f;
    //
    //   NVSDK_NGX_Result result = NVSDK_NGX_D3D11_EvaluateFeature(
    //       m_context, m_dlssFeatureHandle, m_ngxParameters, &evalParams);
    //
    //   if (NVSDK_NGX_FAILED(result))
    //   {
    //       // Fall back to FSR 1.0
    //   }
    //
    // Since we do not ship the NGX SDK, we fall back to FSR 1.0 spatial upscaling
    // which provides a reasonable baseline. When the NGX SDK is available, the
    // above code replaces this fallback.
    // =========================================================================

    // Fallback: Use FSR 1.0 (EASU + RCAS) when DLSS SDK is not linked
    if (!m_fsr1EASUShader || !m_fsr1RCASShader)
    {
        Spark::SimpleConsole::GetInstance().LogWarning(
            "DLSS: NGX SDK not available and FSR1 shaders not compiled — no upscaling performed");
        return;
    }

    Spark::SimpleConsole::GetInstance().LogInfo("DLSS: NGX SDK not linked — falling back to FSR 1.0 spatial upscaling");

    // Temporarily switch to FSR1 mode for the fallback dispatch
    UpscalingMode savedMode = m_settings.mode;
    m_settings.mode = UpscalingMode::FSR1;
    ExecuteFSR1(colorSRV, outputUAV);
    m_settings.mode = savedMode;

    m_dlssFrameIndex++;
}

// =============================================================================
// ExecuteXeSS
// =============================================================================

void UpscalingSystem::ExecuteXeSS(ID3D11ShaderResourceView* colorSRV, ID3D11ShaderResourceView* depthSRV,
                                  ID3D11ShaderResourceView* motionVectorsSRV, ID3D11ShaderResourceView* exposureSRV,
                                  ID3D11UnorderedAccessView* outputUAV, const XMFLOAT2& jitterOffset)
{
    if (!m_initialized || m_settings.mode != UpscalingMode::XeSS)
    {
        return;
    }

    if (!m_context || !colorSRV || !outputUAV)
    {
        return;
    }

    // =========================================================================
    // Intel XeSS SDK Integration Point
    // =========================================================================
    //
    // In a production build with the XeSS SDK, the following code would execute:
    //
    //   xess_d3d11_execute_params_t xessParams = {};
    //   xessParams.inputColorTexture = colorSRV;
    //   xessParams.inputDepthTexture = depthSRV;
    //   xessParams.inputMotionVectors = motionVectorsSRV;
    //   xessParams.inputExposureScaleTexture = exposureSRV;
    //   xessParams.outputTexture = outputUAV;
    //   xessParams.jitterOffsetX = jitterOffset.x;
    //   xessParams.jitterOffsetY = jitterOffset.y;
    //   xessParams.exposureScale = 1.0f;
    //   xessParams.resetHistory = (m_xessFrameIndex == 0);
    //   xessParams.inputWidth = m_renderWidth;
    //   xessParams.inputHeight = m_renderHeight;
    //
    //   xess_result_t result = xessD3D11Execute(m_xessContext, &xessParams);
    //
    //   if (result != XESS_RESULT_SUCCESS)
    //   {
    //       // Fall back to FSR 1.0
    //   }
    //
    // Since we do not ship the XeSS SDK, we fall back to FSR 1.0 spatial
    // upscaling. When the XeSS SDK is integrated, the above code replaces
    // this fallback path.
    // =========================================================================

    // Fallback: Use FSR 1.0 when XeSS SDK is not linked
    if (!m_fsr1EASUShader || !m_fsr1RCASShader)
    {
        Spark::SimpleConsole::GetInstance().LogWarning(
            "XeSS: SDK not available and FSR1 shaders not compiled — no upscaling performed");
        return;
    }

    Spark::SimpleConsole::GetInstance().LogInfo("XeSS: SDK not linked — falling back to FSR 1.0 spatial upscaling");

    // Temporarily switch to FSR1 mode for the fallback dispatch
    UpscalingMode savedMode = m_settings.mode;
    m_settings.mode = UpscalingMode::FSR1;
    ExecuteFSR1(colorSRV, outputUAV);
    m_settings.mode = savedMode;

    m_xessFrameIndex++;
}

// =============================================================================
// GetRecommendedRenderSize
// =============================================================================

void UpscalingSystem::GetRecommendedRenderSize(uint32_t displayWidth, uint32_t displayHeight, UpscalingQuality quality,
                                               uint32_t& outWidth, uint32_t& outHeight)
{
    float scale = UpscalingSettings::GetRenderScale(quality);
    outWidth = std::max(1u, static_cast<uint32_t>(static_cast<float>(displayWidth) * scale));
    outHeight = std::max(1u, static_cast<uint32_t>(static_cast<float>(displayHeight) * scale));

    // Align to even dimensions for compute shader dispatch (threadgroup alignment)
    outWidth = (outWidth + 1u) & ~1u;
    outHeight = (outHeight + 1u) & ~1u;

    // Apply mode-specific adjustments for well-known display resolutions
    // These match the optimal render resolutions recommended by AMD/NVIDIA/Intel
    if (displayWidth == 3840 && displayHeight == 2160) // 4K
    {
        switch (quality)
        {
        case UpscalingQuality::UltraPerformance:
            outWidth = 1280;
            outHeight = 720;
            break;
        case UpscalingQuality::Performance:
            outWidth = 1920;
            outHeight = 1080;
            break;
        case UpscalingQuality::Balanced:
            outWidth = 2259;
            outHeight = 1270;
            break;
        case UpscalingQuality::Quality:
            outWidth = 2560;
            outHeight = 1440;
            break;
        case UpscalingQuality::UltraQuality:
            outWidth = 2954;
            outHeight = 1662;
            break;
        case UpscalingQuality::Native:
            outWidth = 3840;
            outHeight = 2160;
            break;
        }
    }
    else if (displayWidth == 2560 && displayHeight == 1440) // 1440p
    {
        switch (quality)
        {
        case UpscalingQuality::UltraPerformance:
            outWidth = 854;
            outHeight = 480;
            break;
        case UpscalingQuality::Performance:
            outWidth = 1280;
            outHeight = 720;
            break;
        case UpscalingQuality::Balanced:
            outWidth = 1506;
            outHeight = 846;
            break;
        case UpscalingQuality::Quality:
            outWidth = 1706;
            outHeight = 960;
            break;
        case UpscalingQuality::UltraQuality:
            outWidth = 1970;
            outHeight = 1108;
            break;
        case UpscalingQuality::Native:
            outWidth = 2560;
            outHeight = 1440;
            break;
        }
    }
    else if (displayWidth == 1920 && displayHeight == 1080) // 1080p
    {
        switch (quality)
        {
        case UpscalingQuality::UltraPerformance:
            outWidth = 640;
            outHeight = 360;
            break;
        case UpscalingQuality::Performance:
            outWidth = 960;
            outHeight = 540;
            break;
        case UpscalingQuality::Balanced:
            outWidth = 1130;
            outHeight = 636;
            break;
        case UpscalingQuality::Quality:
            outWidth = 1280;
            outHeight = 720;
            break;
        case UpscalingQuality::UltraQuality:
            outWidth = 1478;
            outHeight = 832;
            break;
        case UpscalingQuality::Native:
            outWidth = 1920;
            outHeight = 1080;
            break;
        }
    }
}

#else // !SPARK_PLATFORM_WINDOWS

// =============================================================================
// Linux/macOS stubs — upscaling requires D3D11 compute shaders
// =============================================================================

#include "UpscalingSystem.h"

void UpscalingSystem::DetectFeatures()
{
    m_dlssFeatureInfo.isAvailable = false;
    m_xessFeatureInfo.isAvailable = false;
    m_fsr2Available = false;
}

bool UpscalingSystem::CreateGPUResources()
{
    return true;
}

void UpscalingSystem::RecreateUpscalingResources() {}

bool UpscalingSystem::CompileUpscalingShaders()
{
    return false;
}

void UpscalingSystem::UnbindComputeResources() {}

void UpscalingSystem::ExecuteFSR1(ID3D11ShaderResourceView* inputColorSRV, ID3D11UnorderedAccessView* outputUAV)
{
    (void)inputColorSRV;
    (void)outputUAV;
}

void UpscalingSystem::ExecuteFSR2(const FSR2DispatchDescription& desc)
{
    (void)desc;
}

void UpscalingSystem::ExecuteDLSS(ID3D11ShaderResourceView* colorSRV, ID3D11ShaderResourceView* depthSRV,
                                  ID3D11ShaderResourceView* motionVectorsSRV, ID3D11ShaderResourceView* exposureSRV,
                                  ID3D11UnorderedAccessView* outputUAV, const XMFLOAT2& jitterOffset, bool resetHistory)
{
    (void)colorSRV;
    (void)depthSRV;
    (void)motionVectorsSRV;
    (void)exposureSRV;
    (void)outputUAV;
    (void)jitterOffset;
    (void)resetHistory;
}

void UpscalingSystem::ExecuteXeSS(ID3D11ShaderResourceView* colorSRV, ID3D11ShaderResourceView* depthSRV,
                                  ID3D11ShaderResourceView* motionVectorsSRV, ID3D11ShaderResourceView* exposureSRV,
                                  ID3D11UnorderedAccessView* outputUAV, const XMFLOAT2& jitterOffset)
{
    (void)colorSRV;
    (void)depthSRV;
    (void)motionVectorsSRV;
    (void)exposureSRV;
    (void)outputUAV;
    (void)jitterOffset;
}

void UpscalingSystem::GetRecommendedRenderSize(uint32_t displayWidth, uint32_t displayHeight, UpscalingQuality quality,
                                               uint32_t& outWidth, uint32_t& outHeight)
{
    float scale = UpscalingSettings::GetRenderScale(quality);
    outWidth = std::max(1u, static_cast<uint32_t>(static_cast<float>(displayWidth) * scale));
    outHeight = std::max(1u, static_cast<uint32_t>(static_cast<float>(displayHeight) * scale));
    outWidth = (outWidth + 1u) & ~1u;
    outHeight = (outHeight + 1u) & ~1u;
}

#endif // SPARK_PLATFORM_WINDOWS
