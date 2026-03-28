/**
 * @file TonemapColorGrading.cpp
 * @brief Auto-exposure, tonemapping, and color grading implementation
 */

#include "TonemapColorGrading.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Spark::Graphics
{

    // =========================================================================
    // Shader source
    // =========================================================================

    static const char* kFullscreenVS = R"(
struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOutput main(uint id : SV_VertexID) {
    VSOutput o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}
)";

    // Log-luminance extraction
    static const char* kLuminancePS = R"(
Texture2D<float4> inputTex : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 color = inputTex.Sample(samp, uv).rgb;
    float lum = dot(color, float3(0.2126, 0.7152, 0.0722));
    float logLum = log2(max(lum, 0.00001));
    return float4(logLum, logLum, logLum, 1.0);
}
)";

    // Average downsample (2x2 tap)
    static const char* kLumDownsamplePS = R"(
cbuffer CB : register(b0) {
    float4 exposureParams;
    float4 colorLift;
    float4 colorGamma;
    float4 colorGain;
    float4 whiteBalance;
    float4 texelSize;
};
Texture2D<float4> inputTex : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 ts = texelSize.xy;
    float a = inputTex.Sample(samp, uv + float2(-ts.x, -ts.y)).r;
    float b = inputTex.Sample(samp, uv + float2( ts.x, -ts.y)).r;
    float c = inputTex.Sample(samp, uv + float2(-ts.x,  ts.y)).r;
    float d = inputTex.Sample(samp, uv + float2( ts.x,  ts.y)).r;
    return float4((a+b+c+d) * 0.25, 0, 0, 1);
}
)";

    // Temporal exposure adaptation
    static const char* kAdaptExposurePS = R"(
cbuffer CB : register(b0) {
    float4 exposureParams; // x=deltaTime, y=speedUp, z=speedDown, w=unused
    float4 colorLift;      // x=minEV, y=maxEV, z=compensation, w=targetMidGray
};
Texture2D<float4> currentLum : register(t0);  // Current frame average log-luminance (1x1)
Texture2D<float4> prevAdapted : register(t1);  // Previous adapted luminance
SamplerState samp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float avgLogLum = currentLum.Sample(samp, float2(0.5, 0.5)).r;
    float avgLum = exp2(avgLogLum);

    // Convert luminance to EV100
    float ev100 = log2(avgLum * 100.0 / 12.5);
    ev100 = clamp(ev100, colorLift.x, colorLift.y);
    ev100 += colorLift.z; // compensation

    // Target exposure from EV100
    float targetExposure = 1.0 / (1.2 * exp2(ev100));

    // Temporal adaptation
    float prevExposure = prevAdapted.Sample(samp, float2(0.5, 0.5)).r;
    if (prevExposure <= 0.0) prevExposure = targetExposure;

    float speed = (targetExposure > prevExposure) ? exposureParams.y : exposureParams.z;
    float adapted = prevExposure + (targetExposure - prevExposure) * (1.0 - exp(-exposureParams.x * speed));

    return float4(adapted, adapted, adapted, 1.0);
}
)";

    // Combined tonemap + color grading
    static const char* kTonemapPS = R"(
cbuffer CB : register(b0) {
    float4 exposureParams;  // x=exposure, y=tonemapOp, z=bloomIntensity, w=unused
    float4 colorLift;       // rgb=lift, w=saturation
    float4 colorGamma;      // rgb=1/gamma, w=contrast
    float4 colorGain;       // rgb=gain, w=unused
    float4 whiteBalance;    // x=temperature, y=tint
    float4 texelSize;
};
Texture2D<float4> sceneTex : register(t0);
Texture2D<float4> bloomTex : register(t1);
Texture2D<float4> exposureTex : register(t2);  // Adapted exposure (1x1)
SamplerState linearSamp : register(s0);
SamplerState pointSamp : register(s1);

// --- Tonemap operators ---

float3 ReinhardTonemap(float3 x) {
    return x / (1.0 + x);
}

// ACES fitted (Stephen Hill)
float3 ACESFitted(float3 x) {
    float3 a = x * (x * 2.51 + 0.03);
    float3 b = x * (x * 2.43 + 0.59) + 0.14;
    return saturate(a / b);
}

// Uncharted 2 / Hable filmic
float3 Uncharted2Tonemap(float3 x) {
    float A = 0.15; float B = 0.50; float C = 0.10;
    float D = 0.20; float E = 0.02; float F = 0.30;
    return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}

float3 FilmicTonemap(float3 color) {
    float3 curr = Uncharted2Tonemap(color * 2.0);
    float3 whiteScale = 1.0 / Uncharted2Tonemap(float3(11.2, 11.2, 11.2));
    return curr * whiteScale;
}

// Khronos PBR Neutral
float3 NeutralTonemap(float3 color) {
    float startCompression = 0.8 - 0.04;
    float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, float3(newPeak, newPeak, newPeak), g);
}

// --- White balance ---
float3 ApplyWhiteBalance(float3 color, float temp, float tnt) {
    // Simplified: warm shifts toward orange, cool toward blue
    float3 warmShift = float3(1.0 + temp * 0.1, 1.0, 1.0 - temp * 0.1);
    float3 tintShift = float3(1.0, 1.0 - tnt * 0.05, 1.0 + tnt * 0.05);
    return color * warmShift * tintShift;
}

// --- Lift/Gamma/Gain ---
float3 ApplyLGG(float3 color, float3 lift, float3 invGamma, float3 gain) {
    // Lift: color = color * (1 - lift) + lift  (affects shadows)
    color = color * (1.0 - lift) + lift;
    // Gain: multiply
    color *= gain;
    // Gamma: power curve
    color = pow(max(color, 0.0001), invGamma);
    return color;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 color = sceneTex.Sample(linearSamp, uv).rgb;

    // Add bloom
    if (exposureParams.z > 0.0) {
        float3 bloom = bloomTex.Sample(linearSamp, uv).rgb;
        color += bloom * exposureParams.z;
    }

    // Apply exposure
    float exposure = exposureParams.x;
    float adaptedExposure = exposureTex.Sample(pointSamp, float2(0.5, 0.5)).r;
    if (adaptedExposure > 0.0) exposure *= adaptedExposure;
    color *= exposure;

    // White balance (pre-tonemap in linear space)
    if (whiteBalance.x != 0.0 || whiteBalance.y != 0.0) {
        color = ApplyWhiteBalance(color, whiteBalance.x, whiteBalance.y);
    }

    // Tonemapping
    int op = (int)exposureParams.y;
    if (op == 1) color = ReinhardTonemap(color);
    else if (op == 2) color = ACESFitted(color);
    else if (op == 3) color = FilmicTonemap(color);
    else if (op == 4) color = NeutralTonemap(color);

    // Lift/Gamma/Gain (post-tonemap in roughly LDR range)
    color = ApplyLGG(color, colorLift.rgb, colorGamma.rgb, colorGain.rgb);

    // Saturation
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = lerp(float3(luma, luma, luma), color, colorLift.w);

    // Contrast (around mid-gray)
    color = (color - 0.5) * colorGamma.w + 0.5;

    // Final clamp to [0,1]
    color = saturate(color);

    return float4(color, 1.0);
}
)";

    // =========================================================================
    // Implementation
    // =========================================================================

    bool TonemapColorGrading::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width,
                                         uint32_t height)
    {
        m_device = device;
        m_context = context;
        m_width = width;
        m_height = height;

        if (!CompileShaders())
            return false;
        if (!CreateResources())
            return false;

        m_initialized = true;
        return true;
    }

    void TonemapColorGrading::Shutdown()
    {
        m_initialized = false;
    }

    void TonemapColorGrading::Resize(uint32_t width, uint32_t height)
    {
        if (width == m_width && height == m_height)
            return;
        m_width = width;
        m_height = height;
        CreateResources();
    }

    bool TonemapColorGrading::CompileShaders()
    {
        UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

        auto compile = [&](const char* src, const char* name, const char* target, auto& outShader) -> bool
        {
            ComPtr<ID3DBlob> blob;
            ComPtr<ID3DBlob> err;
            HRESULT hr = D3DCompile(src, strlen(src), name, nullptr, nullptr, "main", target, flags, 0, &blob, &err);
            if (FAILED(hr))
                return false;

            if constexpr (std::is_same_v<std::remove_reference_t<decltype(outShader)>, ComPtr<ID3D11VertexShader>>)
                return SUCCEEDED(
                    m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &outShader));
            else
                return SUCCEEDED(
                    m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &outShader));
        };

        if (!compile(kFullscreenVS, "TonemapVS", "vs_5_0", m_fullscreenVS))
            return false;
        if (!compile(kLuminancePS, "LuminancePS", "ps_5_0", m_luminancePS))
            return false;
        if (!compile(kLumDownsamplePS, "LumDownPS", "ps_5_0", m_lumDownsamplePS))
            return false;
        if (!compile(kAdaptExposurePS, "AdaptPS", "ps_5_0", m_adaptExposurePS))
            return false;
        if (!compile(kTonemapPS, "TonemapPS", "ps_5_0", m_tonemapPS))
            return false;

        return true;
    }

    bool TonemapColorGrading::CreateResources()
    {
        // Luminance reduction chain: 128x128 → 1x1
        uint32_t lumSize = 128;
        for (int i = 0; i < kLumMips; ++i)
        {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = lumSize;
            desc.Height = lumSize;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            m_device->CreateTexture2D(&desc, nullptr, &m_lumTextures[i]);
            m_device->CreateRenderTargetView(m_lumTextures[i].Get(), nullptr, &m_lumRTVs[i]);
            m_device->CreateShaderResourceView(m_lumTextures[i].Get(), nullptr, &m_lumSRVs[i]);

            lumSize = std::max(lumSize / 2, 1u);
        }

        // Adapted luminance (double-buffered 1x1 R16F)
        for (int i = 0; i < 2; ++i)
        {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = 1;
            desc.Height = 1;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            m_device->CreateTexture2D(&desc, nullptr, &m_adaptedLum[i]);
            m_device->CreateRenderTargetView(m_adaptedLum[i].Get(), nullptr, &m_adaptedLumRTV[i]);
            m_device->CreateShaderResourceView(m_adaptedLum[i].Get(), nullptr, &m_adaptedLumSRV[i]);
        }

        // Constant buffer
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(TonemapCB);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);

        // Samplers
        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        m_device->CreateSamplerState(&sampDesc, &m_linearClampSampler);

        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        m_device->CreateSamplerState(&sampDesc, &m_pointSampler);

        return true;
    }

    void TonemapColorGrading::ComputeAverageLuminance(ID3D11ShaderResourceView* inputSRV)
    {
        ID3D11ShaderResourceView* nullSRV = nullptr;

        // Step 1: Extract log-luminance at 128x128
        {
            D3D11_VIEWPORT vp = {};
            vp.Width = 128.0f;
            vp.Height = 128.0f;
            vp.MaxDepth = 1.0f;

            ID3D11RenderTargetView* rtv = m_lumRTVs[0].Get();
            m_context->OMSetRenderTargets(1, &rtv, nullptr);
            m_context->RSSetViewports(1, &vp);
            m_context->PSSetShader(m_luminancePS.Get(), nullptr, 0);
            m_context->PSSetShaderResources(0, 1, &inputSRV);
            m_context->Draw(3, 0);
        }

        // Step 2: Progressive downsample to 1x1
        uint32_t sz = 128;
        for (int i = 1; i < kLumMips; ++i)
        {
            m_context->PSSetShaderResources(0, 1, &nullSRV);

            sz = std::max(sz / 2, 1u);
            D3D11_VIEWPORT vp = {};
            vp.Width = static_cast<float>(sz);
            vp.Height = static_cast<float>(sz);
            vp.MaxDepth = 1.0f;

            // Update texel size in constant buffer
            TonemapCB cb = {};
            uint32_t prevSz = sz * 2;
            cb.texelSize = {1.0f / prevSz, 1.0f / prevSz, 0.0f, 0.0f};

            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
                continue;
            memcpy(mapped.pData, &cb, sizeof(cb));
            m_context->Unmap(m_constantBuffer.Get(), 0);

            ID3D11RenderTargetView* rtv = m_lumRTVs[i].Get();
            m_context->OMSetRenderTargets(1, &rtv, nullptr);
            m_context->RSSetViewports(1, &vp);
            m_context->PSSetShader(m_lumDownsamplePS.Get(), nullptr, 0);

            ID3D11ShaderResourceView* srv = m_lumSRVs[i - 1].Get();
            m_context->PSSetShaderResources(0, 1, &srv);

            ID3D11Buffer* cbs[] = {m_constantBuffer.Get()};
            m_context->PSSetConstantBuffers(0, 1, cbs);

            m_context->Draw(3, 0);
        }
    }

    void TonemapColorGrading::AdaptExposure(float deltaTime)
    {
        int prevBuffer = m_currentLumBuffer;
        int currBuffer = 1 - m_currentLumBuffer;

        TonemapCB cb = {};
        cb.exposureParams = {deltaTime, m_autoExposureSettings.adaptationSpeedUp,
                             m_autoExposureSettings.adaptationSpeedDown, 0.0f};
        cb.colorLift = {m_autoExposureSettings.minEV, m_autoExposureSettings.maxEV,
                        m_autoExposureSettings.compensationEV, m_autoExposureSettings.targetMidGray};

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
            return;
        memcpy(mapped.pData, &cb, sizeof(cb));
        m_context->Unmap(m_constantBuffer.Get(), 0);

        ID3D11ShaderResourceView* nullSRV = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSRV);
        m_context->PSSetShaderResources(1, 1, &nullSRV);

        D3D11_VIEWPORT vp = {};
        vp.Width = 1.0f;
        vp.Height = 1.0f;
        vp.MaxDepth = 1.0f;

        ID3D11RenderTargetView* rtv = m_adaptedLumRTV[currBuffer].Get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
        m_context->RSSetViewports(1, &vp);
        m_context->PSSetShader(m_adaptExposurePS.Get(), nullptr, 0);

        // Bind the 1x1 average luminance and previous adapted exposure
        ID3D11ShaderResourceView* srvs[2] = {m_lumSRVs[kLumMips - 1].Get(), m_adaptedLumSRV[prevBuffer].Get()};
        m_context->PSSetShaderResources(0, 2, srvs);

        ID3D11Buffer* cbs[] = {m_constantBuffer.Get()};
        m_context->PSSetConstantBuffers(0, 1, cbs);
        m_context->Draw(3, 0);

        m_currentLumBuffer = currBuffer;
    }

    void TonemapColorGrading::Execute(ID3D11ShaderResourceView* inputSRV, ID3D11ShaderResourceView* bloomSRV,
                                      ID3D11RenderTargetView* outputRTV, float deltaTime)
    {
        if (!m_initialized)
            return;

        m_context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ID3D11SamplerState* samplers[2] = {m_linearClampSampler.Get(), m_pointSampler.Get()};
        m_context->PSSetSamplers(0, 2, samplers);

        // Auto-exposure: compute average luminance and adapt
        if (m_autoExposureSettings.enabled && m_autoExposureSettings.mode == ExposureMode::Automatic)
        {
            ComputeAverageLuminance(inputSRV);
            AdaptExposure(deltaTime);
        }

        // Final tonemap + color grading pass
        {
            ID3D11ShaderResourceView* nullSRVs[3] = {nullptr, nullptr, nullptr};
            m_context->PSSetShaderResources(0, 3, nullSRVs);

            const auto& cg = m_colorGradingSettings;
            TonemapCB cb = {};
            cb.exposureParams = {m_tonemapSettings.exposure, static_cast<float>(static_cast<int>(m_tonemapSettings.op)),
                                 bloomSRV ? 1.0f : 0.0f, 0.0f};
            cb.colorLift = {cg.lift.x, cg.lift.y, cg.lift.z, cg.saturation};
            cb.colorGamma = {cg.gamma.x > 0.01f ? 1.0f / cg.gamma.x : 1.0f,
                             cg.gamma.y > 0.01f ? 1.0f / cg.gamma.y : 1.0f,
                             cg.gamma.z > 0.01f ? 1.0f / cg.gamma.z : 1.0f, cg.contrast};
            cb.colorGain = {cg.gain.x, cg.gain.y, cg.gain.z, 0.0f};
            cb.whiteBalance = {cg.temperature, cg.tint, 0.0f, 0.0f};
            cb.texelSize = {(m_width > 0) ? (1.0f / m_width) : 0.0f, (m_height > 0) ? (1.0f / m_height) : 0.0f, 0.0f,
                            0.0f};

            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
                return;
            memcpy(mapped.pData, &cb, sizeof(cb));
            m_context->Unmap(m_constantBuffer.Get(), 0);

            D3D11_VIEWPORT vp = {};
            vp.Width = static_cast<float>(m_width);
            vp.Height = static_cast<float>(m_height);
            vp.MaxDepth = 1.0f;

            m_context->OMSetRenderTargets(1, &outputRTV, nullptr);
            m_context->RSSetViewports(1, &vp);
            m_context->PSSetShader(m_tonemapPS.Get(), nullptr, 0);

            // Bind scene, bloom, and adapted exposure
            ID3D11ShaderResourceView* srvs[3] = {
                inputSRV, bloomSRV,
                m_autoExposureSettings.enabled ? m_adaptedLumSRV[m_currentLumBuffer].Get() : nullptr};
            m_context->PSSetShaderResources(0, 3, srvs);

            ID3D11Buffer* cbs[] = {m_constantBuffer.Get()};
            m_context->PSSetConstantBuffers(0, 1, cbs);
            m_context->Draw(3, 0);
        }
    }

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
