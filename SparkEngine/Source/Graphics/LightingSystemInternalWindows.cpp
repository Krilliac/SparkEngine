/**
 * @file LightingSystemInternalWindows.cpp
 * @brief Windows/D3D11 implementation — split from LightingSystemInternal.cpp
 */
#include "Core/Platform.h"
#include "Utils/MathUtils.h"
#ifdef SPARK_PLATFORM_WINDOWS

/**
 * @file LightingSystemInternal.cpp
 * @brief Private helper methods for LightingSystem
 *
 * Contains constant buffer creation, shadow map management, cascaded shadow maps,
 * and light buffer updates. Split from LightingSystem.cpp for maintainability.
 * Light culling and shadow matrix math live in
 * LightingSystemInternalWindowsCulling.cpp, IBL generation in
 * LightingSystemInternalWindowsIBL.cpp, and type string conversions in
 * LightingSystemInternalWindowsTypes.cpp.
 */

#include "LightingSystem.h"
#include "../Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"

#include <windows.h>
#include <d3d11_1.h>
#include "Core/Platform.h"
#include <wrl.h>

#include <algorithm>
#include <cmath>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

#ifdef SPARK_PLATFORM_WINDOWS

HRESULT LightingSystem::CreateConstantBuffers()
{
    if (!m_device)
        return E_FAIL;

    // Create light data buffer (supports up to 64 lights)
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.ByteWidth = sizeof(LightData) * 64;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_lightDataBuffer);
    if (FAILED(hr))
        return hr;

    // Create environment buffer.
    // NOTE: sizeof(EnvironmentLighting) is 104 bytes (4 ComPtrs + floats + a
    // bool) — NOT a multiple of 16, so CreateBuffer rejected it with
    // E_INVALIDARG and the whole LightingSystem failed to initialize
    // ("Failed to initialize LightingSystem" at boot). D3D11 constant
    // buffers must be 16-byte aligned; round the size up.
    bufferDesc.ByteWidth = (sizeof(EnvironmentLighting) + 15u) & ~15u;
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_environmentBuffer);
    if (FAILED(hr))
        return hr;

    // Create shadow data buffer
    bufferDesc.ByteWidth = sizeof(XMMATRIX) * 16; // Up to 16 shadow matrices
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_shadowDataBuffer);
    if (FAILED(hr))
        return hr;

    SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                   "LightingSystem: constant buffers created (light, environment, shadow)");
    return S_OK;
}

HRESULT LightingSystem::CreateShadowMap(uint32_t size, ShadowMap& shadowMap)
{
    if (!m_device)
        return E_FAIL;

    shadowMap.size = size;

    // Create shadow map texture
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = size;
    texDesc.Height = size;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &shadowMap.texture);
    if (FAILED(hr))
        return hr;

    // Create depth stencil view
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

    hr = m_device->CreateDepthStencilView(shadowMap.texture.Get(), &dsvDesc, &shadowMap.dsv);
    if (FAILED(hr))
        return hr;

    // Create shader resource view
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = m_device->CreateShaderResourceView(shadowMap.texture.Get(), &srvDesc, &shadowMap.srv);
    if (FAILED(hr))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "LightingSystem: failed to create shadow map SRV (size=%u)",
                        size);
        return hr;
    }

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "LightingSystem: shadow map created (%ux%u)", size, size);
    return S_OK;
}

HRESULT LightingSystem::CreateCascadedShadowMap()
{
    if (!m_device)
        return E_FAIL;

    m_csmShadowMap = std::make_unique<CascadedShadowMap>();
    m_csmShadowMap->cascades.resize(m_csmShadowMap->cascadeCount);

    for (auto& cascade : m_csmShadowMap->cascades)
    {
        HRESULT hr = CreateShadowMap(m_shadowMapSize, cascade);
        if (FAILED(hr))
            return hr;
    }

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "LightingSystem: cascaded shadow map created (%u cascades, %u size)",
                   m_csmShadowMap->cascadeCount, m_shadowMapSize);
    return S_OK;
}

void LightingSystem::UpdateLightBuffer()
{
    // Update metrics
    m_metrics.activeLights = static_cast<uint32_t>(m_lights.size());

    auto now = std::chrono::high_resolution_clock::now();
    static auto lastUpdate = now;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate);

    if (elapsed.count() >= 100)
    {
        m_metrics.lightCullingTime = elapsed.count() / 1000.0f;
        m_metrics.shadowRenderTime = m_metrics.shadowMapUpdates * 0.5f;
        m_metrics.shadowMapMemory = m_shadowMaps.size() * (m_shadowMapSize * m_shadowMapSize * 4) / (1024.0f * 1024.0f);
        lastUpdate = now;
    }

    // Upload light data array to the GPU constant buffer
    if (!m_context || !m_lightDataBuffer || m_lightDataArray.empty())
    {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_context->Map(m_lightDataBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr) && mapped.pData)
    {
        const size_t maxLights = 64;
        size_t lightCount = std::min(m_lightDataArray.size(), maxLights);
        size_t copySize = lightCount * sizeof(LightData);
        memcpy(mapped.pData, m_lightDataArray.data(), copySize);
        m_context->Unmap(m_lightDataBuffer.Get(), 0);
    }
    else
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "LightingSystem: failed to map light data buffer for update");
        Spark::SimpleConsole::GetInstance().LogWarning("Failed to map light data buffer for update");
    }

    // Upload environment data to the environment constant buffer
    if (m_environmentBuffer)
    {
        D3D11_MAPPED_SUBRESOURCE envMapped = {};
        hr = m_context->Map(m_environmentBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &envMapped);
        if (SUCCEEDED(hr) && envMapped.pData)
        {
            memcpy(envMapped.pData, &m_environmentLighting, sizeof(EnvironmentLighting));
            m_context->Unmap(m_environmentBuffer.Get(), 0);
        }
    }

    // Upload shadow matrices to the shadow data constant buffer
    if (m_shadowDataBuffer && !m_shadowMaps.empty())
    {
        D3D11_MAPPED_SUBRESOURCE shadowMapped = {};
        hr = m_context->Map(m_shadowDataBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &shadowMapped);
        if (SUCCEEDED(hr) && shadowMapped.pData)
        {
            const size_t maxShadowMatrices = 16;
            std::vector<XMMATRIX> shadowMatrices;
            shadowMatrices.reserve(maxShadowMatrices);

            for (const auto& pair : m_shadowMaps)
            {
                if (pair.second && shadowMatrices.size() < maxShadowMatrices)
                {
                    XMMATRIX lightViewProj = XMMatrixMultiply(pair.second->lightMatrix, pair.second->shadowMatrix);
                    shadowMatrices.push_back(lightViewProj);
                }
            }

            if (!shadowMatrices.empty())
            {
                memcpy(shadowMapped.pData, shadowMatrices.data(), shadowMatrices.size() * sizeof(XMMATRIX));
            }
            m_context->Unmap(m_shadowDataBuffer.Get(), 0);
        }
    }
}

void LightingSystem::UpdateShadowMaps(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix)
{
    if (!m_device)
    {
        return;
    }

    // Extract near and far planes from the projection matrix
    // For LH perspective projection: projMatrix._33 = far/(far-near), projMatrix._43 = -near*far/(far-near)
    float projNear = -XMVectorGetZ(projMatrix.r[3]) / XMVectorGetZ(projMatrix.r[2]);
    float projFar = XMVectorGetZ(projMatrix.r[3]) / (1.0f - XMVectorGetZ(projMatrix.r[2]));

    // Clamp to reasonable defaults if extraction fails
    if (projNear <= 0.0f || projNear != projNear)
    {
        projNear = 0.1f;
    }
    if (projFar <= projNear || projFar != projFar)
    {
        projFar = 1000.0f;
    }

    for (const auto& light : m_lights)
    {
        if (!light || !light->IsEnabled() || !light->GetCastShadows())
        {
            continue;
        }

        // Ensure a shadow map exists for this light
        auto it = m_shadowMaps.find(light.get());
        if (it == m_shadowMaps.end())
        {
            auto shadowMap = std::make_unique<ShadowMap>();
            if (SUCCEEDED(CreateShadowMap(light->GetShadowMapSize(), *shadowMap)))
            {
                m_shadowMaps[light.get()] = std::move(shadowMap);
                it = m_shadowMaps.find(light.get());
            }
            else
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                "LightingSystem: failed to create shadow map for light (size=%u)",
                                light->GetShadowMapSize());
                continue;
            }
        }

        if (!it->second)
        {
            continue;
        }

        // For directional lights with CSM technique, update cascaded shadow maps
        if (light->GetType() == LightType::Directional && light->GetShadowTechnique() == ShadowTechnique::CSM)
        {
            if (!m_csmShadowMap)
            {
                if (FAILED(CreateCascadedShadowMap()))
                {
                    Spark::SimpleConsole::GetInstance().LogWarning("Failed to create cascaded shadow map");
                    continue;
                }
            }

            // Recalculate cascade split distances
            CalculateCSMSplits(projNear, projFar, *m_csmShadowMap);

            // Update each cascade's light matrix
            m_csmShadowMap->lightMatrices.resize(m_csmShadowMap->cascadeCount);
            for (uint32_t cascade = 0; cascade < m_csmShadowMap->cascadeCount; ++cascade)
            {
                float cascadeNear = m_csmShadowMap->splitDistances[cascade];
                float cascadeFar = m_csmShadowMap->splitDistances[cascade + 1];

                XMMATRIX cascadeLightMatrix = CalculateLightMatrix(*light, viewMatrix, cascadeNear, cascadeFar);
                m_csmShadowMap->lightMatrices[cascade] = cascadeLightMatrix;

                if (cascade < m_csmShadowMap->cascades.size())
                {
                    m_csmShadowMap->cascades[cascade].lightMatrix = cascadeLightMatrix;
                    m_csmShadowMap->cascades[cascade].shadowMatrix = light->GetShadowMatrix();
                }
            }
        }
        else
        {
            // Standard shadow map: calculate tight-fitting light matrix
            it->second->lightMatrix = CalculateLightMatrix(*light, viewMatrix, projNear, projFar);
            it->second->shadowMatrix = light->GetShadowMatrix();
        }
    }
}

#endif // inner SPARK_PLATFORM_WINDOWS


#endif // SPARK_PLATFORM_WINDOWS
