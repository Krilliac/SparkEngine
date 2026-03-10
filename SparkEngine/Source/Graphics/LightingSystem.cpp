#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file LightingSystem.cpp
 * @brief Complete lighting system implementation with PBR support
 */

#include "LightingSystem.h"
#include "Utils/Assert.h"
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
    ASSERT(device && context);

    m_device = device;
    m_context = context;

    // Create constant buffers
    HRESULT hr = CreateConstantBuffers();
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create constant buffers");
        return hr;
    }

    // Create default environment
    hr = CreateDefaultEnvironment();
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogWarning("Failed to create default environment");
    }

    Spark::SimpleConsole::GetInstance().LogSuccess("LightingSystem initialized successfully");
    return S_OK;
}

void LightingSystem::Shutdown()
{
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
        if (SUCCEEDED(hr))
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
        context->PSSetShaderResources(shadowMapStartSlot,
                                       static_cast<UINT>(shadowSRVs.size()),
                                       shadowSRVs.data());
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
            context->PSSetShaderResources(csmStartSlot,
                                           static_cast<UINT>(csmSRVs.size()),
                                           csmSRVs.data());
        }
    }

    // Bind IBL textures to pixel shader (slots 8-11)
    constexpr UINT iblStartSlot = 8;
    ID3D11ShaderResourceView* iblSRVs[4] = {
        m_environmentLighting.irradianceMap.Get(),
        m_environmentLighting.prefilterMap.Get(),
        m_environmentLighting.brdfLUT.Get(),
        m_environmentLighting.environmentMap.Get()
    };
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
            if (light->GetType() == LightType::Directional &&
                light->GetShadowTechnique() == ShadowTechnique::CSM &&
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

    // Create environment buffer
    bufferDesc.ByteWidth = sizeof(EnvironmentLighting);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_environmentBuffer);
    if (FAILED(hr))
        return hr;

    // Create shadow data buffer
    bufferDesc.ByteWidth = sizeof(XMMATRIX) * 16; // Up to 16 shadow matrices
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_shadowDataBuffer);
    if (FAILED(hr))
        return hr;

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
        return hr;

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
        m_metrics.shadowMapMemory =
            m_shadowMaps.size() * (m_shadowMapSize * m_shadowMapSize * 4) / (1024.0f * 1024.0f);
        lastUpdate = now;
    }

    // Upload light data array to the GPU constant buffer
    if (!m_context || !m_lightDataBuffer || m_lightDataArray.empty())
    {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_context->Map(m_lightDataBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        const size_t maxLights = 64;
        size_t lightCount = std::min(m_lightDataArray.size(), maxLights);
        size_t copySize = lightCount * sizeof(LightData);
        memcpy(mapped.pData, m_lightDataArray.data(), copySize);
        m_context->Unmap(m_lightDataBuffer.Get(), 0);
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogWarning("Failed to map light data buffer for update");
    }

    // Upload environment data to the environment constant buffer
    if (m_environmentBuffer)
    {
        D3D11_MAPPED_SUBRESOURCE envMapped = {};
        hr = m_context->Map(m_environmentBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &envMapped);
        if (SUCCEEDED(hr))
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
        if (SUCCEEDED(hr))
        {
            const size_t maxShadowMatrices = 16;
            std::vector<XMMATRIX> shadowMatrices;
            shadowMatrices.reserve(maxShadowMatrices);

            for (const auto& pair : m_shadowMaps)
            {
                if (pair.second && shadowMatrices.size() < maxShadowMatrices)
                {
                    XMMATRIX lightViewProj = XMMatrixMultiply(
                        pair.second->lightMatrix, pair.second->shadowMatrix);
                    shadowMatrices.push_back(lightViewProj);
                }
            }

            if (!shadowMatrices.empty())
            {
                memcpy(shadowMapped.pData, shadowMatrices.data(),
                       shadowMatrices.size() * sizeof(XMMATRIX));
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
    float projNear = -projMatrix.r[3].m128_f32[2] / projMatrix.r[2].m128_f32[2];
    float projFar = projMatrix.r[3].m128_f32[2] / (1.0f - projMatrix.r[2].m128_f32[2]);

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
                continue;
            }
        }

        if (!it->second)
        {
            continue;
        }

        // For directional lights with CSM technique, update cascaded shadow maps
        if (light->GetType() == LightType::Directional &&
            light->GetShadowTechnique() == ShadowTechnique::CSM)
        {
            if (!m_csmShadowMap)
            {
                if (FAILED(CreateCascadedShadowMap()))
                {
                    Spark::SimpleConsole::GetInstance().LogWarning(
                        "Failed to create cascaded shadow map");
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

                XMMATRIX cascadeLightMatrix = CalculateLightMatrix(
                    *light, viewMatrix, cascadeNear, cascadeFar);
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
            it->second->lightMatrix = CalculateLightMatrix(
                *light, viewMatrix, projNear, projFar);
            it->second->shadowMatrix = light->GetShadowMatrix();
        }
    }
}

void LightingSystem::CullLights(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix)
{
    // Build the six frustum planes from the view-projection matrix
    // Planes are extracted from the combined VP matrix in clip space
    XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);

    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, viewProj);

    // Extract frustum planes (Gribb/Hartmann method)
    // Each plane as (A, B, C, D) where Ax + By + Cz + D >= 0 means inside
    XMFLOAT4 planes[6];

    // Left plane
    planes[0] = XMFLOAT4(
        vp._14 + vp._11,
        vp._24 + vp._21,
        vp._34 + vp._31,
        vp._44 + vp._41);

    // Right plane
    planes[1] = XMFLOAT4(
        vp._14 - vp._11,
        vp._24 - vp._21,
        vp._34 - vp._31,
        vp._44 - vp._41);

    // Bottom plane
    planes[2] = XMFLOAT4(
        vp._14 + vp._12,
        vp._24 + vp._22,
        vp._34 + vp._32,
        vp._44 + vp._42);

    // Top plane
    planes[3] = XMFLOAT4(
        vp._14 - vp._12,
        vp._24 - vp._22,
        vp._34 - vp._32,
        vp._44 - vp._42);

    // Near plane
    planes[4] = XMFLOAT4(
        vp._13,
        vp._23,
        vp._33,
        vp._43);

    // Far plane
    planes[5] = XMFLOAT4(
        vp._14 - vp._13,
        vp._24 - vp._23,
        vp._34 - vp._33,
        vp._44 - vp._43);

    // Normalize all planes
    for (int i = 0; i < 6; ++i)
    {
        XMVECTOR planeVec = XMLoadFloat4(&planes[i]);
        XMVECTOR normalLength = XMVector3Length(
            XMVectorSet(planes[i].x, planes[i].y, planes[i].z, 0.0f));
        float len = XMVectorGetX(normalLength);
        if (len > 0.0f)
        {
            planes[i].x /= len;
            planes[i].y /= len;
            planes[i].z /= len;
            planes[i].w /= len;
        }
    }

    // Rebuild light data array with only visible lights
    m_lightDataArray.clear();
    m_lightDataArray.reserve(m_lights.size());
    uint32_t visibleCount = 0;

    for (const auto& light : m_lights)
    {
        if (!light || !light->IsEnabled())
        {
            continue;
        }

        bool visible = true;

        // Directional and environment lights are always visible
        if (light->GetType() == LightType::Point ||
            light->GetType() == LightType::Spot ||
            light->GetType() == LightType::Area)
        {
            const XMFLOAT3& pos = light->GetPosition();
            float range = light->GetRange();

            // Test the light's bounding sphere against each frustum plane
            for (int i = 0; i < 6; ++i)
            {
                float distance = planes[i].x * pos.x +
                                 planes[i].y * pos.y +
                                 planes[i].z * pos.z +
                                 planes[i].w;

                // If the sphere is entirely behind any plane, cull it
                if (distance < -range)
                {
                    visible = false;
                    break;
                }
            }
        }

        if (visible)
        {
            m_lightDataArray.push_back(light->GetShaderData());
            visibleCount++;
        }
    }

    m_metrics.visibleLights = visibleCount;
    m_metrics.culledLights = m_metrics.activeLights - visibleCount;
}

void LightingSystem::CalculateCSMSplits(float nearPlane, float farPlane, CascadedShadowMap& csm)
{
    csm.splitDistances.clear();
    csm.splitDistances.resize(csm.cascadeCount + 1);

    for (uint32_t i = 0; i < csm.cascadeCount; ++i)
    {
        float p = static_cast<float>(i + 1) / static_cast<float>(csm.cascadeCount);
        float log = nearPlane * std::pow(farPlane / nearPlane, p);
        float uniform = nearPlane + (farPlane - nearPlane) * p;
        float d = csm.splitLambda * (log - uniform) + uniform;
        csm.splitDistances[i + 1] = d;
    }

    csm.splitDistances[0] = nearPlane;
}

XMMATRIX LightingSystem::CalculateLightMatrix(const Light& light, const XMMATRIX& viewMatrix, float nearPlane,
                                              float farPlane)
{
    // For non-directional lights, use the basic light matrix
    if (light.GetType() != LightType::Directional)
    {
        return light.GetLightMatrix();
    }

    // For directional lights, compute a tight-fitting orthographic projection
    // that encompasses the view frustum slice [nearPlane, farPlane].

    // Step 1: Build the inverse view-projection for the frustum slice
    // Use a temporary projection matrix matching the slice
    // We assume a symmetric perspective projection; extract aspect and fov
    // from the current projection indirectly, but we only need the 8 frustum
    // corners in world space. We reconstruct them from the inverse view matrix
    // and the near/far distances with a standard FPS FOV.

    // Compute the 8 corners of the frustum slice in world space
    // NDC corners: x,y in {-1,+1}, z in {0,1} (LH)
    XMMATRIX invView = XMMatrixInverse(nullptr, viewMatrix);

    // Extract forward, right, up from the inverse view matrix
    XMVECTOR camRight = invView.r[0];
    XMVECTOR camUp = invView.r[1];
    XMVECTOR camForward = invView.r[2];
    XMVECTOR camPos = invView.r[3];

    // Use a default FOV of 60 degrees and 16:9 aspect if we cannot extract them
    float fovY = XM_PI / 3.0f;
    float aspect = 16.0f / 9.0f;

    float nearHeight = 2.0f * nearPlane * std::tan(fovY * 0.5f);
    float nearWidth = nearHeight * aspect;
    float farHeight = 2.0f * farPlane * std::tan(fovY * 0.5f);
    float farWidth = farHeight * aspect;

    XMVECTOR nearCenter = XMVectorAdd(camPos, XMVectorScale(camForward, nearPlane));
    XMVECTOR farCenter = XMVectorAdd(camPos, XMVectorScale(camForward, farPlane));

    // Compute the 8 frustum corners
    XMVECTOR frustumCorners[8];
    // Near plane corners
    frustumCorners[0] = XMVectorAdd(nearCenter, XMVectorAdd(XMVectorScale(camUp, nearHeight * 0.5f), XMVectorScale(camRight, -nearWidth * 0.5f)));
    frustumCorners[1] = XMVectorAdd(nearCenter, XMVectorAdd(XMVectorScale(camUp, nearHeight * 0.5f), XMVectorScale(camRight, nearWidth * 0.5f)));
    frustumCorners[2] = XMVectorAdd(nearCenter, XMVectorAdd(XMVectorScale(camUp, -nearHeight * 0.5f), XMVectorScale(camRight, -nearWidth * 0.5f)));
    frustumCorners[3] = XMVectorAdd(nearCenter, XMVectorAdd(XMVectorScale(camUp, -nearHeight * 0.5f), XMVectorScale(camRight, nearWidth * 0.5f)));
    // Far plane corners
    frustumCorners[4] = XMVectorAdd(farCenter, XMVectorAdd(XMVectorScale(camUp, farHeight * 0.5f), XMVectorScale(camRight, -farWidth * 0.5f)));
    frustumCorners[5] = XMVectorAdd(farCenter, XMVectorAdd(XMVectorScale(camUp, farHeight * 0.5f), XMVectorScale(camRight, farWidth * 0.5f)));
    frustumCorners[6] = XMVectorAdd(farCenter, XMVectorAdd(XMVectorScale(camUp, -farHeight * 0.5f), XMVectorScale(camRight, -farWidth * 0.5f)));
    frustumCorners[7] = XMVectorAdd(farCenter, XMVectorAdd(XMVectorScale(camUp, -farHeight * 0.5f), XMVectorScale(camRight, farWidth * 0.5f)));

    // Step 2: Compute the frustum centroid
    XMVECTOR centroid = XMVectorZero();
    for (int i = 0; i < 8; ++i)
    {
        centroid = XMVectorAdd(centroid, frustumCorners[i]);
    }
    centroid = XMVectorScale(centroid, 1.0f / 8.0f);

    // Step 3: Build the light view matrix looking at the centroid
    const XMFLOAT3& lightDir = light.GetDirection();
    XMVECTOR lightDirVec = XMVector3Normalize(XMLoadFloat3(&lightDir));

    // Place the light far enough back along the light direction
    XMVECTOR lightPos = XMVectorSubtract(centroid, XMVectorScale(lightDirVec, farPlane));

    XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    // If light direction is nearly parallel to up, choose a different up vector
    if (std::abs(XMVectorGetX(XMVector3Dot(lightDirVec, lightUp))) > 0.99f)
    {
        lightUp = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    }

    XMVECTOR lightTarget = XMVectorAdd(lightPos, lightDirVec);
    XMMATRIX lightView = XMMatrixLookAtLH(lightPos, lightTarget, lightUp);

    // Step 4: Transform frustum corners into light space and find AABB
    float minX = FLT_MAX, maxX = -FLT_MAX;
    float minY = FLT_MAX, maxY = -FLT_MAX;
    float minZ = FLT_MAX, maxZ = -FLT_MAX;

    for (int i = 0; i < 8; ++i)
    {
        XMVECTOR transformed = XMVector3TransformCoord(frustumCorners[i], lightView);
        float tx = XMVectorGetX(transformed);
        float ty = XMVectorGetY(transformed);
        float tz = XMVectorGetZ(transformed);

        minX = std::min(minX, tx);
        maxX = std::max(maxX, tx);
        minY = std::min(minY, ty);
        maxY = std::max(maxY, ty);
        minZ = std::min(minZ, tz);
        maxZ = std::max(maxZ, tz);
    }

    // Expand the Z range to capture shadow casters behind the camera frustum
    float zRange = maxZ - minZ;
    minZ -= zRange * 0.5f;

    // Step 5: Build an orthographic projection from the light-space AABB
    XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(minX, maxX, minY, maxY, minZ, maxZ);

    return XMMatrixMultiply(lightView, lightProj);
}

HRESULT LightingSystem::GenerateIrradianceMap(ID3D11ShaderResourceView* environmentMap)
{
    if (!m_device || !m_context)
        return E_FAIL;

    // If no environment map is provided, create a default solid-color irradiance cubemap
    const UINT irradianceSize = 32; // Low-res is fine for diffuse irradiance
    const UINT numFaces = 6;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = irradianceSize;
    texDesc.Height = irradianceSize;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = numFaces;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    // Initialize with sky color as a uniform irradiance approximation
    std::vector<uint16_t> faceData(irradianceSize * irradianceSize * 4);
    auto floatToHalf = [](float f) -> uint16_t
    {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        uint32_t sign = (bits >> 16) & 0x8000;
        int32_t exp = ((bits >> 23) & 0xFF) - 127 + 15;
        uint32_t mantissa = bits & 0x007FFFFF;
        if (exp <= 0)
            return static_cast<uint16_t>(sign);
        if (exp >= 31)
            return static_cast<uint16_t>(sign | 0x7C00);
        return static_cast<uint16_t>(sign | (exp << 10) | (mantissa >> 13));
    };

    // Fill with sky color scaled by PI (Lambertian diffuse integral)
    float skyR = m_environmentLighting.skyColor.x * m_environmentLighting.skyIntensity * 3.14159f;
    float skyG = m_environmentLighting.skyColor.y * m_environmentLighting.skyIntensity * 3.14159f;
    float skyB = m_environmentLighting.skyColor.z * m_environmentLighting.skyIntensity * 3.14159f;
    for (UINT i = 0; i < irradianceSize * irradianceSize; ++i)
    {
        faceData[i * 4 + 0] = floatToHalf(skyR);
        faceData[i * 4 + 1] = floatToHalf(skyG);
        faceData[i * 4 + 2] = floatToHalf(skyB);
        faceData[i * 4 + 3] = floatToHalf(1.0f);
    }

    D3D11_SUBRESOURCE_DATA initData[6];
    for (UINT face = 0; face < numFaces; ++face)
    {
        initData[face].pSysMem = faceData.data();
        initData[face].SysMemPitch = irradianceSize * 4 * sizeof(uint16_t);
        initData[face].SysMemSlicePitch = 0;
    }

    ComPtr<ID3D11Texture2D> irradianceTex;
    HRESULT hr = m_device->CreateTexture2D(&texDesc, initData, &irradianceTex);
    if (FAILED(hr))
        return hr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;

    hr = m_device->CreateShaderResourceView(irradianceTex.Get(), &srvDesc, &m_environmentLighting.irradianceMap);
    if (FAILED(hr))
        return hr;

    Spark::SimpleConsole::GetInstance().LogSuccess("Irradiance map generated (" + std::to_string(irradianceSize) + "x" +
                                                   std::to_string(irradianceSize) + " cubemap)");
    return S_OK;
}

HRESULT LightingSystem::GeneratePrefilterMap(ID3D11ShaderResourceView* environmentMap)
{
    if (!m_device || !m_context)
        return E_FAIL;

    // Create a pre-filtered environment cubemap with mip chain for roughness levels
    const UINT prefilterSize = 128;
    const UINT mipLevels = 5; // roughness 0.0, 0.25, 0.5, 0.75, 1.0
    const UINT numFaces = 6;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = prefilterSize;
    texDesc.Height = prefilterSize;
    texDesc.MipLevels = mipLevels;
    texDesc.ArraySize = numFaces;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    auto floatToHalf = [](float f) -> uint16_t
    {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        uint32_t sign = (bits >> 16) & 0x8000;
        int32_t exp = ((bits >> 23) & 0xFF) - 127 + 15;
        uint32_t mantissa = bits & 0x007FFFFF;
        if (exp <= 0)
            return static_cast<uint16_t>(sign);
        if (exp >= 31)
            return static_cast<uint16_t>(sign | 0x7C00);
        return static_cast<uint16_t>(sign | (exp << 10) | (mantissa >> 13));
    };

    // Build subresource data for each face and mip level
    // Higher mips represent rougher surfaces (more blurred sky color)
    std::vector<std::vector<uint16_t>> mipData(mipLevels * numFaces);
    std::vector<D3D11_SUBRESOURCE_DATA> initData(mipLevels * numFaces);

    for (UINT mip = 0; mip < mipLevels; ++mip)
    {
        UINT mipSize = std::max(1U, prefilterSize >> mip);
        float roughness = static_cast<float>(mip) / static_cast<float>(mipLevels - 1);
        // Blend sky color toward a muted average as roughness increases
        float intensity = m_environmentLighting.skyIntensity * (1.0f - roughness * 0.5f);

        for (UINT face = 0; face < numFaces; ++face)
        {
            UINT subresource = face * mipLevels + mip;
            auto& data = mipData[subresource];
            data.resize(mipSize * mipSize * 4);

            for (UINT i = 0; i < mipSize * mipSize; ++i)
            {
                data[i * 4 + 0] = floatToHalf(m_environmentLighting.skyColor.x * intensity);
                data[i * 4 + 1] = floatToHalf(m_environmentLighting.skyColor.y * intensity);
                data[i * 4 + 2] = floatToHalf(m_environmentLighting.skyColor.z * intensity);
                data[i * 4 + 3] = floatToHalf(1.0f);
            }

            initData[subresource].pSysMem = data.data();
            initData[subresource].SysMemPitch = mipSize * 4 * sizeof(uint16_t);
            initData[subresource].SysMemSlicePitch = 0;
        }
    }

    ComPtr<ID3D11Texture2D> prefilterTex;
    HRESULT hr = m_device->CreateTexture2D(&texDesc, initData.data(), &prefilterTex);
    if (FAILED(hr))
        return hr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = mipLevels;

    hr = m_device->CreateShaderResourceView(prefilterTex.Get(), &srvDesc, &m_environmentLighting.prefilterMap);
    if (FAILED(hr))
        return hr;

    Spark::SimpleConsole::GetInstance().LogSuccess("Prefilter map generated (" + std::to_string(prefilterSize) + "x" +
                                                   std::to_string(prefilterSize) + ", " + std::to_string(mipLevels) +
                                                   " mip levels)");
    return S_OK;
}

HRESULT LightingSystem::GenerateBRDFLUT()
{
    if (!m_device || !m_context)
        return E_FAIL;

    // Generate a 2D BRDF integration lookup texture
    // x-axis = NdotV (cos theta), y-axis = roughness
    // Output: (scale, bias) for split-sum approximation F0*scale + bias
    const UINT lutSize = 256;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = lutSize;
    texDesc.Height = lutSize;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    auto floatToHalf = [](float f) -> uint16_t
    {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        uint32_t sign = (bits >> 16) & 0x8000;
        int32_t exp = ((bits >> 23) & 0xFF) - 127 + 15;
        uint32_t mantissa = bits & 0x007FFFFF;
        if (exp <= 0)
            return static_cast<uint16_t>(sign);
        if (exp >= 31)
            return static_cast<uint16_t>(sign | 0x7C00);
        return static_cast<uint16_t>(sign | (exp << 10) | (mantissa >> 13));
    };

    // CPU-side BRDF integration using importance sampling of GGX
    std::vector<uint16_t> lutData(lutSize * lutSize * 2);
    const UINT sampleCount = 1024;

    for (UINT y = 0; y < lutSize; ++y)
    {
        float roughness = (static_cast<float>(y) + 0.5f) / static_cast<float>(lutSize);
        float alpha = roughness * roughness;

        for (UINT x = 0; x < lutSize; ++x)
        {
            float NdotV = (static_cast<float>(x) + 0.5f) / static_cast<float>(lutSize);
            NdotV = std::max(NdotV, 0.001f);

            float scale = 0.0f;
            float bias = 0.0f;

            // Importance sample GGX
            for (UINT i = 0; i < sampleCount; ++i)
            {
                // Hammersley sequence
                float u1 = static_cast<float>(i) / static_cast<float>(sampleCount);
                uint32_t bits2 = i;
                bits2 = (bits2 << 16u) | (bits2 >> 16u);
                bits2 = ((bits2 & 0x55555555u) << 1u) | ((bits2 & 0xAAAAAAAAu) >> 1u);
                bits2 = ((bits2 & 0x33333333u) << 2u) | ((bits2 & 0xCCCCCCCCu) >> 2u);
                bits2 = ((bits2 & 0x0F0F0F0Fu) << 4u) | ((bits2 & 0xF0F0F0F0u) >> 4u);
                bits2 = ((bits2 & 0x00FF00FFu) << 8u) | ((bits2 & 0xFF00FF00u) >> 8u);
                float u2 = static_cast<float>(bits2) * 2.3283064365386963e-10f;

                // GGX importance sampling
                float phi = 2.0f * 3.14159265f * u1;
                float cosTheta = std::sqrt((1.0f - u2) / (1.0f + (alpha * alpha - 1.0f) * u2));
                float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);

                // Half vector in tangent space
                float Hx = sinTheta * std::cos(phi);
                float Hy = sinTheta * std::sin(phi);
                float Hz = cosTheta;

                // View vector (tangent space, V = (sqrt(1 - NdotV^2), 0, NdotV))
                float Vx = std::sqrt(1.0f - NdotV * NdotV);
                float Vy = 0.0f;
                float Vz = NdotV;

                // Light vector L = 2 * dot(V, H) * H - V
                float VdotH = Vx * Hx + Vy * Hy + Vz * Hz;
                float Lx = 2.0f * VdotH * Hx - Vx;
                float Ly = 2.0f * VdotH * Hy - Vy;
                float Lz = 2.0f * VdotH * Hz - Vz;

                float NdotL = std::max(Lz, 0.0f);
                float NdotH = std::max(Hz, 0.0f);
                VdotH = std::max(VdotH, 0.0f);

                if (NdotL > 0.0f)
                {
                    // Smith's Schlick-GGX geometry function
                    float k = alpha / 2.0f;
                    float G_V = NdotV / (NdotV * (1.0f - k) + k);
                    float G_L = NdotL / (NdotL * (1.0f - k) + k);
                    float G = G_V * G_L;

                    float G_Vis = (G * VdotH) / (NdotH * NdotV);
                    float Fc = std::pow(1.0f - VdotH, 5.0f);

                    scale += (1.0f - Fc) * G_Vis;
                    bias += Fc * G_Vis;
                }
            }

            scale /= static_cast<float>(sampleCount);
            bias /= static_cast<float>(sampleCount);

            UINT idx = (y * lutSize + x) * 2;
            lutData[idx + 0] = floatToHalf(scale);
            lutData[idx + 1] = floatToHalf(bias);
        }
    }

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = lutData.data();
    initData.SysMemPitch = lutSize * 2 * sizeof(uint16_t);

    ComPtr<ID3D11Texture2D> brdfTex;
    HRESULT hr = m_device->CreateTexture2D(&texDesc, &initData, &brdfTex);
    if (FAILED(hr))
        return hr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = m_device->CreateShaderResourceView(brdfTex.Get(), &srvDesc, &m_environmentLighting.brdfLUT);
    if (FAILED(hr))
        return hr;

    Spark::SimpleConsole::GetInstance().LogSuccess("BRDF LUT generated (" + std::to_string(lutSize) + "x" +
                                                   std::to_string(lutSize) + ", " + std::to_string(sampleCount) +
                                                   " samples)");
    return S_OK;
}

HRESULT LightingSystem::CreateDefaultEnvironment()
{
    // Set up default environment lighting parameters
    m_environmentLighting.skyColor = {0.5f, 0.7f, 1.0f};
    m_environmentLighting.skyIntensity = 1.0f;
    m_environmentLighting.skyTurbidity = 2.0f;
    m_environmentLighting.sunDirection = {0.3f, 0.7f, 0.2f};
    m_environmentLighting.sunSize = 0.04f;
    m_environmentLighting.sunIntensity = 5.0f;

    // Default fog settings (disabled)
    m_environmentLighting.fogEnabled = false;
    m_environmentLighting.fogColor = {0.5f, 0.6f, 0.7f};
    m_environmentLighting.fogDensity = 0.01f;
    m_environmentLighting.fogStart = 10.0f;
    m_environmentLighting.fogEnd = 100.0f;

    // Generate default IBL textures from the sky color
    HRESULT hr = GenerateIrradianceMap(nullptr);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogWarning("Failed to generate default irradiance map");
        return hr;
    }

    hr = GeneratePrefilterMap(nullptr);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogWarning("Failed to generate default prefilter map");
        return hr;
    }

    hr = GenerateBRDFLUT();
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogWarning("Failed to generate default BRDF LUT");
        return hr;
    }

    Spark::SimpleConsole::GetInstance().LogSuccess("Default environment created with IBL textures");
    return S_OK;
}

// ============================================================================
// UTILITY FUNCTIONS
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
    if (str == "directional")
        return LightType::Directional;
    if (str == "point")
        return LightType::Point;
    if (str == "spot")
        return LightType::Spot;
    if (str == "area")
        return LightType::Area;
    if (str == "environment")
        return LightType::Environment;
    return LightType::Directional; // Default
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
    if (str == "none")
        return ShadowTechnique::None;
    if (str == "basic")
        return ShadowTechnique::Basic;
    if (str == "pcf")
        return ShadowTechnique::PCF;
    if (str == "vsm")
        return ShadowTechnique::VSM;
    if (str == "csm")
        return ShadowTechnique::CSM;
    if (str == "pcss")
        return ShadowTechnique::PCSS;
    return ShadowTechnique::PCF; // Default
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "LightingSystem.h"
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
    data.direction = XMFLOAT4(m_direction.x, m_direction.y, m_direction.z, m_spotAngle * 3.14159f / 180.0f);
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
    m_device = device;
    m_context = context;
    m_environmentLighting.skyColor = {0.5f, 0.7f, 1.0f};
    m_environmentLighting.skyIntensity = 1.0f;
    return S_OK;
}

void LightingSystem::Shutdown()
{
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
HRESULT LightingSystem::CreateConstantBuffers()
{
    return S_OK;
}
HRESULT LightingSystem::CreateShadowMap(uint32_t /*size*/, ShadowMap& /*shadowMap*/)
{
    return S_OK;
}
HRESULT LightingSystem::CreateCascadedShadowMap()
{
    return S_OK;
}
void LightingSystem::UpdateLightBuffer() {}
void LightingSystem::UpdateShadowMaps(const XMMATRIX& /*viewMatrix*/, const XMMATRIX& /*projMatrix*/) {}
void LightingSystem::CullLights(const XMMATRIX& /*viewMatrix*/, const XMMATRIX& /*projMatrix*/) {}
void LightingSystem::CalculateCSMSplits(float /*nearPlane*/, float /*farPlane*/, CascadedShadowMap& /*csm*/) {}
XMMATRIX LightingSystem::CalculateLightMatrix(const Light& /*light*/, const XMMATRIX& /*viewMatrix*/,
                                              float /*nearPlane*/, float /*farPlane*/)
{
    XMMATRIX m;
    memset(&m, 0, sizeof(m));
    return m;
}
HRESULT LightingSystem::GenerateIrradianceMap(ID3D11ShaderResourceView* /*environmentMap*/)
{
    return S_OK;
}
HRESULT LightingSystem::GeneratePrefilterMap(ID3D11ShaderResourceView* /*environmentMap*/)
{
    return S_OK;
}
HRESULT LightingSystem::GenerateBRDFLUT()
{
    return S_OK;
}
HRESULT LightingSystem::CreateDefaultEnvironment()
{
    return S_OK;
}

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
    if (str == "directional")
        return LightType::Directional;
    if (str == "point")
        return LightType::Point;
    if (str == "spot")
        return LightType::Spot;
    if (str == "area")
        return LightType::Area;
    if (str == "environment")
        return LightType::Environment;
    return LightType::Directional;
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
    if (str == "none")
        return ShadowTechnique::None;
    if (str == "basic")
        return ShadowTechnique::Basic;
    if (str == "pcf")
        return ShadowTechnique::PCF;
    if (str == "vsm")
        return ShadowTechnique::VSM;
    if (str == "csm")
        return ShadowTechnique::CSM;
    if (str == "pcss")
        return ShadowTechnique::PCSS;
    return ShadowTechnique::PCF;
}

#endif // SPARK_PLATFORM_WINDOWS
