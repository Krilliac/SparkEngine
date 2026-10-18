/** @file GraphicsDeferredLightingWindows.cpp
 * @brief Single-sample D3D11 deferred resolve, render-thread only.
 * GPU resources share the basic shader's reload/recovery lifetime. Each draw
 * uses fixed stack constants (at most 64 directional/point/spot lights).
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "../Core/RuntimePackage.h"
#include "../Utils/LogMacros.h"
#include "DeferredLightingState.h"
#include "GraphicsEngine.h"
#include "LightingSystem.h"
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>

namespace
{
    using Microsoft::WRL::ComPtr;
    using namespace DirectX;
    constexpr GUID kResolveShader = {0x17a8cce1, 0x493b, 0x455d, {0xa1, 0x51, 0x33, 0x52, 0x88, 0x94, 0x39, 0x01}};
    constexpr GUID kResolveVertex = {0x17a8cce1, 0x493b, 0x455d, {0xa1, 0x51, 0x33, 0x52, 0x88, 0x94, 0x39, 0x02}};
    constexpr GUID kResolveBuffer = {0x17a8cce1, 0x493b, 0x455d, {0xa1, 0x51, 0x33, 0x52, 0x88, 0x94, 0x39, 0x03}};
    constexpr GUID kResolveDepth = {0x17a8cce1, 0x493b, 0x455d, {0xa1, 0x51, 0x33, 0x52, 0x88, 0x94, 0x39, 0x04}};
    constexpr GUID kResolveRaster = {0x17a8cce1, 0x493b, 0x455d, {0xa1, 0x51, 0x33, 0x52, 0x88, 0x94, 0x39, 0x05}};

    struct ResolveLight
    {
        XMFLOAT4 position, direction, color, attenuation;
    };
    struct DeferredResolveConstants
    {
        XMFLOAT4X4 inverseViewProjection;
        XMFLOAT3 cameraPosition;
        uint32_t lightCount = 0;
        XMFLOAT4 ambientColor;
        XMFLOAT4 targetSize;
        ResolveLight lights[64];
    };
    static_assert(sizeof(ResolveLight) == 64);
    static_assert(offsetof(DeferredResolveConstants, lights) == 112);
    static_assert(sizeof(DeferredResolveConstants) == 4208);

    template <typename T> ComPtr<T> GetAttached(ID3D11DeviceChild* owner, const GUID& key)
    {
        ComPtr<T> value;
        UINT size = sizeof(T*);
        if (owner)
            owner->GetPrivateData(key, &size, value.GetAddressOf());
        return value;
    }
} // namespace

HRESULT GraphicsEngine::InitializeDeferredLighting()
{
    // Publish only a complete resource set. The pixel shader owns its auxiliary
    // interfaces; the basic shader owns this pixel shader, with no reference
    // cycle.
    std::error_code error;
    const auto cwd = std::filesystem::current_path(error);
    const auto roots = Spark::RuntimePackage::ResolveContentRoots(
        L"Shaders/HLSL", Spark::RuntimePackage::GetExecutableDirectory(), error ? std::filesystem::path{} : cwd);
    ComPtr<ID3DBlob> vsBlob, psBlob;
    for (const auto& root : roots)
    {
        const auto path = root / L"DeferredLighting.hlsl";
        if (!std::filesystem::exists(path, error))
            continue;
        vsBlob.Reset();
        psBlob.Reset();
        if (SUCCEEDED(CompileShaderFromFile(path.wstring(), "VSMain", "vs_5_0", &vsBlob)) &&
            SUCCEEDED(CompileShaderFromFile(path.wstring(), "PSMain", "ps_5_0", &psBlob)))
            break;
    }
    if (!vsBlob || !psBlob)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Deferred lighting shader unavailable; using forward rendering");
        return S_FALSE;
    }
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> raster;
    HRESULT hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
    if (FAILED(hr))
        return hr;
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    if (FAILED(hr))
        return hr;
    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(DeferredResolveConstants);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, &buffer);
    if (FAILED(hr))
        return hr;
    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = FALSE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    hr = m_device->CreateDepthStencilState(&depthDesc, &depth);
    if (FAILED(hr))
        return hr;
    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    hr = m_device->CreateRasterizerState(&rasterDesc, &raster);
    if (FAILED(hr))
        return hr;
    hr = ps->SetPrivateDataInterface(kResolveVertex, vs.Get());
    if (FAILED(hr))
        return hr;
    hr = ps->SetPrivateDataInterface(kResolveBuffer, buffer.Get());
    if (FAILED(hr))
        return hr;
    hr = ps->SetPrivateDataInterface(kResolveDepth, depth.Get());
    if (FAILED(hr))
        return hr;
    hr = ps->SetPrivateDataInterface(kResolveRaster, raster.Get());
    if (FAILED(hr))
        return hr;
    return m_basicPixelShader->SetPrivateDataInterface(kResolveShader, ps.Get());
}

bool GraphicsEngine::CanResolveDeferredLighting() const
{
    // RenderGraphBased does not yet write this normal/material contract for ECS
    // geometry. Retained resources after a pipeline switch cannot qualify it.
    if (m_currentPipeline != RenderingPipeline::Deferred || !m_context || !m_renderTargetView || !m_depthStencilSRV ||
        !m_depthStencilTexture || !GetAttached<ID3D11PixelShader>(m_basicPixelShader.Get(), kResolveShader) ||
        !GetAttached<ID3D11PixelShader>(m_basicPixelShader.Get(), kDeferredGBufferShaderGuid))
        return false;
    D3D11_TEXTURE2D_DESC depthDesc{};
    m_depthStencilTexture->GetDesc(&depthDesc);
    if (depthDesc.SampleDesc.Count != 1)
        return false;
    for (UINT i = 0; i < 3; ++i)
    {
        if (!m_gBufferTextures[i] || !m_gBufferRTVs[i] || !m_gBufferSRVs[i])
            return false;
        D3D11_TEXTURE2D_DESC desc{};
        m_gBufferTextures[i]->GetDesc(&desc);
        if (desc.SampleDesc.Count != 1 || desc.Width != depthDesc.Width || desc.Height != depthDesc.Height)
            return false;
    }
    ComPtr<ID3D11Resource> output;
    ComPtr<ID3D11Texture2D> outputTexture;
    m_renderTargetView->GetResource(&output);
    if (FAILED(output.As(&outputTexture)))
        return false;
    D3D11_TEXTURE2D_DESC outputDesc{};
    outputTexture->GetDesc(&outputDesc);
    return outputDesc.SampleDesc.Count == 1 && outputDesc.Width == depthDesc.Width &&
           outputDesc.Height == depthDesc.Height;
}

bool GraphicsEngine::ResolveDeferredLighting(const XMMATRIX& view, const XMMATRIX& projection, uint32_t& resolvedLights)
{
    resolvedLights = 0;
    if (!CanResolveDeferredLighting())
        return false;
    auto ps = GetAttached<ID3D11PixelShader>(m_basicPixelShader.Get(), kResolveShader);
    auto vs = GetAttached<ID3D11VertexShader>(ps.Get(), kResolveVertex);
    auto buffer = GetAttached<ID3D11Buffer>(ps.Get(), kResolveBuffer);
    auto depth = GetAttached<ID3D11DepthStencilState>(ps.Get(), kResolveDepth);
    auto raster = GetAttached<ID3D11RasterizerState>(ps.Get(), kResolveRaster);
    if (!vs || !buffer || !depth || !raster)
        return false;

    DeferredResolveConstants constants{};
    XMVECTOR determinant;
    const auto inverse = XMMatrixInverse(&determinant, XMMatrixMultiply(view, projection));
    const float determinantValue = XMVectorGetX(determinant);
    if (!std::isfinite(determinantValue) || determinantValue == 0.0f)
        return false;
    XMStoreFloat4x4(&constants.inverseViewProjection, inverse);
    XMStoreFloat3(&constants.cameraPosition, XMMatrixInverse(nullptr, view).r[3]);
    D3D11_TEXTURE2D_DESC targetDesc{};
    m_gBufferTextures[0]->GetDesc(&targetDesc);
    constants.targetSize = XMFLOAT4(static_cast<float>(targetDesc.Width), static_cast<float>(targetDesc.Height),
                                    1.0f / targetDesc.Width, 1.0f / targetDesc.Height);
    if (m_lightingSystem)
    {
        // The existing environment upload contains ComPtrs. Pack values explicitly;
        // skyColor * skyIntensity is a uniform diffuse sky approximation, not IBL.
        const auto& environment = m_lightingSystem->GetEnvironmentLighting();
        constants.ambientColor = XMFLOAT4(environment.skyColor.x * environment.skyIntensity,
                                          environment.skyColor.y * environment.skyIntensity,
                                          environment.skyColor.z * environment.skyIntensity, 0.0f);
        for (const auto& light : m_lightingSystem->GetLights())
        {
            if (!light || !light->IsEnabled() || light->GetType() > LightType::Spot)
                continue;
            if (constants.lightCount == 64)
                break;
            const auto data = light->GetShaderData();
            constants.lights[constants.lightCount++] = {data.position, data.direction, data.color, data.attenuation};
        }
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    m_context->Unmap(buffer.Get(), 0);

    Spark::Graphics::DeferredLightingState saved(m_context.Get());
    // Remove the writable depth attachment before sampling it. All bindings and
    // viewport are explicit: a preceding shadow pass may have changed them.
    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);
    m_context->OMSetDepthStencilState(depth.Get(), 0);
    m_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    m_context->RSSetState(raster.Get());
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, constants.targetSize.x, constants.targetSize.y, 0.0f, 1.0f};
    m_context->RSSetViewports(1, &viewport);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(vs.Get(), nullptr, 0);
    m_context->PSSetShader(ps.Get(), nullptr, 0);
    m_context->GSSetShader(nullptr, nullptr, 0);
    m_context->HSSetShader(nullptr, nullptr, 0);
    m_context->DSSetShader(nullptr, nullptr, 0);
    m_context->PSSetConstantBuffers(0, 1, buffer.GetAddressOf());
    ID3D11ShaderResourceView* inputs[] = {m_gBufferSRVs[0].Get(), m_gBufferSRVs[1].Get(), m_gBufferSRVs[2].Get(),
                                          m_depthStencilSRV.Get()};
    m_context->PSSetShaderResources(0, 4, inputs);
    m_context->Draw(3, 0);
    resolvedLights = constants.lightCount;
    return true;
}
#endif
