#include "Core/Platform.h"
#include "Utils/MathUtils.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file LightingSystem.cpp
 * @brief Complete lighting system implementation with PBR support
 */

#include "LightingSystem.h"
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
// ============================================================================
// LIGHT CLASS IMPLEMENTATION
// ============================================================================

Light::Light(LightType type) : m_type(type)
{
    // Initialize light based on type
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
    XMVECTOR position = XMLoadFloat3(&m_position);
    XMVECTOR direction = XMLoadFloat3(&m_direction);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    // Create look-at matrix for light
    XMVECTOR target = XMVectorAdd(position, direction);
    return XMMatrixLookAtLH(position, target, up);
}

XMMATRIX Light::GetShadowMatrix() const
{
    switch (m_type)
    {
    case LightType::Directional:
        return XMMatrixOrthographicLH(20.0f, 20.0f, 0.1f, 100.0f);
    case LightType::Point:
        return XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 0.1f, m_range);
    case LightType::Spot:
        return XMMatrixPerspectiveFovLH(XMConvertToRadians(m_spotAngle), 1.0f, 0.1f, m_range);
    case LightType::Area:
        return XMMatrixPerspectiveFovLH(XM_PIDIV4, 1.0f, 0.1f, m_range);
    default:
        return XMMatrixIdentity();
    }
}

LightData Light::GetShaderData() const
{
    LightData data = {};

    data.position = XMFLOAT4(m_position.x, m_position.y, m_position.z, static_cast<float>(m_type));
    data.direction = XMFLOAT4(m_direction.x, m_direction.y, m_direction.z, XMConvertToRadians(m_spotAngle));
    data.color = XMFLOAT4(m_color.x, m_color.y, m_color.z, m_intensity);
    data.attenuation = XMFLOAT4(m_attenuation.x, m_attenuation.y, m_attenuation.z, m_range);
    data.shadowParams = XMFLOAT4(m_castShadows ? 1.0f : 0.0f, m_shadowBias, 0.0f, 0.0f);
    data.lightMatrix = GetLightMatrix();
    data.shadowMatrix = GetShadowMatrix();

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
    {
        SetIntensity(value);
    }
    else if (property == "range")
    {
        SetRange(value);
    }
    else if (property == "spotangle")
    {
        SetSpotAngle(value);
    }
    else if (property == "shadowbias")
    {
        SetShadowBias(value);
    }
}

void Light::Console_SetColor(float r, float g, float b)
{
    SetColor({std::max(0.0f, std::min(1.0f, r)), std::max(0.0f, std::min(1.0f, g)), std::max(0.0f, std::min(1.0f, b))});
}

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

    m_device = nullptr;
    m_context = nullptr;

    Spark::SimpleConsole::GetInstance().LogInfo("LightingSystem shutdown complete");
}

void LightingSystem::Update(float deltaTime, const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_WARN_IF(Spark::LogCategory::Graphics, deltaTime < 0.0f,
                  "LightingSystem::Update called with negative deltaTime");
    // Update metrics
    m_metrics.activeLights = static_cast<uint32_t>(m_lights.size());
    m_metrics.shadowCastingLights = 0;
    m_metrics.visibleLights = 0;

    // Count shadow casting lights and update light data
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

            // Mark light as clean after processing
            light->SetClean();
        }
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

    // Render standard shadow maps for each shadow-casting light
    for (const auto& light : m_lights)
    {
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
                // Standard shadow map: use the stored light/shadow matrices
                renderCallback(shadowMap.lightMatrix, shadowMap.shadowMatrix);
                m_metrics.shadowMapUpdates++;
            }
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
// LIGHT MANAGEMENT METHODS
// ============================================================================

std::shared_ptr<Light> LightingSystem::CreateLight(LightType type)
{
    SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "Creating light of type %d", static_cast<int>(type));
    auto light = std::make_shared<Light>(type);
    m_lights.push_back(light);

    // Create shadow map for this light if shadows are enabled
    if (light->GetCastShadows() && m_shadowsEnabled)
    {
        auto shadowMap = std::make_unique<ShadowMap>();
        if (SUCCEEDED(CreateShadowMap(m_shadowMapSize, *shadowMap)))
        {
            m_shadowMaps[light.get()] = std::move(shadowMap);
        }
    }

    Spark::SimpleConsole::GetInstance().LogInfo("Created new light of type: " + std::to_string(static_cast<int>(type)));
    return light;
}

void LightingSystem::AddLight(std::shared_ptr<Light> light)
{
    SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, light);
    if (light)
    {
        m_lights.push_back(light);

        // Create shadow map if needed
        if (light->GetCastShadows() && m_shadowsEnabled)
        {
            auto shadowMap = std::make_unique<ShadowMap>();
            if (SUCCEEDED(CreateShadowMap(m_shadowMapSize, *shadowMap)))
            {
                m_shadowMaps[light.get()] = std::move(shadowMap);
            }
        }
    }
}

void LightingSystem::RemoveLight(std::shared_ptr<Light> light)
{
    SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, light);
    if (light)
    {
        // Remove shadow map
        auto it = m_shadowMaps.find(light.get());
        if (it != m_shadowMaps.end())
        {
            m_shadowMaps.erase(it);
        }

        // Remove from lights vector
        m_lights.erase(std::remove(m_lights.begin(), m_lights.end(), light), m_lights.end());
    }
}

void LightingSystem::RemoveAllLights()
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Removing all lights (%zu total)", m_lights.size());
    m_shadowMaps.clear();
    m_lights.clear();

    // Recreate default directional light
    auto defaultLight = std::make_shared<Light>(LightType::Directional);
    defaultLight->SetDirection({0.3f, -0.7f, 0.2f});
    defaultLight->SetColor({1.0f, 0.95f, 0.8f});
    defaultLight->SetIntensity(3.0f);
    m_lights.push_back(defaultLight);
}

void LightingSystem::SetEnvironmentMap(const std::string& filePath)
{
    // This would normally load an HDR environment map
    // For now, just log the request
    Spark::SimpleConsole::GetInstance().LogInfo("Environment map set to: " + filePath);

    // Generate IBL textures after loading
    GenerateIBLTextures();
}

void LightingSystem::GenerateIBLTextures()
{
    if (!m_device)
        return;

    // This would normally generate irradiance map, prefilter map, and BRDF LUT
    // For now, just log the operation
    Spark::SimpleConsole::GetInstance().LogInfo("Generating IBL textures");

    HRESULT hr = GenerateIrradianceMap(m_environmentLighting.environmentMap.Get());
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to generate irradiance map");
        return;
    }
    hr = GeneratePrefilterMap(m_environmentLighting.environmentMap.Get());
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to generate prefilter map");
        return;
    }
    hr = GenerateBRDFLUT();
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to generate BRDF LUT");
        return;
    }
    Spark::SimpleConsole::GetInstance().LogSuccess("IBL textures generated successfully");
}

// ============================================================================
// CONSOLE INTEGRATION METHODS
// ============================================================================

std::string LightingSystem::Console_GetLightInfo(int lightIndex) const
{
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size()))
    {
        return "Error: Invalid light index " + std::to_string(lightIndex);
    }

    const auto& light = m_lights[lightIndex];
    if (!light)
    {
        return "Error: Light at index " + std::to_string(lightIndex) + " is null";
    }

    return "Light [" + std::to_string(lightIndex) + "]:\n" + light->GetInfo();
}

int LightingSystem::Console_CreateLight(const std::string& type)
{
    LightType lightType = StringToLightType(type);
    auto light = CreateLight(lightType);

    if (m_lights.empty())
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create light of type: " + type);
        return -1;
    }

    int index = static_cast<int>(m_lights.size() - 1);
    Spark::SimpleConsole::GetInstance().LogSuccess("Created light at index " + std::to_string(index));
    return index;
}

bool LightingSystem::Console_DeleteLight(int lightIndex)
{
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size()))
    {
        Spark::SimpleConsole::GetInstance().LogError("Invalid light index: " + std::to_string(lightIndex));
        return false;
    }

    auto light = m_lights[lightIndex];
    RemoveLight(light);

    Spark::SimpleConsole::GetInstance().LogSuccess("Deleted light at index " + std::to_string(lightIndex));
    return true;
}

void LightingSystem::Console_SetLightProperty(int lightIndex, const std::string& property, float value)
{
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size()))
    {
        Spark::SimpleConsole::GetInstance().LogError("Invalid light index: " + std::to_string(lightIndex));
        return;
    }

    auto& light = m_lights[lightIndex];
    if (light)
    {
        light->Console_SetProperty(property, value);
        Spark::SimpleConsole::GetInstance().LogSuccess("Set " + property + " = " + std::to_string(value) +
                                                       " for light " + std::to_string(lightIndex));
    }
}

void LightingSystem::Console_SetLightColor(int lightIndex, float r, float g, float b)
{
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size()))
    {
        Spark::SimpleConsole::GetInstance().LogError("Invalid light index: " + std::to_string(lightIndex));
        return;
    }

    auto& light = m_lights[lightIndex];
    if (light)
    {
        light->Console_SetColor(r, g, b);
        Spark::SimpleConsole::GetInstance().LogSuccess("Set color for light " + std::to_string(lightIndex));
    }
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
    else
    {
        Spark::SimpleConsole::GetInstance().LogError("Invalid shadow quality: " + quality);
        return;
    }

    SetGlobalShadowQuality(size);
    Spark::SimpleConsole::GetInstance().LogSuccess("Shadow quality set to " + quality);
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

    Spark::SimpleConsole::GetInstance().LogSuccess("Environment set to " + skyType);
}

void LightingSystem::Console_EnableLightCulling(bool enabled)
{
    EnableLightCulling(enabled);
    Spark::SimpleConsole::GetInstance().LogInfo("Light culling " + std::string(enabled ? "enabled" : "disabled"));
}

void LightingSystem::Console_ReloadIBL()
{
    GenerateIBLTextures();
    Spark::SimpleConsole::GetInstance().LogSuccess("IBL textures reloaded");
}

// ============================================================================
// PRIVATE HELPER METHODS
// ============================================================================


#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

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
    m_device = nullptr;
    m_context = nullptr;
}

void LightingSystem::Update(float /*deltaTime*/, const XMMATRIX& /*viewMatrix*/, const XMMATRIX& /*projMatrix*/)
{
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

#endif // SPARK_PLATFORM_WINDOWS
