/**
 * @file MaterialSystemWindows.cpp
 * @brief Windows/D3D11 MaterialSystem methods — texture loading, binding, samplers, GPU state
 *
 * Contains LoadTexture, UnloadTexture, GetSampler, BindMaterial,
 * CreateDefaultMaterials, CreateSampler, GetFileTimestamp, PerformPeriodicMaintenance.
 * Platform-independent code (lifecycle, CRUD, metrics) stays in MaterialSystem.cpp.
 * Linux counterpart lives in MaterialSystemLinux.cpp.
 */

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "MaterialSystem.h"
#include "../Utils/SparkConsole.h"
#include "Utils/LocalFileCache.h"
#include <cstring>
#include <filesystem>

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

#endif // SPARK_PLATFORM_WINDOWS
