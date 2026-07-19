/**
 * @file LightingSystemInternalWindowsIBL.cpp
 * @brief Windows/D3D11 image-based lighting generation for LightingSystem
 *
 * GenerateIrradianceMap / GeneratePrefilterMap / GenerateBRDFLUT /
 * CreateDefaultEnvironment split out of LightingSystemInternalWindows.cpp
 * (which keeps constant buffer creation, shadow map management, and
 * per-frame buffer updates). Linux stubs live in
 * LightingSystemInternalLinux.cpp.
 */
#include "Core/Platform.h"
#include "Utils/MathUtils.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "LightingSystem.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

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
    float skyR = m_environmentLighting.skyColor.x * m_environmentLighting.skyIntensity * MathUtils::PI;
    float skyG = m_environmentLighting.skyColor.y * m_environmentLighting.skyIntensity * MathUtils::PI;
    float skyB = m_environmentLighting.skyColor.z * m_environmentLighting.skyIntensity * MathUtils::PI;
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

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Irradiance map generated (%ux%u cubemap)", irradianceSize,
                   irradianceSize);
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

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Prefilter map generated (%ux%u, %u mip levels)", prefilterSize,
                   prefilterSize, mipLevels);
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
                float phi = MathUtils::TWO_PI * u1;
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

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "BRDF LUT generated (%ux%u, %u samples)", lutSize, lutSize,
                   sampleCount);
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

    SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                   "Default environment created with IBL textures (sky=%.1f,%.1f,%.1f intensity=%.1f)",
                   m_environmentLighting.skyColor.x, m_environmentLighting.skyColor.y, m_environmentLighting.skyColor.z,
                   m_environmentLighting.skyIntensity);
    Spark::SimpleConsole::GetInstance().LogSuccess("Default environment created with IBL textures");
    return S_OK;
}

#endif // SPARK_PLATFORM_WINDOWS
