/**
 * @file LightingSystemWindows.cpp
 * @brief Windows/D3D11 implementation — split from LightingSystem.cpp
 */
#include "LightingSystem.h"
#include "Core/Platform.h"
#include "Utils/MathUtils.h"
#ifdef SPARK_PLATFORM_WINDOWS

/**
 * @file LightingSystem.cpp
 * @brief Complete lighting system implementation with PBR support
 */

#include "Utils/Assert.h"
#include "../Utils/Hash.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <DirectXColors.h>

#ifdef SPARK_PLATFORM_WINDOWS

using namespace DirectX;

// Light lives in LightingSystemWindowsTypes.cpp; light management and the
// Console_* command surface live in LightingSystemWindowsLightOps.cpp.

// ============================================================================
// LIGHTING SYSTEM IMPLEMENTATION
// ============================================================================

LightingSystem::LightingSystem() : m_device(nullptr), m_context(nullptr)
{
    // Create default directional light (sun)
    m_lights.push_back(std::make_shared<Light>(LightType::Directional));
    m_lights[0]->SetDirection({0.3f, -0.7f, 0.2f});
    m_lights[0]->SetColor({1.0f, 0.95f, 0.8f});
    m_lights[0]->SetIntensity(3.0f);
}

LightingSystem::~LightingSystem()
{
    Shutdown();
}

HRESULT LightingSystem::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, context);

    m_device = device;
    m_context = context;

    // Create constant buffers
    HRESULT hr = CreateConstantBuffers();
    if (FAILED(hr))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to create lighting constant buffers");
        return hr;
    }

    // Create default environment
    hr = CreateDefaultEnvironment();
    if (FAILED(hr))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Failed to create default environment");
    }

    // Phase M: activate the Tier 2 orphans that belong on the lighting
    // surface. Both are pure CPU — a failure here does not block the
    // lighting system from running the existing shadow map / IBL paths.
    if (!m_shadowCache.Initialize(/*dynamic*/ 2048, /*cached*/ 4096, /*minTile*/ 256))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "LightingSystem: CachedShadowAtlas::Initialize returned false");
    }
    m_probeCache.Initialize(/*maxCachedProbes*/ 64, /*renderBudget*/ 4);

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "LightingSystem initialized with %zu lights", m_lights.size());
    return S_OK;
}

void LightingSystem::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "LightingSystem shutting down (%zu lights, %zu shadow maps)",
                   m_lights.size(), m_shadowMaps.size());
    // Clear lights
    m_lights.clear();
    m_lightDataArray.clear();

    // Clear shadow maps
    m_shadowMaps.clear();
    m_csmShadowMap.reset();

    // Reset DirectX resources
    m_lightBuffer.Reset();
    m_lightBufferSRV.Reset();
    m_lightDataBuffer.Reset();
    m_environmentBuffer.Reset();
    m_shadowDataBuffer.Reset();

    // Clear environment resources
    m_environmentLighting.environmentMap.Reset();
    m_environmentLighting.irradianceMap.Reset();
    m_environmentLighting.prefilterMap.Reset();
    m_environmentLighting.brdfLUT.Reset();

    // Phase M: tear down the orphan caches. Both are safe to call on an
    // uninitialised instance (each guards its own m_initialized flag).
    m_shadowCache.Shutdown();
    m_probeCache.Shutdown();

    m_device = nullptr;
    m_context = nullptr;

    Spark::SimpleConsole::GetInstance().LogInfo("LightingSystem shutdown complete");
}

void LightingSystem::Update(float deltaTime, const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_WARN_IF(Spark::LogCategory::Graphics, deltaTime < 0.0f,
                  "LightingSystem::Update called with negative deltaTime");

    // Phase M: tick the Tier 2 orphan caches before anything else reads
    // from them. `BeginFrame` clears the per-frame shadow render list
    // and advances the internal frame counter; the probe cache's
    // `Update()` runs the priority/budget evaluator against the camera
    // position extracted from the inverse view matrix.
    m_shadowCache.BeginFrame();

    XMVECTOR cameraPosVec = XMMatrixInverse(nullptr, viewMatrix).r[3];
    float camX = XMVectorGetX(cameraPosVec);
    float camY = XMVectorGetY(cameraPosVec);
    float camZ = XMVectorGetZ(cameraPosVec);
    m_probeCache.Update(camX, camY, camZ);

    // Update metrics
    m_metrics.activeLights = static_cast<uint32_t>(m_lights.size());
    m_metrics.shadowCastingLights = 0;
    m_metrics.visibleLights = 0;

    // Count shadow casting lights and update light data
    m_lightDataArray.clear();
    m_lightDataArray.reserve(m_lights.size());

    uint32_t lightIndex = 0;
    for (const auto& light : m_lights)
    {
        if (light && light->IsEnabled())
        {
            m_lightDataArray.push_back(light->GetShaderData());
            m_metrics.visibleLights++;

            if (light->GetCastShadows())
            {
                m_metrics.shadowCastingLights++;

                // Reserve this light's tile in the cached shadow atlas. Nothing
                // used to call RequestShadow, so the allocator reported an empty
                // atlas every frame no matter how many shadow casters existed.
                // Lights carry no static flag yet, so every request is dynamic —
                // the atlas tracks occupancy, it does not skip any render.
                Spark::Graphics::ShadowUpdateRequest shadowRequest;
                shadowRequest.lightId = lightIndex;
                shadowRequest.priority = light->GetIntensity();
                shadowRequest.isStatic = false;
                const XMFLOAT3& lightPosition = light->GetPosition();
                const XMFLOAT3& lightDirection = light->GetDirection();
                shadowRequest.posX = lightPosition.x;
                shadowRequest.posY = lightPosition.y;
                shadowRequest.posZ = lightPosition.z;
                shadowRequest.dirX = lightDirection.x;
                shadowRequest.dirY = lightDirection.y;
                shadowRequest.dirZ = lightDirection.z;
                shadowRequest.range = light->GetRange();
                shadowRequest.spotAngle = light->GetSpotAngle();
                m_shadowCache.RequestShadow(shadowRequest);
            }

            // Mark light as clean after processing
            light->SetClean();
        }

        ++lightIndex;
    }

    // Perform frustum-based light culling if enabled
    if (m_lightCullingEnabled)
    {
        CullLights(viewMatrix, projMatrix);
    }

    // Update light buffer
    UpdateLightBuffer();

    // Update shadow maps if shadows are enabled
    if (m_shadowsEnabled)
    {
        UpdateShadowMaps(viewMatrix, projMatrix);
    }

    // Update culling metrics
    m_metrics.culledLights = m_metrics.activeLights - m_metrics.visibleLights;

    // Phase M: close the cached shadow atlas frame so the two sub-
    // atlases ratchet their per-frame state. The probe cache does not
    // have a corresponding EndFrame — its frame counter advances at
    // the top of `Update()`.
    m_shadowCache.EndFrame();
}

void LightingSystem::EnableShadows(bool enabled)
{
    m_shadowsEnabled = enabled;
    Spark::SimpleConsole::GetInstance().LogInfo("Shadows " + std::string(enabled ? "enabled" : "disabled") +
                                                " globally");
}

void LightingSystem::SetGlobalShadowQuality(uint32_t size)
{
    m_shadowMapSize = size;

    // Recreate existing shadow maps with new size
    for (auto& pair : m_shadowMaps)
    {
        if (pair.second)
        {
            CreateShadowMap(size, *pair.second);
        }
    }

    Spark::SimpleConsole::GetInstance().LogInfo("Shadow map quality set to " + std::to_string(size) + "x" +
                                                std::to_string(size));
}

void LightingSystem::Console_EnableShadows(bool enabled)
{
    EnableShadows(enabled);
    Spark::SimpleConsole::GetInstance().LogInfo("Console command: Shadows " +
                                                std::string(enabled ? "enabled" : "disabled"));
}

std::string LightingSystem::Console_ListLights() const
{
    std::stringstream ss;
    ss << "Lighting System - Active Lights (" << m_lights.size() << "):\n";

    for (size_t i = 0; i < m_lights.size(); ++i)
    {
        const auto& light = m_lights[i];
        if (light)
        {
            ss << "  [" << i << "] ";
            switch (light->GetType())
            {
            case LightType::Directional:
                ss << "Directional Light";
                break;
            case LightType::Point:
                ss << "Point Light";
                break;
            case LightType::Spot:
                ss << "Spot Light";
                break;
            case LightType::Area:
                ss << "Area Light";
                break;
            case LightType::Environment:
                ss << "Environment Light";
                break;
            }
            ss << " - " << (light->IsEnabled() ? "Enabled" : "Disabled");
            if (light->GetCastShadows())
                ss << " (Shadows)";
            ss << "\n";
        }
    }

    ss << "Environment Light: " << (m_environmentLighting.fogEnabled ? "Enabled" : "Disabled") << "\n";
    ss << "Shadow Quality: " << m_shadowMapSize << "x" << m_shadowMapSize;

    return ss.str();
}

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

LightingSystem::LightingMetrics LightingSystem::Console_GetMetrics() const
{
    return m_metrics;
}

// ============================================================================
// PRIVATE HELPER METHODS
// ============================================================================


#endif // inner SPARK_PLATFORM_WINDOWS


#endif // SPARK_PLATFORM_WINDOWS
