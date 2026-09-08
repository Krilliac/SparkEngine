/**
 * @file LightingSystemWindowsShadows.cpp
 * @brief Windows/D3D11 lighting-data binding and shadow-map rendering
 */
#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "LightingSystem.h"
#include "../Utils/SparkConsole.h"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace DirectX;

void LightingSystem::BindLightingData(ID3D11DeviceContext* context)
{
    if (!context || !m_lightDataBuffer)
    {
        return;
    }

    // Update light data buffer with current light array
    if (!m_lightDataArray.empty())
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = context->Map(m_lightDataBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr) && mapped.pData)
        {
            size_t copySize =
                std::min(m_lightDataArray.size() * sizeof(LightData), static_cast<size_t>(64 * sizeof(LightData)));
            memcpy(mapped.pData, m_lightDataArray.data(), copySize);
            context->Unmap(m_lightDataBuffer.Get(), 0);
        }
    }

    // Bind constant buffers to vertex and pixel shader stages
    // Slot 1: light data, Slot 2: environment, Slot 3: shadow matrices
    ID3D11Buffer* buffers[] = {m_lightDataBuffer.Get(), m_environmentBuffer.Get(), m_shadowDataBuffer.Get()};
    context->VSSetConstantBuffers(1, 3, buffers);
    context->PSSetConstantBuffers(1, 3, buffers);

    // Bind shadow map SRVs to pixel shader (starting at texture slot 4)
    constexpr UINT shadowMapStartSlot = 4;
    std::vector<ID3D11ShaderResourceView*> shadowSRVs;
    shadowSRVs.reserve(m_shadowMaps.size());

    for (const auto& pair : m_shadowMaps)
    {
        if (pair.second && pair.second->srv)
        {
            shadowSRVs.push_back(pair.second->srv.Get());
        }
    }

    if (!shadowSRVs.empty())
    {
        context->PSSetShaderResources(shadowMapStartSlot, static_cast<UINT>(shadowSRVs.size()), shadowSRVs.data());
    }

    // Bind CSM shadow map SRVs if available
    if (m_csmShadowMap)
    {
        UINT csmStartSlot = shadowMapStartSlot + static_cast<UINT>(shadowSRVs.size());
        std::vector<ID3D11ShaderResourceView*> csmSRVs;
        csmSRVs.reserve(m_csmShadowMap->cascades.size());

        for (const auto& cascade : m_csmShadowMap->cascades)
        {
            if (cascade.srv)
            {
                csmSRVs.push_back(cascade.srv.Get());
            }
        }

        if (!csmSRVs.empty())
        {
            context->PSSetShaderResources(csmStartSlot, static_cast<UINT>(csmSRVs.size()), csmSRVs.data());
        }
    }

    // Bind IBL textures to pixel shader (slots 8-11)
    constexpr UINT iblStartSlot = 8;
    ID3D11ShaderResourceView* iblSRVs[4] = {
        m_environmentLighting.irradianceMap.Get(), m_environmentLighting.prefilterMap.Get(),
        m_environmentLighting.brdfLUT.Get(), m_environmentLighting.environmentMap.Get()};
    context->PSSetShaderResources(iblStartSlot, 4, iblSRVs);
}

void LightingSystem::RenderShadowMaps(std::function<void(const XMMATRIX&, const XMMATRIX&)> renderCallback)
{
    if (!renderCallback || !m_shadowsEnabled || !m_context)
    {
        return;
    }

    m_metrics.shadowMapUpdates = 0;

    // Save the current viewport to restore after shadow rendering
    UINT numViewports = 1;
    D3D11_VIEWPORT originalViewport;
    m_context->RSGetViewports(&numViewports, &originalViewport);

    // Save the current render targets
    ComPtr<ID3D11RenderTargetView> originalRTV;
    ComPtr<ID3D11DepthStencilView> originalDSV;
    m_context->OMGetRenderTargets(1, &originalRTV, &originalDSV);

    // Render standard shadow maps for each shadow-casting light. `lightIndex`
    // is the same identity LightingSystem::Update hands to the cached shadow
    // atlas, so MarkRendered below closes the request/render loop.
    uint32_t lightIndex = 0;
    for (const auto& light : m_lights)
    {
        const uint32_t currentLightIndex = lightIndex++;

        if (!light || !light->IsEnabled() || !light->GetCastShadows())
        {
            continue;
        }

        auto it = m_shadowMaps.find(light.get());
        if (it == m_shadowMaps.end() || !it->second || !it->second->dsv)
        {
            continue;
        }

        try
        {
            const ShadowMap& shadowMap = *it->second;

            // Set shadow map viewport
            D3D11_VIEWPORT shadowViewport = {};
            shadowViewport.TopLeftX = 0.0f;
            shadowViewport.TopLeftY = 0.0f;
            shadowViewport.Width = static_cast<float>(shadowMap.size);
            shadowViewport.Height = static_cast<float>(shadowMap.size);
            shadowViewport.MinDepth = 0.0f;
            shadowViewport.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &shadowViewport);

            // Unbind any render targets; only depth writing
            ID3D11RenderTargetView* nullRTV = nullptr;
            m_context->OMSetRenderTargets(1, &nullRTV, shadowMap.dsv.Get());

            // Clear shadow map depth buffer
            m_context->ClearDepthStencilView(shadowMap.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

            // For directional lights with CSM, render each cascade
            if (light->GetType() == LightType::Directional && light->GetShadowTechnique() == ShadowTechnique::CSM &&
                m_csmShadowMap)
            {
                for (uint32_t cascade = 0; cascade < m_csmShadowMap->cascadeCount; ++cascade)
                {
                    if (cascade >= m_csmShadowMap->cascades.size())
                    {
                        break;
                    }

                    const ShadowMap& csmCascade = m_csmShadowMap->cascades[cascade];
                    if (!csmCascade.dsv)
                    {
                        continue;
                    }

                    // Set cascade viewport
                    D3D11_VIEWPORT cascadeViewport = {};
                    cascadeViewport.TopLeftX = 0.0f;
                    cascadeViewport.TopLeftY = 0.0f;
                    cascadeViewport.Width = static_cast<float>(csmCascade.size);
                    cascadeViewport.Height = static_cast<float>(csmCascade.size);
                    cascadeViewport.MinDepth = 0.0f;
                    cascadeViewport.MaxDepth = 1.0f;
                    m_context->RSSetViewports(1, &cascadeViewport);

                    m_context->OMSetRenderTargets(1, &nullRTV, csmCascade.dsv.Get());
                    m_context->ClearDepthStencilView(csmCascade.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

                    // Use the precomputed cascade light matrix
                    XMMATRIX cascadeMatrix = (cascade < m_csmShadowMap->lightMatrices.size())
                                                 ? m_csmShadowMap->lightMatrices[cascade]
                                                 : csmCascade.lightMatrix;
                    XMMATRIX cascadeProj = XMMatrixIdentity(); // Already baked into cascadeMatrix

                    renderCallback(cascadeMatrix, cascadeProj);
                    m_metrics.shadowMapUpdates++;
                }
            }
            else
            {
                // Standard shadow map: use the stored light/shadow matrices. UpdateShadowMaps
                // guarantees lightMatrix * shadowMatrix is the light view-projection, so a
                // directional light (whose lightMatrix is already combined) has an identity
                // projection here and the depth pass never applies a second projection.
                renderCallback(shadowMap.lightMatrix, shadowMap.shadowMatrix);
                m_metrics.shadowMapUpdates++;
            }

            m_shadowCache.MarkRendered(currentLightIndex);
        }
        catch (...)
        {
            Spark::SimpleConsole::GetInstance().LogWarning("Error in shadow map render callback for light");
        }
    }

    // Restore original render targets and viewport
    ID3D11RenderTargetView* rtvRestore = originalRTV.Get();
    m_context->OMSetRenderTargets(1, &rtvRestore, originalDSV.Get());
    m_context->RSSetViewports(1, &originalViewport);
}

#endif // SPARK_PLATFORM_WINDOWS
