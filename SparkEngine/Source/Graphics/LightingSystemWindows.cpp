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

LightingSystem::LightingMetrics LightingSystem::Console_GetMetrics() const
{
    return m_metrics;
}

// ============================================================================
// PRIVATE HELPER METHODS
// ============================================================================


#endif // inner SPARK_PLATFORM_WINDOWS


#endif // SPARK_PLATFORM_WINDOWS
