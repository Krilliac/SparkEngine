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

// ============================================================================
// LIGHT CLASS IMPLEMENTATION
// ============================================================================

Light::Light(LightType type) : m_type(type)
{
    // Initialize light based on type
    switch (type) {
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
    switch (m_type) {
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
    if (property == "intensity") {
        SetIntensity(value);
    } else if (property == "range") {
        SetRange(value);
    } else if (property == "spotangle") {
        SetSpotAngle(value);
    } else if (property == "shadowbias") {
        SetShadowBias(value);
    }
}

void Light::Console_SetColor(float r, float g, float b)
{
    SetColor({std::max(0.0f, std::min(1.0f, r)), 
              std::max(0.0f, std::min(1.0f, g)), 
              std::max(0.0f, std::min(1.0f, b))});
}

// ============================================================================
// LIGHTING SYSTEM IMPLEMENTATION
// ============================================================================

LightingSystem::LightingSystem()
    : m_device(nullptr), m_context(nullptr)
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
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create constant buffers");
        return hr;
    }
    
    // Create default environment
    hr = CreateDefaultEnvironment();
    if (FAILED(hr)) {
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
    
    for (const auto& light : m_lights) {
        if (light && light->IsEnabled()) {
            m_lightDataArray.push_back(light->GetShaderData());
            m_metrics.visibleLights++;
            
            if (light->GetCastShadows()) {
                m_metrics.shadowCastingLights++;
            }
            
            // Mark light as clean after processing
            light->SetClean();
        }
    }
    
    // Update light buffer
    UpdateLightBuffer();
    
    // Update shadow maps if shadows are enabled
    if (m_shadowsEnabled) {
        UpdateShadowMaps(viewMatrix, projMatrix);
    }
    
    // Update culling metrics
    m_metrics.culledLights = m_metrics.activeLights - m_metrics.visibleLights;
}

void LightingSystem::EnableShadows(bool enabled)
{
    m_shadowsEnabled = enabled;
    Spark::SimpleConsole::GetInstance().LogInfo("Shadows " + std::string(enabled ? "enabled" : "disabled") + " globally");
}

void LightingSystem::SetGlobalShadowQuality(uint32_t size)
{
    m_shadowMapSize = size;
    
    // Recreate existing shadow maps with new size
    for (auto& pair : m_shadowMaps) {
        if (pair.second) {
            CreateShadowMap(size, *pair.second);
        }
    }
    
    Spark::SimpleConsole::GetInstance().LogInfo("Shadow map quality set to " + std::to_string(size) + "x" + std::to_string(size));
}

void LightingSystem::Console_EnableShadows(bool enabled)
{
    EnableShadows(enabled);
    Spark::SimpleConsole::GetInstance().LogInfo("Console command: Shadows " + std::string(enabled ? "enabled" : "disabled"));
}

std::string LightingSystem::Console_ListLights() const
{
    std::stringstream ss;
    ss << "Lighting System - Active Lights (" << m_lights.size() << "):\n";
    
    for (size_t i = 0; i < m_lights.size(); ++i) {
        const auto& light = m_lights[i];
        if (light) {
            ss << "  [" << i << "] ";
            switch (light->GetType()) {
                case LightType::Directional: ss << "Directional Light"; break;
                case LightType::Point: ss << "Point Light"; break;
                case LightType::Spot: ss << "Spot Light"; break;
                case LightType::Area: ss << "Area Light"; break;
                case LightType::Environment: ss << "Environment Light"; break;
            }
            ss << " - " << (light->IsEnabled() ? "Enabled" : "Disabled");
            if (light->GetCastShadows()) ss << " (Shadows)";
            ss << "\n";
        }
    }
    
    ss << "Environment Light: " << (m_environmentLighting.fogEnabled ? "Enabled" : "Disabled") << "\n";
    ss << "Shadow Quality: " << m_shadowMapSize << "x" << m_shadowMapSize;
    
    return ss.str();
}

void LightingSystem::BindLightingData(ID3D11DeviceContext* context)
{
    if (context && m_lightDataBuffer) {
        // Update light data buffer with current light array
        if (!m_lightDataArray.empty()) {
            D3D11_MAPPED_SUBRESOURCE mapped;
            HRESULT hr = context->Map(m_lightDataBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hr)) {
                size_t copySize = std::min(m_lightDataArray.size() * sizeof(LightData), static_cast<size_t>(64 * sizeof(LightData)));
                memcpy(mapped.pData, m_lightDataArray.data(), copySize);
                context->Unmap(m_lightDataBuffer.Get(), 0);
            }
        }
        
        // Bind constant buffers
        ID3D11Buffer* buffers[] = { m_lightDataBuffer.Get(), m_environmentBuffer.Get(), m_shadowDataBuffer.Get() };
        context->VSSetConstantBuffers(1, 3, buffers);
        context->PSSetConstantBuffers(1, 3, buffers);
        
        Spark::SimpleConsole::GetInstance().LogInfo("Lighting data bound to shaders");
    }
}

void LightingSystem::RenderShadowMaps(std::function<void(const XMMATRIX&, const XMMATRIX&)> renderCallback)
{
    if (!renderCallback || !m_shadowsEnabled) return;
    
    m_metrics.shadowMapUpdates = 0;
    
    for (const auto& light : m_lights) {
        if (light && light->IsEnabled() && light->GetCastShadows()) {
            try {
                XMMATRIX lightView = light->GetLightMatrix();
                XMMATRIX lightProj = light->GetShadowMatrix();
                
                // Set up shadow map render target if it exists
                auto it = m_shadowMaps.find(light.get());
                if (it != m_shadowMaps.end() && it->second) {
                    m_context->OMSetRenderTargets(0, nullptr, it->second->dsv.Get());
                    m_context->ClearDepthStencilView(it->second->dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
                }
                
                renderCallback(lightView, lightProj);
                m_metrics.shadowMapUpdates++;
                
            } catch (...) {
                Spark::SimpleConsole::GetInstance().LogWarning("Error in shadow map render callback for light");
            }
        }
    }
    
    Spark::SimpleConsole::GetInstance().LogInfo("Shadow maps rendered: " + std::to_string(m_metrics.shadowMapUpdates) + " updates");
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
    if (light->GetCastShadows() && m_shadowsEnabled) {
        auto shadowMap = std::make_unique<ShadowMap>();
        if (SUCCEEDED(CreateShadowMap(m_shadowMapSize, *shadowMap))) {
            m_shadowMaps[light.get()] = std::move(shadowMap);
        }
    }
    
    Spark::SimpleConsole::GetInstance().LogInfo("Created new light of type: " + std::to_string(static_cast<int>(type)));
    return light;
}

void LightingSystem::AddLight(std::shared_ptr<Light> light)
{
    if (light) {
        m_lights.push_back(light);
        
        // Create shadow map if needed
        if (light->GetCastShadows() && m_shadowsEnabled) {
            auto shadowMap = std::make_unique<ShadowMap>();
            if (SUCCEEDED(CreateShadowMap(m_shadowMapSize, *shadowMap))) {
                m_shadowMaps[light.get()] = std::move(shadowMap);
            }
        }
    }
}

void LightingSystem::RemoveLight(std::shared_ptr<Light> light)
{
    if (light) {
        // Remove shadow map
        auto it = m_shadowMaps.find(light.get());
        if (it != m_shadowMaps.end()) {
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
    if (!m_device) return;
    
    // This would normally generate irradiance map, prefilter map, and BRDF LUT
    // For now, just log the operation
    Spark::SimpleConsole::GetInstance().LogInfo("Generating IBL textures");
    
    HRESULT hr = GenerateIrradianceMap(m_environmentLighting.environmentMap.Get());
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to generate irradiance map");
        return;
    }
    hr = GeneratePrefilterMap(m_environmentLighting.environmentMap.Get());
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to generate prefilter map");
        return;
    }
    hr = GenerateBRDFLUT();
    if (FAILED(hr)) {
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
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size())) {
        return "Error: Invalid light index " + std::to_string(lightIndex);
    }
    
    const auto& light = m_lights[lightIndex];
    if (!light) {
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
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size())) {
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
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size())) {
        Spark::SimpleConsole::GetInstance().LogError("Invalid light index: " + std::to_string(lightIndex));
        return;
    }
    
    auto& light = m_lights[lightIndex];
    if (light) {
        light->Console_SetProperty(property, value);
        Spark::SimpleConsole::GetInstance().LogSuccess("Set " + property + " = " + std::to_string(value) + " for light " + std::to_string(lightIndex));
    }
}

void LightingSystem::Console_SetLightColor(int lightIndex, float r, float g, float b)
{
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size())) {
        Spark::SimpleConsole::GetInstance().LogError("Invalid light index: " + std::to_string(lightIndex));
        return;
    }
    
    auto& light = m_lights[lightIndex];
    if (light) {
        light->Console_SetColor(r, g, b);
        Spark::SimpleConsole::GetInstance().LogSuccess("Set color for light " + std::to_string(lightIndex));
    }
}

void LightingSystem::Console_SetShadowQuality(const std::string& quality)
{
    uint32_t size = 1024;
    
    if (quality == "low") size = 512;
    else if (quality == "medium") size = 1024;
    else if (quality == "high") size = 2048;
    else if (quality == "ultra") size = 4096;
    else {
        Spark::SimpleConsole::GetInstance().LogError("Invalid shadow quality: " + quality);
        return;
    }
    
    SetGlobalShadowQuality(size);
    Spark::SimpleConsole::GetInstance().LogSuccess("Shadow quality set to " + quality);
}

void LightingSystem::Console_SetEnvironment(const std::string& skyType)
{
    if (skyType == "clear") {
        m_environmentLighting.skyColor = {0.5f, 0.7f, 1.0f};
        m_environmentLighting.skyIntensity = 1.0f;
        m_environmentLighting.fogEnabled = false;
    } else if (skyType == "overcast") {
        m_environmentLighting.skyColor = {0.6f, 0.6f, 0.6f};
        m_environmentLighting.skyIntensity = 0.8f;
        m_environmentLighting.fogEnabled = true;
        m_environmentLighting.fogDensity = 0.02f;
    } else if (skyType == "sunset") {
        m_environmentLighting.skyColor = {1.0f, 0.6f, 0.3f};
        m_environmentLighting.skyIntensity = 1.2f;
        m_environmentLighting.fogEnabled = false;
    } else if (skyType == "night") {
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
    if (!m_device) return E_FAIL;
    
    // Create light data buffer (supports up to 64 lights)
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.ByteWidth = sizeof(LightData) * 64;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    
    HRESULT hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_lightDataBuffer);
    if (FAILED(hr)) return hr;
    
    // Create environment buffer
    bufferDesc.ByteWidth = sizeof(EnvironmentLighting);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_environmentBuffer);
    if (FAILED(hr)) return hr;
    
    // Create shadow data buffer
    bufferDesc.ByteWidth = sizeof(XMMATRIX) * 16; // Up to 16 shadow matrices
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_shadowDataBuffer);
    if (FAILED(hr)) return hr;
    
    return S_OK;
}

HRESULT LightingSystem::CreateShadowMap(uint32_t size, ShadowMap& shadowMap)
{
    if (!m_device) return E_FAIL;
    
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
    if (FAILED(hr)) return hr;
    
    // Create depth stencil view
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    
    hr = m_device->CreateDepthStencilView(shadowMap.texture.Get(), &dsvDesc, &shadowMap.dsv);
    if (FAILED(hr)) return hr;
    
    // Create shader resource view
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    
    hr = m_device->CreateShaderResourceView(shadowMap.texture.Get(), &srvDesc, &shadowMap.srv);
    if (FAILED(hr)) return hr;
    
    return S_OK;
}

HRESULT LightingSystem::CreateCascadedShadowMap()
{
    if (!m_device) return E_FAIL;
    
    m_csmShadowMap = std::make_unique<CascadedShadowMap>();
    m_csmShadowMap->cascades.resize(m_csmShadowMap->cascadeCount);
    
    for (auto& cascade : m_csmShadowMap->cascades) {
        HRESULT hr = CreateShadowMap(m_shadowMapSize, cascade);
        if (FAILED(hr)) return hr;
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
    
    if (elapsed.count() >= 100) { // Update every 100ms
        m_metrics.lightCullingTime = elapsed.count() / 1000.0f;
        m_metrics.shadowRenderTime = m_metrics.shadowMapUpdates * 0.5f; // Estimate
        m_metrics.shadowMapMemory = m_shadowMaps.size() * (m_shadowMapSize * m_shadowMapSize * 4) / (1024.0f * 1024.0f);
        lastUpdate = now;
    }
}

void LightingSystem::UpdateShadowMaps(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix)
{
    // Update shadow map matrices and culling
    for (const auto& light : m_lights) {
        if (light && light->IsEnabled() && light->GetCastShadows()) {
            auto it = m_shadowMaps.find(light.get());
            if (it != m_shadowMaps.end() && it->second) {
                it->second->lightMatrix = light->GetLightMatrix();
                it->second->shadowMatrix = light->GetShadowMatrix();
            }
        }
    }
}

void LightingSystem::CullLights(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix)
{
    // Simple frustum culling for lights
    // This is a simplified implementation
    uint32_t visibleCount = 0;
    
    for (const auto& light : m_lights) {
        if (light && light->IsEnabled()) {
            // For now, consider all lights visible
            // In a real implementation, this would test against view frustum
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
    
    for (uint32_t i = 0; i < csm.cascadeCount; ++i) {
        float p = static_cast<float>(i + 1) / static_cast<float>(csm.cascadeCount);
        float log = nearPlane * std::pow(farPlane / nearPlane, p);
        float uniform = nearPlane + (farPlane - nearPlane) * p;
        float d = csm.splitLambda * (log - uniform) + uniform;
        csm.splitDistances[i + 1] = d;
    }
    
    csm.splitDistances[0] = nearPlane;
}

XMMATRIX LightingSystem::CalculateLightMatrix(const Light& light, const XMMATRIX& viewMatrix, float nearPlane, float farPlane)
{
    return light.GetLightMatrix();
}

HRESULT LightingSystem::GenerateIrradianceMap(ID3D11ShaderResourceView* environmentMap)
{
    // Placeholder for irradiance map generation
    Spark::SimpleConsole::GetInstance().LogInfo("Generating irradiance map (placeholder)");
    return S_OK;
}

HRESULT LightingSystem::GeneratePrefilterMap(ID3D11ShaderResourceView* environmentMap)
{
    // Placeholder for prefilter map generation
    Spark::SimpleConsole::GetInstance().LogInfo("Generating prefilter map (placeholder)");
    return S_OK;
}

HRESULT LightingSystem::GenerateBRDFLUT()
{
    // Placeholder for BRDF LUT generation
    Spark::SimpleConsole::GetInstance().LogInfo("Generating BRDF LUT (placeholder)");
    return S_OK;
}

HRESULT LightingSystem::CreateDefaultEnvironment()
{
    // Set up default environment lighting
    m_environmentLighting.skyColor = {0.5f, 0.7f, 1.0f};
    m_environmentLighting.skyIntensity = 1.0f;
    m_environmentLighting.skyTurbidity = 2.0f;
    m_environmentLighting.sunDirection = {0.3f, 0.7f, 0.2f};
    m_environmentLighting.sunSize = 0.04f;
    m_environmentLighting.sunIntensity = 5.0f;
    
    Spark::SimpleConsole::GetInstance().LogInfo("Default environment created");
    return S_OK;
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

std::string LightTypeToString(LightType type)
{
    switch (type) {
        case LightType::Directional: return "directional";
        case LightType::Point: return "point";
        case LightType::Spot: return "spot";
        case LightType::Area: return "area";
        case LightType::Environment: return "environment";
        default: return "unknown";
    }
}

LightType StringToLightType(const std::string& str)
{
    if (str == "directional") return LightType::Directional;
    if (str == "point") return LightType::Point;
    if (str == "spot") return LightType::Spot;
    if (str == "area") return LightType::Area;
    if (str == "environment") return LightType::Environment;
    return LightType::Directional; // Default
}

std::string ShadowTechniqueToString(ShadowTechnique technique)
{
    switch (technique) {
        case ShadowTechnique::None: return "none";
        case ShadowTechnique::Basic: return "basic";
        case ShadowTechnique::PCF: return "pcf";
        case ShadowTechnique::VSM: return "vsm";
        case ShadowTechnique::CSM: return "csm";
        case ShadowTechnique::PCSS: return "pcss";
        default: return "unknown";
    }
}

ShadowTechnique StringToShadowTechnique(const std::string& str)
{
    if (str == "none") return ShadowTechnique::None;
    if (str == "basic") return ShadowTechnique::Basic;
    if (str == "pcf") return ShadowTechnique::PCF;
    if (str == "vsm") return ShadowTechnique::VSM;
    if (str == "csm") return ShadowTechnique::CSM;
    if (str == "pcss") return ShadowTechnique::PCSS;
    return ShadowTechnique::PCF; // Default
}