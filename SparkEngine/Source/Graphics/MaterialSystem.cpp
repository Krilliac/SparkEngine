/**
 * @file MaterialSystem.cpp
 * @brief Core MaterialSystem implementation — lifecycle, CRUD, texture loading, utilities
 *
 * Material class implementation is in PBRMaterial.cpp.
 * Console inspection/listing/validation commands are in MaterialConsoleOps.cpp.
 * Console editing/texture/hot-reload commands are in MaterialConsoleEdit.cpp.
 */

#include "MaterialSystem.h"
#include "Core/Contracts.h"
#include "Core/Platform.h"
#include "../Utils/Assert.h"
#include "../Utils/Hash.h"
#include "../Utils/Validate.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#ifdef SPARK_PLATFORM_WINDOWS
#include "../Utils/SparkConsole.h"
#include "Utils/LocalFileCache.h"
#include <wincodec.h>
#include <wincodecsdk.h>
#else
#include "RHI/RHI.h"
#include <sys/stat.h>
#endif

// ============================================================================
// PLATFORM-INDEPENDENT IMPLEMENTATIONS
// ============================================================================

MaterialSystem::MaterialSystem() : m_device(nullptr), m_context(nullptr), m_hotReloadEnabled(false)
{
    memset(&m_metrics, 0, sizeof(m_metrics));
}

MaterialSystem::~MaterialSystem()
{
    Shutdown();
}

HRESULT MaterialSystem::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_EXPECTS(device != nullptr);
    SPARK_EXPECTS(context != nullptr);
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
#ifdef SPARK_PLATFORM_WINDOWS
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, context);
#endif
    m_device = device;
    m_context = context;
    memset(&m_metrics, 0, sizeof(m_metrics));
    m_frameStartTime = std::chrono::high_resolution_clock::now();

    HRESULT hr = CreateDefaultMaterials();
    UpdateMetrics();

#ifdef SPARK_PLATFORM_WINDOWS
    if (SUCCEEDED(hr))
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "MaterialSystem initialized successfully");
    }
    else
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "MaterialSystem failed to create default materials");
    }
#else
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "MaterialSystem (Linux) initialized");
#endif

    return hr;
}

void MaterialSystem::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "MaterialSystem shutting down (%zu materials)", m_materials.size());
    m_materials.clear();
    m_textureCache.clear();
    m_samplerCache.clear();
    m_fileTimestamps.clear();
    m_defaultMaterial.reset();
    m_errorMaterial.reset();
    m_device = nullptr;
    m_context = nullptr;
    memset(&m_metrics, 0, sizeof(m_metrics));
}

std::shared_ptr<Material> MaterialSystem::CreateMaterial(const std::string& name)
{
    SPARK_VALIDATE_RET(Spark::LogCategory::Graphics, !name.empty(), nullptr);

    auto existing = GetMaterial(name);
    if (existing && existing != m_defaultMaterial)
    {
        return existing;
    }

    SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "Creating material '%s'", name.c_str());
    auto material = std::make_shared<Material>(name);
    m_materials[name] = material;
    UpdateMetrics();
    return material;
}

std::shared_ptr<Material> MaterialSystem::LoadMaterial(const std::string& filePath)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_VALIDATE_RET(Spark::LogCategory::Graphics, !filePath.empty(), nullptr);

    auto it = m_materials.find(filePath);
    if (it != m_materials.end())
    {
        return it->second;
    }

    // Extract a material name from the filename
    std::string name = filePath;
    auto slashPos = filePath.find_last_of("/\\");
    if (slashPos != std::string::npos)
    {
        name = filePath.substr(slashPos + 1);
    }
    auto dotPos = name.find_last_of('.');
    if (dotPos != std::string::npos)
    {
        name = name.substr(0, dotPos);
    }

    auto material = std::make_shared<Material>(name);
    if (material->LoadFromFile(filePath, m_device))
    {
        m_materials[filePath] = material;

        if (m_hotReloadEnabled)
        {
            m_fileTimestamps[filePath] = GetFileTimestamp(filePath);
        }

        SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "Loaded material from '%s'", filePath.c_str());
        return material;
    }

    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to load material: %s", filePath.c_str());
    return m_errorMaterial;
}

std::shared_ptr<Material> MaterialSystem::GetMaterial(const std::string& name) const
{
    auto it = m_materials.find(name);
    return (it != m_materials.end()) ? it->second : m_defaultMaterial;
}

void MaterialSystem::UnloadMaterial(const std::string& name)
{
    SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "Unloading material '%s'", name.c_str());
    m_materials.erase(name);
    m_fileTimestamps.erase(name);
    UpdateMetrics();
}

void MaterialSystem::UnloadAllMaterials()
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Unloading all materials (%zu total)", m_materials.size());
    m_materials.clear();
    m_fileTimestamps.clear();
    UpdateMetrics();
}

void MaterialSystem::EnableHotReloading(bool enabled)
{
    m_hotReloadEnabled = enabled;
    if (enabled)
    {
        for (const auto& pair : m_materials)
        {
            m_fileTimestamps[pair.first] = GetFileTimestamp(pair.first);
        }
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Hot reload enabled");
    }
    else
    {
        m_fileTimestamps.clear();
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Hot reload disabled");
    }
}

void MaterialSystem::UpdateHotReload()
{
    if (!m_hotReloadEnabled)
        return;

    for (auto& pair : m_fileTimestamps)
    {
        const std::string& filePath = pair.first;
        uint64_t& lastTimestamp = pair.second;

        uint64_t currentTimestamp = GetFileTimestamp(filePath);
        if (currentTimestamp > lastTimestamp)
        {
            auto it = m_materials.find(filePath);
            if (it != m_materials.end())
            {
                if (it->second->LoadFromFile(filePath, m_device))
                {
                    lastTimestamp = currentTimestamp;
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Hot reloaded material: %s", filePath.c_str());
                }
                else
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to hot reload material: %s",
                                    filePath.c_str());
                }
            }
        }
    }
}

int MaterialSystem::ReloadAllMaterials()
{
    int reloadedCount = 0;
    for (auto& pair : m_materials)
    {
        if (pair.second->LoadFromFile(pair.first, m_device))
        {
            reloadedCount++;
        }
    }
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Reloaded %d materials", reloadedCount);
    return reloadedCount;
}

void MaterialSystem::BeginFrame()
{
    m_frameStartTime = std::chrono::high_resolution_clock::now();

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.materialSwitches = 0;
        m_metrics.textureBinds = 0;
    }

    UpdateMetrics();
    UpdateHotReload();
    PerformPeriodicMaintenance();
}

void MaterialSystem::EndFrame()
{
    auto frameEndTime = std::chrono::high_resolution_clock::now();
    auto frameDuration = std::chrono::duration_cast<std::chrono::microseconds>(frameEndTime - m_frameStartTime);
    (void)frameDuration;
}

std::shared_ptr<Material> MaterialSystem::CreateMaterialInstance(const std::string& templateName,
                                                                 const std::string& instanceName)
{
    auto templateMat = GetMaterial(templateName);
    if (!templateMat || templateMat == m_defaultMaterial)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "CreateMaterialInstance: template not found: %s",
                        templateName.c_str());
        return m_errorMaterial;
    }

    auto instance = templateMat->CreateInstance(instanceName);
    if (instance)
    {
        if (m_device)
        {
            instance->CompileMaterial(m_device);
        }
        m_materials[instanceName] = instance;
    }
    return instance;
}

void MaterialSystem::BindMaterial(const std::string& name)
{
    auto material = GetMaterial(name);
    BindMaterial(material);
}

bool MaterialSystem::ReloadMaterial(const std::string& name)
{
    auto it = m_materials.find(name);
    if (it == m_materials.end() || !it->second)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "ReloadMaterial: material not found: %s", name.c_str());
        return false;
    }

    bool result = it->second->ReloadMaterial(m_device);

    if (result && m_hotReloadEnabled)
    {
        m_fileTimestamps[name] = GetFileTimestamp(name);
    }

    return result;
}

MaterialSystem::MaterialMetrics MaterialSystem::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    MaterialMetrics metrics = m_metrics;
    metrics.loadedMaterials = static_cast<int>(m_materials.size());
    metrics.textureCount = static_cast<int>(m_textureCache.size());
    metrics.hotReloadEnabled = m_hotReloadEnabled;

    int totalVariants = 0;
    for (const auto& pair : m_materials)
    {
        if (pair.second)
        {
            totalVariants += static_cast<int>(pair.second->GetAvailableVariants().size());
        }
    }
    metrics.variantCount = totalVariants;

    return metrics;
}

void MaterialSystem::UpdateMetrics()
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_metrics.loadedMaterials = static_cast<int>(m_materials.size());
    m_metrics.textureCount = static_cast<int>(m_textureCache.size());
    m_metrics.hotReloadEnabled = m_hotReloadEnabled;

    size_t totalTextureMemory = 0;
    // Estimate total texture memory from cache size (rough 1MB/texture)
    totalTextureMemory = m_textureCache.size() * 1024 * 1024;
    m_metrics.textureMemory = totalTextureMemory;

    int totalVariants = 0;
    for (const auto& pair : m_materials)
    {
        if (pair.second)
        {
            totalVariants += static_cast<int>(pair.second->GetAvailableVariants().size());
        }
    }
    m_metrics.variantCount = totalVariants;

    m_metrics.averageLoadTime = 0.0f;
}

size_t MaterialSystem::HashSampling(const TextureSampling& sampling) const
{
    size_t hash = 0;
    auto hashCombine = [](size_t& seed, size_t value) { seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2); };

    hashCombine(hash, std::hash<int>{}(static_cast<int>(sampling.filter)));
    hashCombine(hash, std::hash<int>{}(static_cast<int>(sampling.addressU)));
    hashCombine(hash, std::hash<int>{}(static_cast<int>(sampling.addressV)));
    hashCombine(hash, std::hash<int>{}(static_cast<int>(sampling.addressW)));
    hashCombine(hash, std::hash<unsigned int>{}(sampling.maxAnisotropy));
    hashCombine(hash, std::hash<float>{}(sampling.mipLODBias));
    hashCombine(hash, std::hash<float>{}(sampling.minLOD));
    hashCombine(hash, std::hash<float>{}(sampling.maxLOD));

    return hash;
}

std::vector<std::string> MaterialSystem::GetShaderPermutation(const std::string& name) const
{
    auto material = GetMaterial(name);
    if (material && material != m_defaultMaterial)
    {
        return material->GetShaderPermutation();
    }
    return {};
}

std::string MaterialSystem::TextureTypeToString(MaterialTextureType type) const
{
    switch (type)
    {
    case MaterialTextureType::Albedo:
        return "Albedo";
    case MaterialTextureType::Normal:
        return "Normal";
    case MaterialTextureType::Metallic:
        return "Metallic";
    case MaterialTextureType::Roughness:
        return "Roughness";
    case MaterialTextureType::Occlusion:
        return "Occlusion";
    case MaterialTextureType::Emissive:
        return "Emissive";
    case MaterialTextureType::Height:
        return "Height";
    case MaterialTextureType::DetailAlbedo:
        return "DetailAlbedo";
    case MaterialTextureType::DetailNormal:
        return "DetailNormal";
    case MaterialTextureType::Subsurface:
        return "Subsurface";
    case MaterialTextureType::Transmission:
        return "Transmission";
    case MaterialTextureType::Clearcoat:
        return "Clearcoat";
    case MaterialTextureType::ClearcoatRoughness:
        return "ClearcoatRoughness";
    case MaterialTextureType::Anisotropy:
        return "Anisotropy";
    case MaterialTextureType::Custom0:
        return "Custom0";
    case MaterialTextureType::Custom1:
        return "Custom1";
    case MaterialTextureType::Custom2:
        return "Custom2";
    case MaterialTextureType::Custom3:
        return "Custom3";
    default:
        return "Unknown";
    }
}

MaterialTextureType MaterialSystem::StringToTextureType(const std::string& str) const
{
    if (str == "Albedo")
        return MaterialTextureType::Albedo;
    if (str == "Normal")
        return MaterialTextureType::Normal;
    if (str == "Metallic")
        return MaterialTextureType::Metallic;
    if (str == "Roughness")
        return MaterialTextureType::Roughness;
    if (str == "Occlusion")
        return MaterialTextureType::Occlusion;
    if (str == "Emissive")
        return MaterialTextureType::Emissive;
    if (str == "Height")
        return MaterialTextureType::Height;
    if (str == "DetailAlbedo")
        return MaterialTextureType::DetailAlbedo;
    if (str == "DetailNormal")
        return MaterialTextureType::DetailNormal;
    if (str == "Subsurface")
        return MaterialTextureType::Subsurface;
    if (str == "Transmission")
        return MaterialTextureType::Transmission;
    if (str == "Clearcoat")
        return MaterialTextureType::Clearcoat;
    if (str == "ClearcoatRoughness")
        return MaterialTextureType::ClearcoatRoughness;
    if (str == "Anisotropy")
        return MaterialTextureType::Anisotropy;
    if (str == "Custom0")
        return MaterialTextureType::Custom0;
    if (str == "Custom1")
        return MaterialTextureType::Custom1;
    if (str == "Custom2")
        return MaterialTextureType::Custom2;
    if (str == "Custom3")
        return MaterialTextureType::Custom3;
    return MaterialTextureType::Albedo;
}

// ============================================================================
// PLATFORM-SPECIFIC IMPLEMENTATIONS
// ============================================================================

#ifdef SPARK_PLATFORM_WINDOWS

ComPtr<ID3D11ShaderResourceView> MaterialSystem::LoadTexture(const std::string& filePath)
{
    auto it = m_textureCache.find(filePath);
    if (it != m_textureCache.end())
    {
        return it->second;
    }

    ComPtr<ID3D11ShaderResourceView> texture = LoadTextureFromFile(filePath);
    if (texture)
    {
        m_textureCache[filePath] = texture;
        Spark::SimpleConsole::GetInstance().LogInfo("Loaded texture: " + filePath);
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load texture: " + filePath);
    }

    return texture;
}

void MaterialSystem::UnloadTexture(const std::string& filePath)
{
    auto it = m_textureCache.find(filePath);
    if (it != m_textureCache.end())
    {
        m_textureCache.erase(it);
        Spark::SimpleConsole::GetInstance().LogInfo("Unloaded texture: " + filePath);
    }
}

ComPtr<ID3D11SamplerState> MaterialSystem::GetSampler(const TextureSampling& sampling)
{
    size_t hash = HashSampling(sampling);
    auto it = m_samplerCache.find(hash);
    if (it != m_samplerCache.end())
    {
        return it->second;
    }

    ComPtr<ID3D11SamplerState> sampler;
    HRESULT hr = CreateSampler(sampling, &sampler);
    if (SUCCEEDED(hr))
    {
        m_samplerCache[hash] = sampler;
    }
    return sampler;
}

void MaterialSystem::BindMaterial(const std::shared_ptr<Material>& material)
{
    if (!material || !m_context)
    {
        return;
    }

    // Auto-compile if needed
    if (!material->m_compiled && m_device)
    {
        material->CompileMaterial(m_device);
    }

    // Update the constant buffer with current PBR properties
    if (material->m_constantBuffer)
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_context->Map(material->m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr) && mapped.pData)
        {
            const auto& pbr = material->GetPBRProperties();
            MaterialConstants constants = {};
            constants.albedoColor = pbr.albedoColor;
            constants.metallicFactor = pbr.metallicFactor;
            constants.roughnessFactor = pbr.roughnessFactor;
            constants.normalScale = pbr.normalScale;
            constants.occlusionStrength = pbr.occlusionStrength;
            constants.emissiveColor = pbr.emissiveColor;
            constants.emissiveFactor = pbr.emissiveFactor;
            constants.alphaCutoff = pbr.alphaCutoff;
            constants.indexOfRefraction = pbr.indexOfRefraction;
            constants.pad0 = 0.0f;
            constants.pad1 = 0.0f;
            std::memcpy(mapped.pData, &constants, sizeof(MaterialConstants));
            m_context->Unmap(material->m_constantBuffer.Get(), 0);
        }

        ID3D11Buffer* cbuffers[] = {material->m_constantBuffer.Get()};
        m_context->PSSetConstantBuffers(1, 1, cbuffers);
    }

    // Bind texture SRVs for each active texture slot
    static const std::pair<MaterialTextureType, UINT> textureSlots[] = {
        {MaterialTextureType::Albedo, 0},    {MaterialTextureType::Normal, 1},    {MaterialTextureType::Metallic, 2},
        {MaterialTextureType::Roughness, 3}, {MaterialTextureType::Occlusion, 4}, {MaterialTextureType::Emissive, 5},
    };

    int boundTextures = 0;
    for (const auto& [texType, slot] : textureSlots)
    {
        if (material->HasTexture(texType))
        {
            const auto& matTex = material->GetTexture(texType);
            if (matTex.enabled && matTex.texture)
            {
                ID3D11ShaderResourceView* srv = matTex.texture.Get();
                m_context->PSSetShaderResources(slot, 1, &srv);
                boundTextures++;
            }
            else
            {
                ID3D11ShaderResourceView* nullSrv = nullptr;
                m_context->PSSetShaderResources(slot, 1, &nullSrv);
            }
        }
        else
        {
            ID3D11ShaderResourceView* nullSrv = nullptr;
            m_context->PSSetShaderResources(slot, 1, &nullSrv);
        }
    }

    // Set blend state
    if (material->m_blendState)
    {
        const FLOAT blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        m_context->OMSetBlendState(material->m_blendState.Get(), blendFactor, 0xFFFFFFFF);
    }

    if (material->m_depthStencilState)
    {
        m_context->OMSetDepthStencilState(material->m_depthStencilState.Get(), 0);
    }

    if (material->m_rasterizerState)
    {
        m_context->RSSetState(material->m_rasterizerState.Get());
    }

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.materialSwitches++;
        m_metrics.textureBinds += boundTextures;
    }
}

HRESULT MaterialSystem::CreateDefaultMaterials()
{
    m_defaultMaterial = std::make_shared<Material>("Default");
    PBRProperties defaultPbr = {};
    defaultPbr.albedoColor = {0.7f, 0.7f, 0.7f, 1.0f};
    defaultPbr.metallicFactor = 0.0f;
    defaultPbr.roughnessFactor = 0.8f;
    defaultPbr.normalScale = 1.0f;
    defaultPbr.occlusionStrength = 1.0f;
    defaultPbr.emissiveColor = {0.0f, 0.0f, 0.0f};
    defaultPbr.emissiveFactor = 0.0f;
    defaultPbr.alphaCutoff = 0.5f;
    defaultPbr.indexOfRefraction = 1.5f;
    m_defaultMaterial->SetPBRProperties(defaultPbr);

    m_errorMaterial = std::make_shared<Material>("Error");
    PBRProperties errorPbr = defaultPbr;
    errorPbr.albedoColor = {1.0f, 0.0f, 1.0f, 1.0f};
    errorPbr.emissiveColor = {0.2f, 0.0f, 0.2f};
    errorPbr.emissiveFactor = 0.5f;
    m_errorMaterial->SetPBRProperties(errorPbr);

    if (m_device)
    {
        m_defaultMaterial->CompileMaterial(m_device);
        m_errorMaterial->CompileMaterial(m_device);
    }

    return S_OK;
}

HRESULT MaterialSystem::CreateSampler(const TextureSampling& sampling, ID3D11SamplerState** sampler)
{
    if (!m_device)
        return E_FAIL;

    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = sampling.filter;
    desc.AddressU = sampling.addressU;
    desc.AddressV = sampling.addressV;
    desc.AddressW = sampling.addressW;
    desc.MaxAnisotropy = sampling.maxAnisotropy;
    desc.MipLODBias = sampling.mipLODBias;
    desc.MinLOD = sampling.minLOD;
    desc.MaxLOD = sampling.maxLOD;
    desc.BorderColor[0] = sampling.borderColor.x;
    desc.BorderColor[1] = sampling.borderColor.y;
    desc.BorderColor[2] = sampling.borderColor.z;
    desc.BorderColor[3] = sampling.borderColor.w;

    return m_device->CreateSamplerState(&desc, sampler);
}

uint64_t MaterialSystem::GetFileTimestamp(const std::string& filePath) const
{
    try
    {
        if (std::filesystem::exists(filePath))
        {
            auto time = std::filesystem::last_write_time(filePath);
            return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
        }
    }
    catch (const std::exception&)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to get timestamp for file: %s", filePath.c_str());
    }
    return 0;
}

// LoadTextureFromFile is in MaterialTextureLoading.cpp (WIC-based, Windows only)

void MaterialSystem::PerformPeriodicMaintenance()
{
    static auto lastMaintenanceTime = std::chrono::high_resolution_clock::now();
    auto currentTime = std::chrono::high_resolution_clock::now();
    auto deltaTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastMaintenanceTime);

    if (deltaTime.count() >= 60)
    {
        lastMaintenanceTime = currentTime;

        if (m_samplerCache.size() > 50)
        {
            Spark::SimpleConsole::GetInstance().LogInfo(
                "MaterialSystem maintenance: " + std::to_string(m_samplerCache.size()) + " samplers in cache");
        }

        size_t estimatedMemory = m_textureCache.size() * 1024 * 1024;
        if (estimatedMemory > 500 * 1024 * 1024)
        {
            Spark::SimpleConsole::GetInstance().LogWarning("MaterialSystem using high memory: ~" +
                                                           std::to_string(estimatedMemory / 1024 / 1024) + "MB");
        }
    }
}

#else // !SPARK_PLATFORM_WINDOWS

ComPtr<ID3D11ShaderResourceView> MaterialSystem::LoadTexture(const std::string& /*filePath*/)
{
    return ComPtr<ID3D11ShaderResourceView>();
}

void MaterialSystem::UnloadTexture(const std::string& filePath)
{
    m_textureCache.erase(filePath);
    UpdateMetrics();
}

ComPtr<ID3D11SamplerState> MaterialSystem::GetSampler(const TextureSampling& /*sampling*/)
{
    return ComPtr<ID3D11SamplerState>();
}

void MaterialSystem::BindMaterial(const std::shared_ptr<Material>& material)
{
    if (!material)
        return;

    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_metrics.materialSwitches++;
}

HRESULT MaterialSystem::CreateDefaultMaterials()
{
    m_defaultMaterial = std::make_shared<Material>("__default");

    m_errorMaterial = std::make_shared<Material>("__error");
    PBRProperties errorPBR;
    errorPBR.albedoColor = {1.0f, 0.0f, 1.0f, 1.0f};
    errorPBR.metallicFactor = 0.0f;
    errorPBR.roughnessFactor = 0.8f;
    errorPBR.emissiveColor = {0.5f, 0.0f, 0.5f};
    errorPBR.emissiveFactor = 0.5f;
    m_errorMaterial->SetPBRProperties(errorPBR);

    return S_OK;
}

HRESULT MaterialSystem::CreateSampler(const TextureSampling& sampling, ID3D11SamplerState** /*sampler*/)
{
    auto rhiDevice = Spark::RHI::CreateDevice(Spark::RHI::GraphicsBackend::Auto);
    if (!rhiDevice)
        return E_FAIL;

    Spark::RHI::RHISamplerDesc desc;

    if (sampling.filter == D3D11_FILTER_ANISOTROPIC)
    {
        desc.minFilter = Spark::RHI::RHIFilterMode::Anisotropic;
        desc.magFilter = Spark::RHI::RHIFilterMode::Anisotropic;
        desc.mipFilter = Spark::RHI::RHIFilterMode::Anisotropic;
    }
    else if (sampling.filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR)
    {
        desc.minFilter = Spark::RHI::RHIFilterMode::Linear;
        desc.magFilter = Spark::RHI::RHIFilterMode::Linear;
        desc.mipFilter = Spark::RHI::RHIFilterMode::Linear;
    }
    else if (sampling.filter == D3D11_FILTER_MIN_MAG_MIP_POINT)
    {
        desc.minFilter = Spark::RHI::RHIFilterMode::Nearest;
        desc.magFilter = Spark::RHI::RHIFilterMode::Nearest;
        desc.mipFilter = Spark::RHI::RHIFilterMode::Nearest;
    }
    else
    {
        desc.minFilter = Spark::RHI::RHIFilterMode::Linear;
        desc.magFilter = Spark::RHI::RHIFilterMode::Linear;
        desc.mipFilter = Spark::RHI::RHIFilterMode::Linear;
    }

    auto mapAddressMode = [](D3D11_TEXTURE_ADDRESS_MODE mode) -> Spark::RHI::RHIAddressMode
    {
        switch (mode)
        {
        case D3D11_TEXTURE_ADDRESS_WRAP:
            return Spark::RHI::RHIAddressMode::Wrap;
        case D3D11_TEXTURE_ADDRESS_CLAMP:
            return Spark::RHI::RHIAddressMode::Clamp;
        case D3D11_TEXTURE_ADDRESS_MIRROR:
            return Spark::RHI::RHIAddressMode::Mirror;
        case D3D11_TEXTURE_ADDRESS_BORDER:
            return Spark::RHI::RHIAddressMode::Border;
        case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE:
            return Spark::RHI::RHIAddressMode::MirrorOnce;
        default:
            return Spark::RHI::RHIAddressMode::Wrap;
        }
    };

    desc.addressU = mapAddressMode(sampling.addressU);
    desc.addressV = mapAddressMode(sampling.addressV);
    desc.addressW = mapAddressMode(sampling.addressW);
    desc.maxAnisotropy = sampling.maxAnisotropy;
    desc.mipLodBias = sampling.mipLODBias;
    desc.minLod = sampling.minLOD;
    desc.maxLod = sampling.maxLOD;
    desc.borderColor[0] = sampling.borderColor.x;
    desc.borderColor[1] = sampling.borderColor.y;
    desc.borderColor[2] = sampling.borderColor.z;
    desc.borderColor[3] = sampling.borderColor.w;

    auto rhiSampler = rhiDevice->CreateSampler(desc);
    if (!rhiSampler)
        return E_FAIL;

    return S_OK;
}

uint64_t MaterialSystem::GetFileTimestamp(const std::string& filePath) const
{
    struct stat fileStat;
    if (stat(filePath.c_str(), &fileStat) == 0)
    {
        return static_cast<uint64_t>(fileStat.st_mtime);
    }
    return 0;
}

ComPtr<ID3D11ShaderResourceView> MaterialSystem::LoadTextureFromFile(const std::string& /*filePath*/)
{
    return ComPtr<ID3D11ShaderResourceView>();
}

void MaterialSystem::PerformPeriodicMaintenance()
{
    // No-op on Linux - no GPU resources to manage
}

#endif // SPARK_PLATFORM_WINDOWS
