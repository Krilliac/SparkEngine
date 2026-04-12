/**
 * @file LightingSystemLinux.cpp
 * @brief Linux implementation — split from LightingSystem.cpp
 */
#include "Core/Platform.h"
#include "LightingSystem.h"
#include "Utils/MathUtils.h"
#ifndef SPARK_PLATFORM_WINDOWS


#include "LightingSystem.h"
#include "../Utils/Hash.h"
#include "../Utils/Validate.h"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>

// ============================================================================
// Light (Linux stub)
// ============================================================================

Light::Light(LightType type) : m_type(type)
{
    switch (type)
    {
    case LightType::Directional:
        m_position = {0.0f, 10.0f, 0.0f};
        m_direction = {0.0f, -1.0f, 0.0f};
        m_intensity = 3.0f;
        m_range = 1000.0f;
        break;
    case LightType::Point:
        m_position = {0.0f, 2.0f, 0.0f};
        m_direction = {0.0f, -1.0f, 0.0f};
        m_intensity = 10.0f;
        m_range = 10.0f;
        break;
    case LightType::Spot:
        m_position = {0.0f, 5.0f, 0.0f};
        m_direction = {0.0f, -1.0f, 0.0f};
        m_intensity = 15.0f;
        m_range = 15.0f;
        m_spotAngle = 30.0f;
        break;
    case LightType::Area:
        m_position = {0.0f, 3.0f, 0.0f};
        m_direction = {0.0f, -1.0f, 0.0f};
        m_intensity = 8.0f;
        m_range = 12.0f;
        break;
    case LightType::Environment:
        m_intensity = 1.0f;
        m_castShadows = false;
        break;
    }
    m_dirty = true;
}

XMMATRIX Light::GetLightMatrix() const
{
    XMMATRIX m;
    memset(&m, 0, sizeof(m));
    return m;
}

XMMATRIX Light::GetShadowMatrix() const
{
    XMMATRIX m;
    memset(&m, 0, sizeof(m));
    return m;
}

LightData Light::GetShaderData() const
{
    LightData data;
    memset(&data, 0, sizeof(data));
    data.position = XMFLOAT4(m_position.x, m_position.y, m_position.z, static_cast<float>(m_type));
    data.direction = XMFLOAT4(m_direction.x, m_direction.y, m_direction.z, MathUtils::DegreesToRadians(m_spotAngle));
    data.color = XMFLOAT4(m_color.x, m_color.y, m_color.z, m_intensity);
    data.attenuation = XMFLOAT4(m_attenuation.x, m_attenuation.y, m_attenuation.z, m_range);
    data.shadowParams = XMFLOAT4(m_castShadows ? 1.0f : 0.0f, m_shadowBias, 0.0f, 0.0f);
    return data;
}

std::string Light::GetInfo() const
{
    std::stringstream ss;
    ss << "Light Type: " << static_cast<int>(m_type) << "\n";
    ss << "Position: (" << m_position.x << ", " << m_position.y << ", " << m_position.z << ")\n";
    ss << "Direction: (" << m_direction.x << ", " << m_direction.y << ", " << m_direction.z << ")\n";
    ss << "Color: (" << m_color.x << ", " << m_color.y << ", " << m_color.z << ")\n";
    ss << "Intensity: " << m_intensity << "\n";
    ss << "Range: " << m_range << "\n";
    ss << "Enabled: " << (m_enabled ? "Yes" : "No") << "\n";
    ss << "Cast Shadows: " << (m_castShadows ? "Yes" : "No") << "\n";
    return ss.str();
}

void Light::Console_SetProperty(const std::string& property, float value)
{
    if (property == "intensity")
        SetIntensity(value);
    else if (property == "range")
        SetRange(value);
    else if (property == "spotangle")
        SetSpotAngle(value);
    else if (property == "shadowbias")
        SetShadowBias(value);
}

void Light::Console_SetColor(float r, float g, float b)
{
    SetColor({std::max(0.0f, std::min(1.0f, r)), std::max(0.0f, std::min(1.0f, g)), std::max(0.0f, std::min(1.0f, b))});
}

// ============================================================================
// LightingSystem (Linux stub)
// ============================================================================

LightingSystem::LightingSystem() : m_device(nullptr), m_context(nullptr)
{
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
    m_device = device;
    m_context = context;
    m_environmentLighting.skyColor = {0.5f, 0.7f, 1.0f};
    m_environmentLighting.skyIntensity = 1.0f;

    // Phase M: the Tier 2 orphan caches run on every platform because
    // they are pure CPU. The Linux stub tracks the exact same lifecycle
    // as the Windows path so portable tests see consistent state.
    m_shadowCache.Initialize(2048, 4096, 256);
    m_probeCache.Initialize(64, 4);

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "LightingSystem (Linux) initialized");
    return S_OK;
}

void LightingSystem::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "LightingSystem (Linux) shutting down");
    m_lights.clear();
    m_lightDataArray.clear();
    m_shadowMaps.clear();
    m_csmShadowMap.reset();
    // Phase M: tear down orphan caches alongside the rest of the state.
    m_shadowCache.Shutdown();
    m_probeCache.Shutdown();
    m_device = nullptr;
    m_context = nullptr;
}

void LightingSystem::Update(float /*deltaTime*/, const XMMATRIX& viewMatrix, const XMMATRIX& /*projMatrix*/)
{
    // Phase M: mirror the Windows tick path — begin frame on the shadow
    // atlas, pull the camera position from the inverse view matrix, and
    // feed the probe cache. EndFrame is called at the bottom of the
    // method so the two sub-atlases advance their state.
    m_shadowCache.BeginFrame();
    XMVECTOR cameraPosVec = XMMatrixInverse(nullptr, viewMatrix).r[3];
    float camX = XMVectorGetX(cameraPosVec);
    float camY = XMVectorGetY(cameraPosVec);
    float camZ = XMVectorGetZ(cameraPosVec);
    m_probeCache.Update(camX, camY, camZ);

    m_metrics.activeLights = static_cast<uint32_t>(m_lights.size());
    m_metrics.shadowCastingLights = 0;
    m_metrics.visibleLights = 0;

    m_lightDataArray.clear();
    m_lightDataArray.reserve(m_lights.size());

    for (const auto& light : m_lights)
    {
        if (light && light->IsEnabled())
        {
            m_lightDataArray.push_back(light->GetShaderData());
            m_metrics.visibleLights++;
            if (light->GetCastShadows())
            {
                m_metrics.shadowCastingLights++;
            }
            light->SetClean();
        }
    }
    m_metrics.culledLights = m_metrics.activeLights - m_metrics.visibleLights;

    // Phase M: advance the cached shadow atlas frame state.
    m_shadowCache.EndFrame();
}

void LightingSystem::RenderShadowMaps(std::function<void(const XMMATRIX&, const XMMATRIX&)> /*renderCallback*/)
{
    // No-op on Linux
}

void LightingSystem::BindLightingData(ID3D11DeviceContext* /*context*/)
{
    // No-op on Linux
}

std::shared_ptr<Light> LightingSystem::CreateLight(LightType type)
{
    auto light = std::make_shared<Light>(type);
    m_lights.push_back(light);
    return light;
}

void LightingSystem::AddLight(std::shared_ptr<Light> light)
{
    if (light)
    {
        m_lights.push_back(light);
    }
}

void LightingSystem::RemoveLight(std::shared_ptr<Light> light)
{
    if (light)
    {
        m_lights.erase(std::remove(m_lights.begin(), m_lights.end(), light), m_lights.end());
    }
}

void LightingSystem::RemoveAllLights()
{
    m_lights.clear();
    auto defaultLight = std::make_shared<Light>(LightType::Directional);
    defaultLight->SetDirection({0.3f, -0.7f, 0.2f});
    defaultLight->SetColor({1.0f, 0.95f, 0.8f});
    defaultLight->SetIntensity(3.0f);
    m_lights.push_back(defaultLight);
}

void LightingSystem::SetEnvironmentMap(const std::string& /*filePath*/)
{
    // No-op on Linux
}

void LightingSystem::GenerateIBLTextures()
{
    // No-op on Linux
}

void LightingSystem::SetGlobalShadowQuality(uint32_t size)
{
    m_shadowMapSize = size;
}

void LightingSystem::EnableShadows(bool enabled)
{
    m_shadowsEnabled = enabled;
}

LightingSystem::LightingMetrics LightingSystem::Console_GetMetrics() const
{
    return m_metrics;
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
    ss << "Shadow Quality: " << m_shadowMapSize << "x" << m_shadowMapSize;
    return ss.str();
}

std::string LightingSystem::Console_GetLightInfo(int lightIndex) const
{
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size()))
    {
        return "Error: Invalid light index " + std::to_string(lightIndex);
    }
    const auto& light = m_lights[lightIndex];
    if (!light)
        return "Error: Light at index " + std::to_string(lightIndex) + " is null";
    return "Light [" + std::to_string(lightIndex) + "]:\n" + light->GetInfo();
}

int LightingSystem::Console_CreateLight(const std::string& type)
{
    LightType lightType = StringToLightType(type);
    CreateLight(lightType);
    if (m_lights.empty())
        return -1;
    return static_cast<int>(m_lights.size() - 1);
}

bool LightingSystem::Console_DeleteLight(int lightIndex)
{
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size()))
        return false;
    auto light = m_lights[lightIndex];
    RemoveLight(light);
    return true;
}

void LightingSystem::Console_SetLightProperty(int lightIndex, const std::string& property, float value)
{
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size()))
        return;
    auto& light = m_lights[lightIndex];
    if (light)
        light->Console_SetProperty(property, value);
}

void LightingSystem::Console_SetLightColor(int lightIndex, float r, float g, float b)
{
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size()))
        return;
    auto& light = m_lights[lightIndex];
    if (light)
        light->Console_SetColor(r, g, b);
}

void LightingSystem::Console_EnableShadows(bool enabled)
{
    EnableShadows(enabled);
}

void LightingSystem::Console_SetShadowQuality(const std::string& quality)
{
    uint32_t size = 1024;
    if (quality == "low")
        size = 512;
    else if (quality == "medium")
        size = 1024;
    else if (quality == "high")
        size = 2048;
    else if (quality == "ultra")
        size = 4096;
    SetGlobalShadowQuality(size);
}

void LightingSystem::Console_SetEnvironment(const std::string& skyType)
{
    if (skyType == "clear")
    {
        m_environmentLighting.skyColor = {0.5f, 0.7f, 1.0f};
        m_environmentLighting.skyIntensity = 1.0f;
        m_environmentLighting.fogEnabled = false;
    }
    else if (skyType == "overcast")
    {
        m_environmentLighting.skyColor = {0.6f, 0.6f, 0.6f};
        m_environmentLighting.skyIntensity = 0.8f;
        m_environmentLighting.fogEnabled = true;
        m_environmentLighting.fogDensity = 0.02f;
    }
    else if (skyType == "sunset")
    {
        m_environmentLighting.skyColor = {1.0f, 0.6f, 0.3f};
        m_environmentLighting.skyIntensity = 1.2f;
        m_environmentLighting.fogEnabled = false;
    }
    else if (skyType == "night")
    {
        m_environmentLighting.skyColor = {0.1f, 0.1f, 0.3f};
        m_environmentLighting.skyIntensity = 0.3f;
        m_environmentLighting.fogEnabled = false;
    }
}

void LightingSystem::Console_EnableLightCulling(bool enabled)
{
    EnableLightCulling(enabled);
}

void LightingSystem::Console_ReloadIBL()
{
    GenerateIBLTextures();
}

// Private helpers (no-op on Linux)
// ============================================================================
// Utility functions
// ============================================================================

std::string LightTypeToString(LightType type)
{
    switch (type)
    {
    case LightType::Directional:
        return "directional";
    case LightType::Point:
        return "point";
    case LightType::Spot:
        return "spot";
    case LightType::Area:
        return "area";
    case LightType::Environment:
        return "environment";
    default:
        return "unknown";
    }
}

LightType StringToLightType(const std::string& str)
{
    using namespace Spark::HashLiterals;
    switch (Spark::FNV1a64(str))
    {
    case "directional"_hash64:
        return LightType::Directional;
    case "point"_hash64:
        return LightType::Point;
    case "spot"_hash64:
        return LightType::Spot;
    case "area"_hash64:
        return LightType::Area;
    case "environment"_hash64:
        return LightType::Environment;
    default:
        return LightType::Directional;
    }
}

std::string ShadowTechniqueToString(ShadowTechnique technique)
{
    switch (technique)
    {
    case ShadowTechnique::None:
        return "none";
    case ShadowTechnique::Basic:
        return "basic";
    case ShadowTechnique::PCF:
        return "pcf";
    case ShadowTechnique::VSM:
        return "vsm";
    case ShadowTechnique::CSM:
        return "csm";
    case ShadowTechnique::PCSS:
        return "pcss";
    default:
        return "unknown";
    }
}

ShadowTechnique StringToShadowTechnique(const std::string& str)
{
    using namespace Spark::HashLiterals;
    switch (Spark::FNV1a64(str))
    {
    case "none"_hash64:
        return ShadowTechnique::None;
    case "basic"_hash64:
        return ShadowTechnique::Basic;
    case "pcf"_hash64:
        return ShadowTechnique::PCF;
    case "vsm"_hash64:
        return ShadowTechnique::VSM;
    case "csm"_hash64:
        return ShadowTechnique::CSM;
    case "pcss"_hash64:
        return ShadowTechnique::PCSS;
    default:
        return ShadowTechnique::PCF;
    }
}


#endif // !SPARK_PLATFORM_WINDOWS
