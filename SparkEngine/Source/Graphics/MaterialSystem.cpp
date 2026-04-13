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

// Platform-specific methods (LoadTexture, BindMaterial, CreateSampler, etc.)
// live in MaterialSystemWindows.cpp and MaterialSystemLinux.cpp.

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
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
#ifdef SPARK_PLATFORM_WINDOWS
    SPARK_EXPECTS(device != nullptr);
    SPARK_EXPECTS(context != nullptr);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, context);
#endif
    // On Linux the D3D11 device/context are stubs — accept null in headless
    // mode. The Linux branch of CreateDefaultMaterials operates entirely on
    // CPU-side Material objects and doesn't touch the device pointer.
    m_device = device;
    m_context = context;
    memset(&m_metrics, 0, sizeof(m_metrics));
    m_frameStartTime = std::chrono::high_resolution_clock::now();

    HRESULT hr = CreateDefaultMaterials();
    UpdateMetrics();

    // Phase P: activate the persistent material constant buffer manager.
    // Pure CPU — no device dependency, runs on every platform. Sized for
    // 4096 materials × 256 bytes each = 1 MB shadow buffer, matching the
    // defaults the audit described as the reference working set.
    m_persistentCB.Initialize(/*maxMaterials*/ 4096, /*cbSizePerMat*/ 256);

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
    // Phase P: drop the material CB manager alongside the other
    // material storage. The Shutdown is safe on an uninitialised
    // manager (guarded by its own m_initialized flag).
    m_persistentCB.Shutdown();
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

    // Phase P: advance the persistent material CB manager's frame
    // counter so `UpdateMaterial` stamps `lastUpdatedFrame` correctly.
    // This is a single increment — zero cost even on scenes with no
    // material activity.
    m_persistentCB.BeginFrame();

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

// Platform-specific implementations (LoadTexture, UnloadTexture, GetSampler,
// BindMaterial, CreateDefaultMaterials, CreateSampler, GetFileTimestamp,
// PerformPeriodicMaintenance) live in:
//   - MaterialSystemWindows.cpp (D3D11)
//   - MaterialSystemLinux.cpp   (RHI stubs)
