/**
 * @file MaterialSystemLinux.cpp
 * @brief Linux MaterialSystem methods — RHI-based stubs for texture/sampler/binding
 *
 * Contains LoadTexture, UnloadTexture, GetSampler, BindMaterial,
 * CreateDefaultMaterials, CreateSampler, GetFileTimestamp, LoadTextureFromFile,
 * PerformPeriodicMaintenance — all using RHI abstractions or no-op stubs.
 * Platform-independent code (lifecycle, CRUD, metrics) stays in MaterialSystem.cpp.
 * Windows counterpart lives in MaterialSystemWindows.cpp.
 */

#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "MaterialSystem.h"
#include "RHI/RHI.h"
#include <sys/stat.h>

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
    // No-op on Linux — no GPU resources to manage
}

#endif // !SPARK_PLATFORM_WINDOWS
