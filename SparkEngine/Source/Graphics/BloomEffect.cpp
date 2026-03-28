/**
 * @file BloomEffect.cpp
 * @brief HDR bloom implementation with progressive downsample/upsample
 */

#include "BloomEffect.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include <algorithm>
#include <cmath>

namespace Spark::Graphics
{

    // =========================================================================
    // Fullscreen vertex shader (shared with PostProcessingPipeline)
    // =========================================================================

    static const char* kBloomFullscreenVS = R"(
struct VSOutput {
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};
VSOutput main(uint vertexId : SV_VertexID) {
    VSOutput o;
    o.texCoord = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(o.texCoord * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

    // =========================================================================
    // Prefilter: extract bright pixels with soft knee
    // =========================================================================

    static const char* kPrefilterPS = R"(
cbuffer BloomCB : register(b0) {
    float4 thresholdParams; // x=threshold, y=threshold-knee, z=2*knee, w=0.25/knee
    float4 texelSize;       // x=1/w, y=1/h, z=scatter, w=intensity
    float4 params;          // x=clampMax
};
Texture2D<float4> inputTex : register(t0);
SamplerState linearSampler : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 color = inputTex.Sample(linearSampler, uv);

    // Clamp to prevent fireflies
    color = min(color, params.x);

    // Luminance (Rec. 709)
    float luma = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));

    // Soft knee threshold curve
    float knee = thresholdParams.y;
    float soft = luma - thresholdParams.x + knee;
    soft = clamp(soft, 0.0, thresholdParams.z);
    soft = soft * soft * thresholdParams.w;

    float contribution = max(soft, luma - thresholdParams.x);
    contribution /= max(luma, 0.00001);

    return float4(color.rgb * contribution, 1.0);
}
)";

    // =========================================================================
    // Downsample: 13-tap bilinear filter (reduces aliasing and fireflies)
    // =========================================================================

    static const char* kDownsamplePS = R"(
cbuffer BloomCB : register(b0) {
    float4 thresholdParams;
    float4 texelSize;
    float4 params;
};
Texture2D<float4> inputTex : register(t0);
SamplerState linearSampler : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 ts = texelSize.xy;

    // 13-tap downsample (Call of Duty: Advanced Warfare presentation)
    // Center cross (4 taps)
    float4 a = inputTex.Sample(linearSampler, uv + float2(-ts.x, -ts.y));
    float4 b = inputTex.Sample(linearSampler, uv + float2( 0.0,  -ts.y));
    float4 c = inputTex.Sample(linearSampler, uv + float2( ts.x, -ts.y));
    float4 d = inputTex.Sample(linearSampler, uv + float2(-ts.x,  0.0));
    float4 e = inputTex.Sample(linearSampler, uv);
    float4 f = inputTex.Sample(linearSampler, uv + float2( ts.x,  0.0));
    float4 g = inputTex.Sample(linearSampler, uv + float2(-ts.x,  ts.y));
    float4 h = inputTex.Sample(linearSampler, uv + float2( 0.0,   ts.y));
    float4 i = inputTex.Sample(linearSampler, uv + float2( ts.x,  ts.y));

    // Corner diagonals (4 taps at half-texel offsets)
    float4 j = inputTex.Sample(linearSampler, uv + float2(-ts.x, -ts.y) * 2.0);
    float4 k = inputTex.Sample(linearSampler, uv + float2( ts.x, -ts.y) * 2.0);
    float4 l = inputTex.Sample(linearSampler, uv + float2(-ts.x,  ts.y) * 2.0);
    float4 m = inputTex.Sample(linearSampler, uv + float2( ts.x,  ts.y) * 2.0);

    // Weighted combination: center 4 get 0.5 weight, edges get 0.125
    float4 result = e * 0.125;
    result += (a + c + g + i) * 0.03125;
    result += (b + d + f + h) * 0.0625;
    result += (j + k + l + m) * 0.03125;

    return result;
}
)";

    // =========================================================================
    // Upsample: 9-tap tent filter with scatter blend
    // =========================================================================

    static const char* kUpsamplePS = R"(
cbuffer BloomCB : register(b0) {
    float4 thresholdParams;
    float4 texelSize;  // z = scatter
    float4 params;
};
Texture2D<float4> inputTex : register(t0);   // Lower-res mip (to upsample)
Texture2D<float4> higherTex : register(t1);  // Higher-res mip (to blend with)
SamplerState linearSampler : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 ts = texelSize.xy;

    // 9-tap tent filter for smooth upsampling
    float4 result = float4(0, 0, 0, 0);
    result += inputTex.Sample(linearSampler, uv + float2(-ts.x, -ts.y));
    result += inputTex.Sample(linearSampler, uv + float2( 0.0,  -ts.y)) * 2.0;
    result += inputTex.Sample(linearSampler, uv + float2( ts.x, -ts.y));
    result += inputTex.Sample(linearSampler, uv + float2(-ts.x,  0.0))  * 2.0;
    result += inputTex.Sample(linearSampler, uv)                         * 4.0;
    result += inputTex.Sample(linearSampler, uv + float2( ts.x,  0.0))  * 2.0;
    result += inputTex.Sample(linearSampler, uv + float2(-ts.x,  ts.y));
    result += inputTex.Sample(linearSampler, uv + float2( 0.0,   ts.y)) * 2.0;
    result += inputTex.Sample(linearSampler, uv + float2( ts.x,  ts.y));
    result *= (1.0 / 16.0);

    // Blend with higher-resolution mip using scatter factor
    float4 higher = higherTex.Sample(linearSampler, uv);
    return lerp(higher, result, texelSize.z);
}
)";

    // =========================================================================
    // Implementation
    // =========================================================================

    bool BloomEffect::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width, uint32_t height)
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

    void BloomEffect::Shutdown()
    {
        for (auto& mip : m_mipChain)
            mip = {};
        for (auto& mip : m_upsampleChain)
            mip = {};
        m_prefilterTarget = {};
        m_initialized = false;
    }

    void BloomEffect::Resize(uint32_t width, uint32_t height)
    {
        if (width == m_width && height == m_height)
            return;
        m_width = width;
        m_height = height;
        CreateResources();
    }

    bool BloomEffect::CompileShaders()
    {
        UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

        // Vertex shader
        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompile(kBloomFullscreenVS, strlen(kBloomFullscreenVS), "BloomVS", nullptr, nullptr, "main",
                                "vs_5_0", flags, 0, &vsBlob, &errorBlob);
        if (FAILED(hr))
            return false;
        hr =
            m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_fullscreenVS);
        if (FAILED(hr))
            return false;

        // Prefilter PS
        auto compilePS = [&](const char* src, const char* name, ComPtr<ID3D11PixelShader>& outPS) -> bool
        {
            ComPtr<ID3DBlob> blob;
            ComPtr<ID3DBlob> err;
            hr = D3DCompile(src, strlen(src), name, nullptr, nullptr, "main", "ps_5_0", flags, 0, &blob, &err);
            if (FAILED(hr))
                return false;
            hr = m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &outPS);
            return SUCCEEDED(hr);
        };

        if (!compilePS(kPrefilterPS, "BloomPrefilter", m_prefilterPS))
            return false;
        if (!compilePS(kDownsamplePS, "BloomDownsample", m_downsamplePS))
            return false;
        if (!compilePS(kUpsamplePS, "BloomUpsample", m_upsamplePS))
            return false;

        return true;
    }

    bool BloomEffect::CreateResources()
    {
        // Calculate mip chain dimensions
        m_mipCount = std::min(m_settings.maxIterations, kMaxMips);
        uint32_t w = m_width / 2;
        uint32_t h = m_height / 2;

        // Ensure we don't go below 1x1
        int actualMips = 0;
        for (int i = 0; i < m_mipCount; ++i)
        {
            if (w < 1 || h < 1)
                break;
            actualMips = i + 1;
            w /= 2;
            h /= 2;
        }
        m_mipCount = actualMips;

        // Create mip chain textures
        auto createTarget = [&](MipLevel& mip, uint32_t tw, uint32_t th) -> bool
        {
            mip = {};
            mip.width = tw;
            mip.height = th;

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = tw;
            desc.Height = th;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &mip.texture);
            if (FAILED(hr))
                return false;

            ComPtr<ID3D11RenderTargetView> rtv;
            hr = m_device->CreateRenderTargetView(mip.texture.Get(), nullptr, &rtv);
            if (FAILED(hr))
                return false;
            mip.rtv = rtv;

            ComPtr<ID3D11ShaderResourceView> srv;
            hr = m_device->CreateShaderResourceView(mip.texture.Get(), nullptr, &srv);
            if (FAILED(hr))
                return false;
            mip.srv = srv;

            return true;
        };

        // Prefilter at half resolution
        if (!createTarget(m_prefilterTarget, m_width / 2, m_height / 2))
            return false;

        // Downsample chain
        w = m_width / 2;
        h = m_height / 2;
        for (int i = 0; i < m_mipCount; ++i)
        {
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
            if (!createTarget(m_mipChain[i], w, h))
                return false;
            if (!createTarget(m_upsampleChain[i], w, h))
                return false;
        }

        // Constant buffer
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(BloomCB);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer)))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "BloomEffect: Failed to create constant buffer");
            return false;
        }

        // Linear clamp sampler
        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(m_device->CreateSamplerState(&sampDesc, &m_linearClampSampler)))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "BloomEffect: Failed to create sampler state");
            return false;
        }

        return true;
    }

    void BloomEffect::PrefilterPass(ID3D11ShaderResourceView* input)
    {
        float knee = m_settings.threshold * m_settings.softKnee;

        BloomCB cb = {};
        cb.thresholdParams = {m_settings.threshold, knee, 2.0f * knee, knee > 0.0f ? 0.25f / knee : 0.0f};
        cb.texelSize = {(m_prefilterTarget.width > 0) ? (1.0f / m_prefilterTarget.width) : 0.0f,
                        (m_prefilterTarget.height > 0) ? (1.0f / m_prefilterTarget.height) : 0.0f, m_settings.scatter,
                        m_settings.intensity};
        cb.params = {m_settings.clampMax, 0.0f, m_settings.anamorphicRatio, 0.0f};

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
            return;
        memcpy(mapped.pData, &cb, sizeof(cb));
        m_context->Unmap(m_constantBuffer.Get(), 0);

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(m_prefilterTarget.width);
        vp.Height = static_cast<float>(m_prefilterTarget.height);
        vp.MaxDepth = 1.0f;

        ID3D11RenderTargetView* rtv = m_prefilterTarget.rtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
        m_context->RSSetViewports(1, &vp);
        m_context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
        m_context->PSSetShader(m_prefilterPS.Get(), nullptr, 0);
        m_context->PSSetShaderResources(0, 1, &input);
        ID3D11Buffer* cbs[] = {m_constantBuffer.Get()};
        m_context->PSSetConstantBuffers(0, 1, cbs);
        ID3D11SamplerState* samplers[] = {m_linearClampSampler.Get()};
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->Draw(3, 0);
    }

    void BloomEffect::DownsamplePass(int srcMip)
    {
        auto& src = (srcMip == 0) ? m_prefilterTarget : m_mipChain[srcMip - 1];
        auto& dst = m_mipChain[srcMip];

        BloomCB cb = {};
        cb.texelSize = {1.0f / src.width, 1.0f / src.height, m_settings.scatter, m_settings.intensity};
        cb.params = {m_settings.clampMax, static_cast<float>(srcMip), 0.0f, 0.0f};

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
            return;
        memcpy(mapped.pData, &cb, sizeof(cb));
        m_context->Unmap(m_constantBuffer.Get(), 0);

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(dst.width);
        vp.Height = static_cast<float>(dst.height);
        vp.MaxDepth = 1.0f;

        ID3D11RenderTargetView* rtv = dst.rtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
        m_context->RSSetViewports(1, &vp);
        m_context->PSSetShader(m_downsamplePS.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv = src.srv.Get();
        m_context->PSSetShaderResources(0, 1, &srv);
        m_context->Draw(3, 0);
    }

    void BloomEffect::UpsamplePass(int dstMip)
    {
        auto& src = (dstMip + 1 < m_mipCount) ? m_upsampleChain[dstMip + 1] : m_mipChain[m_mipCount - 1];
        auto& higher = m_mipChain[dstMip];
        auto& dst = m_upsampleChain[dstMip];

        BloomCB cb = {};
        cb.texelSize = {1.0f / src.width, 1.0f / src.height, m_settings.scatter, m_settings.intensity};

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
            return;
        memcpy(mapped.pData, &cb, sizeof(cb));
        m_context->Unmap(m_constantBuffer.Get(), 0);

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(dst.width);
        vp.Height = static_cast<float>(dst.height);
        vp.MaxDepth = 1.0f;

        ID3D11RenderTargetView* rtv = dst.rtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
        m_context->RSSetViewports(1, &vp);
        m_context->PSSetShader(m_upsamplePS.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[2] = {src.srv.Get(), higher.srv.Get()};
        m_context->PSSetShaderResources(0, 2, srvs);
        m_context->Draw(3, 0);
    }

    ID3D11ShaderResourceView* BloomEffect::Execute(ID3D11ShaderResourceView* inputSRV)
    {
        if (!m_initialized || !m_settings.enabled || m_mipCount < 1)
            return nullptr;

        // 1. Prefilter bright pixels
        PrefilterPass(inputSRV);

        // Unbind SRVs between passes
        ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};

        // 2. Progressive downsample
        for (int i = 0; i < m_mipCount; ++i)
        {
            m_context->PSSetShaderResources(0, 2, nullSRVs);
            DownsamplePass(i);
        }

        // 3. Progressive upsample (bottom to top)
        for (int i = m_mipCount - 2; i >= 0; --i)
        {
            m_context->PSSetShaderResources(0, 2, nullSRVs);
            UpsamplePass(i);
        }

        // Return the finest upsample level as the bloom texture
        return m_upsampleChain[0].srv.Get();
    }

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
