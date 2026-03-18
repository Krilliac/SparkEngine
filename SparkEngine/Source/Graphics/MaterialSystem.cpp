#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file MaterialSystem.cpp
 * @brief Core MaterialSystem implementation — lifecycle, CRUD, texture loading, utilities
 *
 * Material class implementation is in PBRMaterial.cpp.
 * Console inspection/listing/validation commands are in MaterialConsoleOps.cpp.
 * Console editing/texture/hot-reload commands are in MaterialConsoleEdit.cpp.
 */

#include "MaterialSystem.h"
#include "../Utils/Assert.h"
#include "../Utils/Hash.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include "Utils/LocalFileCache.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <cstring>

#ifdef SPARK_PLATFORM_WINDOWS
#include <wincodec.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <wincodecsdk.h>

#ifdef SPARK_PLATFORM_WINDOWS

// ============================================================================
// MATERIAL SYSTEM IMPLEMENTATION
// ============================================================================

MaterialSystem::MaterialSystem() : m_device(nullptr), m_context(nullptr), m_hotReloadEnabled(false) {}

MaterialSystem::~MaterialSystem()
{
    Shutdown();
}

HRESULT MaterialSystem::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, context);
    m_device = device;
    m_context = context;

    HRESULT hr = CreateDefaultMaterials();
    if (SUCCEEDED(hr))
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "MaterialSystem initialized successfully");
    }
    else
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "MaterialSystem failed to create default materials");
    }
    return hr;
}

void MaterialSystem::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "MaterialSystem shutting down (%zu materials, %zu cached textures)",
                   m_materials.size(), m_textureCache.size());
    m_materials.clear();
    m_textureCache.clear();
    m_samplerCache.clear();
    m_defaultMaterial.reset();
    m_errorMaterial.reset();
    m_device = nullptr;
    m_context = nullptr;
}

std::shared_ptr<Material> MaterialSystem::CreateMaterial(const std::string& name)
{
    SPARK_VALIDATE_RET(Spark::LogCategory::Graphics, !name.empty(), nullptr);
    SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "Creating material '%s'", name.c_str());
    auto material = std::make_shared<Material>(name);
    m_materials[name] = material;
    return material;
}

std::shared_ptr<Material> MaterialSystem::LoadMaterial(const std::string& filePath)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_VALIDATE_RET(Spark::LogCategory::Graphics, !filePath.empty(), nullptr);
    // Check if already loaded
    auto it = m_materials.find(filePath);
    if (it != m_materials.end())
    {
        return it->second;
    }

    auto material = std::make_shared<Material>(filePath);
    if (material->LoadFromFile(filePath, m_device))
    {
        m_materials[filePath] = material;

        // Store file timestamp for hot reloading
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
    SPARK_WARN_IF(Spark::LogCategory::Graphics, it == m_materials.end() && !name.empty(),
                  "Material not found, returning default");
    return (it != m_materials.end()) ? it->second : m_defaultMaterial;
}

void MaterialSystem::UnloadMaterial(const std::string& name)
{
    SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "Unloading material '%s'", name.c_str());
    auto it = m_materials.find(name);
    if (it != m_materials.end())
    {
        m_materials.erase(it);
        m_fileTimestamps.erase(name);
    }
}

void MaterialSystem::UnloadAllMaterials()
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Unloading all materials (%zu total)", m_materials.size());
    m_materials.clear();
    m_fileTimestamps.clear();
}

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
            // File has been modified, reload it
            auto it = m_materials.find(filePath);
            if (it != m_materials.end())
            {
                if (it->second->LoadFromFile(filePath, m_device))
                {
                    lastTimestamp = currentTimestamp;
                    Spark::SimpleConsole::GetInstance().LogInfo("Hot reloaded material: " + filePath);
                }
                else
                {
                    Spark::SimpleConsole::GetInstance().LogError("Failed to hot reload material: " + filePath);
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
        auto& material = pair.second;
        if (material->LoadFromFile(pair.first, m_device))
        {
            reloadedCount++;
        }
    }

    Spark::SimpleConsole::GetInstance().LogInfo("Reloaded " + std::to_string(reloadedCount) + " materials");
    return reloadedCount;
}

void MaterialSystem::BeginFrame()
{
    m_frameStartTime = std::chrono::high_resolution_clock::now();

    // Reset per-frame metrics
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_metrics.materialSwitches = 0;
    m_metrics.textureBinds = 0;

    UpdateMetrics();

    // Update hot reloading
    UpdateHotReload();

    // Perform periodic maintenance
    PerformPeriodicMaintenance();
}

void MaterialSystem::EndFrame()
{
    auto frameEndTime = std::chrono::high_resolution_clock::now();
    auto frameDuration = std::chrono::duration_cast<std::chrono::microseconds>(frameEndTime - m_frameStartTime);

    // Update frame time metrics if needed
}

std::shared_ptr<Material> MaterialSystem::CreateMaterialInstance(const std::string& templateName,
                                                                 const std::string& instanceName)
{
    auto templateMat = GetMaterial(templateName);
    if (!templateMat || templateMat == m_defaultMaterial)
    {
        Spark::SimpleConsole::GetInstance().LogError("CreateMaterialInstance: template material not found: " +
                                                     templateName);
        return m_errorMaterial;
    }

    auto instance = templateMat->CreateInstance(instanceName);
    if (instance)
    {
        // Compile the instance so it has its own constant buffer
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
        if (SUCCEEDED(hr))
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

        // Bind constant buffer to pixel shader slot 1 (slot 0 is often per-frame/camera)
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

    // Set depth stencil state
    if (material->m_depthStencilState)
    {
        m_context->OMSetDepthStencilState(material->m_depthStencilState.Get(), 0);
    }

    // Set rasterizer state
    if (material->m_rasterizerState)
    {
        m_context->RSSetState(material->m_rasterizerState.Get());
    }

    // Update per-frame metrics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.materialSwitches++;
        m_metrics.textureBinds += boundTextures;
    }
}

bool MaterialSystem::ReloadMaterial(const std::string& name)
{
    auto it = m_materials.find(name);
    if (it == m_materials.end() || !it->second)
    {
        Spark::SimpleConsole::GetInstance().LogError("ReloadMaterial: material not found: " + name);
        return false;
    }

    bool result = it->second->ReloadMaterial(m_device);

    // Update file timestamp if hot reload is enabled
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
    for (const auto& materialPair : m_materials)
    {
        if (materialPair.second)
        {
            totalVariants += static_cast<int>(materialPair.second->GetAvailableVariants().size());
        }
    }
    metrics.variantCount = totalVariants;

    return metrics;
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
        Spark::SimpleConsole::GetInstance().LogSuccess("Hot reload enabled");
    }
    else
    {
        m_fileTimestamps.clear();
        Spark::SimpleConsole::GetInstance().LogInfo("Hot reload disabled");
    }
}
HRESULT MaterialSystem::CreateDefaultMaterials()
{
    // Create default material
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

    // Create error material (magenta color for missing materials)
    m_errorMaterial = std::make_shared<Material>("Error");
    PBRProperties errorPbr = defaultPbr;
    errorPbr.albedoColor = {1.0f, 0.0f, 1.0f, 1.0f}; // Magenta
    errorPbr.emissiveColor = {0.2f, 0.0f, 0.2f};
    errorPbr.emissiveFactor = 0.5f;
    m_errorMaterial->SetPBRProperties(errorPbr);

    // Compile default pipeline states for both materials
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

size_t MaterialSystem::HashSampling(const TextureSampling& sampling) const
{
    size_t hash = 0;
    hash ^= std::hash<int>{}(static_cast<int>(sampling.filter));
    hash ^= std::hash<int>{}(static_cast<int>(sampling.addressU)) << 1;
    hash ^= std::hash<int>{}(static_cast<int>(sampling.addressV)) << 2;
    hash ^= std::hash<int>{}(static_cast<int>(sampling.addressW)) << 3;
    hash ^= std::hash<UINT>{}(sampling.maxAnisotropy) << 4;
    hash ^= std::hash<float>{}(sampling.mipLODBias) << 5;
    hash ^= std::hash<float>{}(sampling.minLOD) << 6;
    hash ^= std::hash<float>{}(sampling.maxLOD) << 7;
    return hash;
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
        // Error accessing file
        Spark::SimpleConsole::GetInstance().LogError("Failed to get timestamp for file: " + filePath);
        return 0; // Return 0 if we can't get the timestamp
    }
    return 0;
}

// LoadTextureFromFile implementation with proper WIC loading and mipmap support
ComPtr<ID3D11ShaderResourceView> MaterialSystem::LoadTextureFromFile(const std::string& filePath)
{
    ComPtr<ID3D11ShaderResourceView> texture;

    if (!m_device || filePath.empty())
    {
        Spark::SimpleConsole::GetInstance().LogError("Invalid device or empty file path in LoadTextureFromFile");
        return texture;
    }

    if (!std::filesystem::exists(filePath))
    {
        Spark::SimpleConsole::GetInstance().LogError("Texture file not found: " + filePath);
        return texture;
    }

    try
    {
        // Initialize WIC factory
        ComPtr<IWICImagingFactory> wicFactory;
        HRESULT hr =
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create WIC Imaging Factory for: " + filePath);
            return texture;
        }

        // Create decoder
        ComPtr<IWICBitmapDecoder> decoder;
        std::wstring wideFilePath(filePath.begin(), filePath.end());
        hr = wicFactory->CreateDecoderFromFilename(wideFilePath.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create WIC decoder for: " + filePath);
            return texture;
        }

        // Get first frame
        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to get frame from decoder for: " + filePath);
            return texture;
        }

        // Get original size for mipmap calculation
        UINT originalWidth, originalHeight;
        hr = frame->GetSize(&originalWidth, &originalHeight);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to get frame size for: " + filePath);
            return texture;
        }

        // Calculate mip levels (power of 2 textures get full mip chain)
        UINT mipLevels = 1;
        if ((originalWidth & (originalWidth - 1)) == 0 && (originalHeight & (originalHeight - 1)) == 0)
        {
            // Power of 2 texture - calculate full mip chain
            UINT maxDimension = std::max(originalWidth, originalHeight);
            while (maxDimension > 1)
            {
                maxDimension >>= 1;
                mipLevels++;
            }
        }

        // Create format converter
        ComPtr<IWICFormatConverter> converter;
        hr = wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create format converter for: " + filePath);
            return texture;
        }

        // Convert to RGBA format
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to initialize format converter for: " + filePath);
            return texture;
        }

        // Create texture description
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = originalWidth;
        texDesc.Height = originalHeight;
        texDesc.MipLevels = mipLevels;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        texDesc.CPUAccessFlags = 0;
        texDesc.MiscFlags = (mipLevels > 1) ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

        // Prepare initial data for base mip level
        std::vector<BYTE> imageData(originalWidth * originalHeight * 4);
        hr = converter->CopyPixels(nullptr, originalWidth * 4, static_cast<UINT>(imageData.size()), imageData.data());
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to copy pixels for: " + filePath);
            return texture;
        }

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = imageData.data();
        initData.SysMemPitch = originalWidth * 4;
        initData.SysMemSlicePitch = 0;

        // Create texture
        ComPtr<ID3D11Texture2D> tex2D;
        hr = m_device->CreateTexture2D(&texDesc, &initData, &tex2D);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create Direct3D texture for: " + filePath);
            return texture;
        }

        // Create shader resource view
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = mipLevels;

        hr = m_device->CreateShaderResourceView(tex2D.Get(), &srvDesc, &texture);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create shader resource view for: " + filePath);
            return texture;
        }

        // Generate mipmaps if enabled
        if (mipLevels > 1 && m_context)
        {
            m_context->GenerateMips(texture.Get());
        }

        Spark::SimpleConsole::GetInstance().LogInfo(
            "Successfully loaded texture: " + filePath + " (" + std::to_string(originalWidth) + "x" +
            std::to_string(originalHeight) + ", " + std::to_string(mipLevels) + " mips)");
    }
    catch (const std::exception& e)
    {
        Spark::SimpleConsole::GetInstance().LogError("Exception loading texture " + filePath + ": " +
                                                     std::string(e.what()));
        texture.Reset();
    }

    return texture;
}

void MaterialSystem::UpdateMetrics()
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);

    // Basic metrics
    m_metrics.loadedMaterials = static_cast<int>(m_materials.size());
    m_metrics.textureCount = static_cast<int>(m_textureCache.size());
    m_metrics.hotReloadEnabled = m_hotReloadEnabled;

    // Calculate texture memory usage (improved estimation)
    size_t totalTextureMemory = 0;
    for (const auto& pair : m_textureCache)
    {
        // For a more accurate estimate, we'd need to query the actual texture
        // For now, estimate based on common texture sizes and formats
        // This is a rough approximation - in production you'd want to track actual sizes
        totalTextureMemory += 1024 * 1024; // 1MB per texture (very rough estimate)
    }
    m_metrics.textureMemory = totalTextureMemory;

    // Count variants across all materials
    int totalVariants = 0;
    for (const auto& materialPair : m_materials)
    {
        if (materialPair.second)
        {
            // Access variant count through material's internal structure
            // Since we can't directly access private members, we estimate based on naming
            totalVariants += 1; // Each material has at least one "default" variant
        }
    }
    m_metrics.variantCount = totalVariants;

    // Performance metrics (would be updated during actual rendering)
    // These would be incremented during actual material binding operations
    // m_metrics.materialSwitches and m_metrics.textureBinds are reset in BeginFrame()

    // Load time tracking — actual per-material timing would require
    // instrumenting CreateMaterial(). For now, report 0 until that's added.
    m_metrics.averageLoadTime = 0.0f;
}

void MaterialSystem::PerformPeriodicMaintenance()
{
    // This method can be called periodically to perform maintenance tasks
    // such as cleaning up unused resources, optimizing caches, etc.

    static auto lastMaintenanceTime = std::chrono::high_resolution_clock::now();
    auto currentTime = std::chrono::high_resolution_clock::now();
    auto deltaTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastMaintenanceTime);

    // Perform maintenance every 60 seconds
    if (deltaTime.count() >= 60)
    {
        lastMaintenanceTime = currentTime;

        // Clean up unused samplers (keep commonly used ones)
        if (m_samplerCache.size() > 50)
        {
            // In a full implementation, you'd track usage frequency
            // For now, just log that maintenance would occur
            Spark::SimpleConsole::GetInstance().LogInfo(
                "MaterialSystem maintenance: " + std::to_string(m_samplerCache.size()) + " samplers in cache");
        }

        // Log memory usage
        size_t estimatedMemory = m_textureCache.size() * 1024 * 1024; // Rough estimate
        if (estimatedMemory > 500 * 1024 * 1024)
        { // > 500MB
            Spark::SimpleConsole::GetInstance().LogWarning("MaterialSystem using high memory: ~" +
                                                           std::to_string(estimatedMemory / 1024 / 1024) + "MB");
        }
    }
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
    using namespace Spark::HashLiterals;
    switch (Spark::FNV1a64(str))
    {
    case "Albedo"_hash64:
        return MaterialTextureType::Albedo;
    case "Normal"_hash64:
        return MaterialTextureType::Normal;
    case "Metallic"_hash64:
        return MaterialTextureType::Metallic;
    case "Roughness"_hash64:
        return MaterialTextureType::Roughness;
    case "Occlusion"_hash64:
        return MaterialTextureType::Occlusion;
    case "Emissive"_hash64:
        return MaterialTextureType::Emissive;
    case "Height"_hash64:
        return MaterialTextureType::Height;
    case "DetailAlbedo"_hash64:
        return MaterialTextureType::DetailAlbedo;
    case "DetailNormal"_hash64:
        return MaterialTextureType::DetailNormal;
    case "Subsurface"_hash64:
        return MaterialTextureType::Subsurface;
    case "Transmission"_hash64:
        return MaterialTextureType::Transmission;
    case "Clearcoat"_hash64:
        return MaterialTextureType::Clearcoat;
    case "ClearcoatRoughness"_hash64:
        return MaterialTextureType::ClearcoatRoughness;
    case "Anisotropy"_hash64:
        return MaterialTextureType::Anisotropy;
    case "Custom0"_hash64:
        return MaterialTextureType::Custom0;
    case "Custom1"_hash64:
        return MaterialTextureType::Custom1;
    case "Custom2"_hash64:
        return MaterialTextureType::Custom2;
    case "Custom3"_hash64:
        return MaterialTextureType::Custom3;
    default:
        return MaterialTextureType::Albedo;
    }
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "MaterialSystem.h"
#include "RHI/RHI.h"
#include "../Utils/Hash.h"
#include "../Utils/Validate.h"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <sys/stat.h>

// ============================================================================
// MaterialSystem (Linux full implementation)
// ============================================================================

// ============================================================================
// MaterialSystem (Linux full implementation)
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
    m_device = device;
    m_context = context;
    memset(&m_metrics, 0, sizeof(m_metrics));
    m_frameStartTime = std::chrono::high_resolution_clock::now();

    CreateDefaultMaterials();

    UpdateMetrics();
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "MaterialSystem (Linux) initialized");
    return S_OK;
}

void MaterialSystem::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "MaterialSystem (Linux) shutting down (%zu materials)",
                   m_materials.size());
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
    auto existing = GetMaterial(name);
    if (existing)
    {
        return existing;
    }
    auto material = std::make_shared<Material>(name);
    m_materials[name] = material;
    UpdateMetrics();
    return material;
}

std::shared_ptr<Material> MaterialSystem::LoadMaterial(const std::string& filePath)
{
    auto it = m_materials.find(filePath);
    if (it != m_materials.end())
        return it->second;

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
    material->LoadFromFile(filePath, m_device);
    m_materials[filePath] = material;
    UpdateMetrics();
    return material;
}

std::shared_ptr<Material> MaterialSystem::GetMaterial(const std::string& name) const
{
    auto it = m_materials.find(name);
    return (it != m_materials.end()) ? it->second : nullptr;
}

void MaterialSystem::UnloadMaterial(const std::string& name)
{
    m_materials.erase(name);
    UpdateMetrics();
}

void MaterialSystem::UnloadAllMaterials()
{
    m_materials.clear();
    UpdateMetrics();
}

ComPtr<ID3D11ShaderResourceView> MaterialSystem::LoadTexture(const std::string& /*filePath*/)
{
    // No GPU texture loading on Linux - return empty ComPtr
    return ComPtr<ID3D11ShaderResourceView>();
}

void MaterialSystem::UnloadTexture(const std::string& filePath)
{
    m_textureCache.erase(filePath);
    UpdateMetrics();
}

ComPtr<ID3D11SamplerState> MaterialSystem::GetSampler(const TextureSampling& /*sampling*/)
{
    // No GPU sampler creation on Linux - return empty ComPtr
    return ComPtr<ID3D11SamplerState>();
}

void MaterialSystem::EnableHotReloading(bool enabled)
{
    m_hotReloadEnabled = enabled;
    if (enabled)
    {
        // Initialize timestamps for all currently loaded materials
        for (const auto& pair : m_materials)
        {
            m_fileTimestamps[pair.first] = GetFileTimestamp(pair.first);
        }
    }
    else
    {
        m_fileTimestamps.clear();
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
                    fprintf(stderr, "[MaterialSystem] Hot reloaded material: %s\n", filePath.c_str());
                }
                else
                {
                    fprintf(stderr, "[MaterialSystem] Failed to hot reload material: %s\n", filePath.c_str());
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
    fprintf(stderr, "[MaterialSystem] Reloaded %d materials\n", reloadedCount);
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
}

void MaterialSystem::EndFrame()
{
    auto endTime = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(endTime - m_frameStartTime).count();
    (void)elapsed; // Available for profiling if needed

    UpdateMetrics();
    PerformPeriodicMaintenance();
}

std::shared_ptr<Material> MaterialSystem::CreateMaterialInstance(const std::string& templateName,
                                                                 const std::string& instanceName)
{
    auto templateMat = GetMaterial(templateName);
    if (!templateMat)
    {
        fprintf(stderr, "[MaterialSystem] CreateMaterialInstance: template '%s' not found\n", templateName.c_str());
        return nullptr;
    }

    auto instance = templateMat->CreateInstance(instanceName);
    if (instance)
    {
        instance->CompileMaterial(m_device);
        m_materials[instanceName] = instance;
    }
    return instance;
}

void MaterialSystem::BindMaterial(const std::string& name)
{
    auto material = GetMaterial(name);
    BindMaterial(material);
}

void MaterialSystem::BindMaterial(const std::shared_ptr<Material>& material)
{
    if (!material)
        return;

    // No-op on Linux - no GPU binding
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.materialSwitches++;
    }
}

bool MaterialSystem::ReloadMaterial(const std::string& name)
{
    auto it = m_materials.find(name);
    if (it == m_materials.end() || !it->second)
    {
        fprintf(stderr, "[MaterialSystem] ReloadMaterial: material '%s' not found\n", name.c_str());
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

// ============================================================================
// Private helper methods
// ============================================================================

HRESULT MaterialSystem::CreateDefaultMaterials()
{
    // Create default material with standard PBR defaults
    m_defaultMaterial = std::make_shared<Material>("__default");

    // Create error material - bright magenta to be visually obvious
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
    // On Linux, create the sampler state through the RHI abstraction layer.
    // The ID3D11SamplerState** output is unused; the RHI manages sampler lifetime.
    auto rhiDevice = Spark::RHI::CreateDevice(Spark::RHI::GraphicsBackend::Auto);
    if (!rhiDevice)
        return E_FAIL;

    // Map D3D11 filter enum to RHI filter mode
    Spark::RHI::RHISamplerDesc desc;

    // D3D11_FILTER_ANISOTROPIC = 0x55
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
        // Default to linear filtering for other filter combinations
        desc.minFilter = Spark::RHI::RHIFilterMode::Linear;
        desc.magFilter = Spark::RHI::RHIFilterMode::Linear;
        desc.mipFilter = Spark::RHI::RHIFilterMode::Linear;
    }

    // Map D3D11 address modes to RHI address modes
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

    Spark::RHI::IRHISampler* rhiSampler = rhiDevice->CreateSampler(desc);
    if (!rhiSampler)
        return E_FAIL;

    return S_OK;
}

size_t MaterialSystem::HashSampling(const TextureSampling& sampling) const
{
    size_t hash = 0;
    // Combine hash values for sampling parameters
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
    // No GPU texture loading on Linux
    return ComPtr<ID3D11ShaderResourceView>();
}

void MaterialSystem::UpdateMetrics()
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_metrics.loadedMaterials = static_cast<int>(m_materials.size());
    m_metrics.textureCount = static_cast<int>(m_textureCache.size());

    // Count variants
    int totalVariants = 0;
    for (const auto& pair : m_materials)
    {
        if (pair.second)
        {
            totalVariants += static_cast<int>(pair.second->GetAvailableVariants().size());
        }
    }
    m_metrics.variantCount = totalVariants;
    m_metrics.hotReloadEnabled = m_hotReloadEnabled;
}

void MaterialSystem::PerformPeriodicMaintenance()
{
    // No-op on Linux - no GPU resources to manage
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
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

    using namespace Spark::HashLiterals;
    switch (Spark::FNV1a64(lower))
    {
    case "albedo"_hash64:
        return MaterialTextureType::Albedo;
    case "normal"_hash64:
        return MaterialTextureType::Normal;
    case "metallic"_hash64:
        return MaterialTextureType::Metallic;
    case "roughness"_hash64:
        return MaterialTextureType::Roughness;
    case "occlusion"_hash64:
        return MaterialTextureType::Occlusion;
    case "emissive"_hash64:
        return MaterialTextureType::Emissive;
    case "height"_hash64:
        return MaterialTextureType::Height;
    case "detailalbedo"_hash64:
        return MaterialTextureType::DetailAlbedo;
    case "detailnormal"_hash64:
        return MaterialTextureType::DetailNormal;
    case "subsurface"_hash64:
        return MaterialTextureType::Subsurface;
    case "transmission"_hash64:
        return MaterialTextureType::Transmission;
    case "clearcoat"_hash64:
        return MaterialTextureType::Clearcoat;
    case "clearcoatroughness"_hash64:
        return MaterialTextureType::ClearcoatRoughness;
    case "anisotropy"_hash64:
        return MaterialTextureType::Anisotropy;
    case "custom0"_hash64:
        return MaterialTextureType::Custom0;
    case "custom1"_hash64:
        return MaterialTextureType::Custom1;
    case "custom2"_hash64:
        return MaterialTextureType::Custom2;
    case "custom3"_hash64:
        return MaterialTextureType::Custom3;
    default:
        return MaterialTextureType::Albedo;
    }
}

#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// PLATFORM-INDEPENDENT IMPLEMENTATIONS
// ============================================================================

std::vector<std::string> MaterialSystem::GetShaderPermutation(const std::string& name) const
{
    auto material = GetMaterial(name);
    if (material && material != m_defaultMaterial)
    {
        return material->GetShaderPermutation();
    }
    return {};
}
