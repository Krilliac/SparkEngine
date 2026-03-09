/**
 * @file IBLGenerator.h
 * @brief Image-Based Lighting generation: irradiance map, prefiltered environment, BRDF LUT
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <string>
#include <cstring>
#include <cmath>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace Spark::Graphics
{

    struct IBLSettings
    {
        uint32_t irradianceMapSize = 64;        ///< Irradiance cubemap face resolution
        uint32_t prefilteredMapSize = 256;       ///< Prefiltered env map face resolution
        uint32_t prefilteredMipLevels = 5;       ///< Number of roughness mip levels
        uint32_t brdfLUTSize = 512;              ///< BRDF integration LUT resolution
        uint32_t sampleCount = 1024;             ///< Monte Carlo sample count
    };

    /**
     * @class IBLGenerator
     * @brief Generates IBL textures for PBR environment lighting
     *
     * Given an environment cubemap, generates:
     * 1. Irradiance map — diffuse convolution (cosine-weighted hemisphere)
     * 2. Prefiltered environment map — specular convolution at multiple roughness levels
     * 3. BRDF integration LUT — split-sum approximation lookup texture
     */
    class IBLGenerator
    {
      public:
        IBLGenerator() = default;
        ~IBLGenerator() = default;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
        {
            m_device = device;
            m_context = context;

            if (!CompileShaders())
                return false;
            if (!CreateBRDFLUT())
                return false;

            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            m_irradianceCS.Reset();
            m_prefilterCS.Reset();
            m_brdfLutCS.Reset();
            m_iblCB.Reset();
            m_irradianceMap.Reset();
            m_irradianceSRV.Reset();
            m_prefilteredMap.Reset();
            m_prefilteredSRV.Reset();
            m_brdfLUT.Reset();
            m_brdfLUTSRV.Reset();
            m_initialized = false;
        }

        /**
         * @brief Generate irradiance and prefiltered maps from an environment cubemap
         */
        bool GenerateIBL(ID3D11ShaderResourceView* envCubemapSRV)
        {
            if (!m_initialized || !envCubemapSRV)
                return false;

            if (!GenerateIrradianceMap(envCubemapSRV))
                return false;
            if (!GeneratePrefilteredMap(envCubemapSRV))
                return false;

            return true;
        }

        ID3D11ShaderResourceView* GetIrradianceMapSRV() const { return m_irradianceSRV.Get(); }
        ID3D11ShaderResourceView* GetPrefilteredMapSRV() const { return m_prefilteredSRV.Get(); }
        ID3D11ShaderResourceView* GetBRDFLUTSRV() const { return m_brdfLUTSRV.Get(); }

        IBLSettings& GetSettings() { return m_settings; }

        std::string Console_GetStatus() const
        {
            std::string s = "IBL Generator:\n";
            s += "  Irradiance map: " + std::to_string(m_settings.irradianceMapSize) + "x"
                 + std::to_string(m_settings.irradianceMapSize) + "\n";
            s += "  Prefiltered map: " + std::to_string(m_settings.prefilteredMapSize) + "x"
                 + std::to_string(m_settings.prefilteredMapSize) + " ("
                 + std::to_string(m_settings.prefilteredMipLevels) + " mips)\n";
            s += "  BRDF LUT: " + std::to_string(m_settings.brdfLUTSize) + "x"
                 + std::to_string(m_settings.brdfLUTSize) + "\n";
            return s;
        }

      private:
        struct IBLCB
        {
            XMFLOAT4 params;     // x=roughness, y=sampleCount, z=faceSize, w=faceIndex
            XMFLOAT4 params2;    // x=mipLevel, y=totalMips, z=unused, w=unused
        };

        bool CompileShaders()
        {
            // Irradiance convolution compute shader
            const char* irradianceCSSource = R"(
                TextureCube<float4> EnvMap : register(t0);
                SamplerState LinearSampler : register(s0);
                RWTexture2DArray<float4> Output : register(u0);

                cbuffer IBLCB : register(b0)
                {
                    float4 params;   // x=roughness, y=sampleCount, z=faceSize, w=faceIndex
                    float4 params2;
                };

                static const float PI = 3.14159265359;

                float3 GetCubeDirection(uint face, float2 uv)
                {
                    // Map face index + UV to cube direction
                    float2 st = uv * 2.0 - 1.0;
                    float3 dir;
                    switch (face)
                    {
                        case 0: dir = float3( 1, -st.y, -st.x); break; // +X
                        case 1: dir = float3(-1, -st.y,  st.x); break; // -X
                        case 2: dir = float3( st.x,  1,  st.y); break; // +Y
                        case 3: dir = float3( st.x, -1, -st.y); break; // -Y
                        case 4: dir = float3( st.x, -st.y,  1); break; // +Z
                        case 5: dir = float3(-st.x, -st.y, -1); break; // -Z
                    }
                    return normalize(dir);
                }

                [numthreads(8, 8, 1)]
                void main(uint3 id : SV_DispatchThreadID)
                {
                    uint faceSize = (uint)params.z;
                    uint face = (uint)params.w;

                    if (id.x >= faceSize || id.y >= faceSize)
                        return;

                    float2 uv = (float2(id.xy) + 0.5) / float(faceSize);
                    float3 N = GetCubeDirection(face, uv);

                    // Cosine-weighted hemisphere integration
                    float3 up = abs(N.y) < 0.999 ? float3(0, 1, 0) : float3(0, 0, 1);
                    float3 right = normalize(cross(up, N));
                    up = cross(N, right);

                    float3 irradiance = 0;
                    float totalWeight = 0;
                    float sampleDelta = 0.025;

                    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
                    {
                        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
                        {
                            float3 tangentSample = float3(sin(theta) * cos(phi),
                                                          sin(theta) * sin(phi),
                                                          cos(theta));
                            float3 sampleDir = tangentSample.x * right +
                                               tangentSample.y * up +
                                               tangentSample.z * N;

                            irradiance += EnvMap.SampleLevel(LinearSampler, sampleDir, 0).rgb
                                          * cos(theta) * sin(theta);
                            totalWeight += 1.0;
                        }
                    }

                    irradiance = PI * irradiance / totalWeight;
                    Output[uint3(id.xy, face)] = float4(irradiance, 1.0);
                }
            )";

            // Prefiltered environment map compute shader (GGX importance sampling)
            const char* prefilterCSSource = R"(
                TextureCube<float4> EnvMap : register(t0);
                SamplerState LinearSampler : register(s0);
                RWTexture2DArray<float4> Output : register(u0);

                cbuffer IBLCB : register(b0)
                {
                    float4 params;   // x=roughness, y=sampleCount, z=faceSize, w=faceIndex
                    float4 params2;
                };

                static const float PI = 3.14159265359;

                float3 GetCubeDirection(uint face, float2 uv)
                {
                    float2 st = uv * 2.0 - 1.0;
                    float3 dir;
                    switch (face)
                    {
                        case 0: dir = float3( 1, -st.y, -st.x); break;
                        case 1: dir = float3(-1, -st.y,  st.x); break;
                        case 2: dir = float3( st.x,  1,  st.y); break;
                        case 3: dir = float3( st.x, -1, -st.y); break;
                        case 4: dir = float3( st.x, -st.y,  1); break;
                        case 5: dir = float3(-st.x, -st.y, -1); break;
                    }
                    return normalize(dir);
                }

                float RadicalInverse_VdC(uint bits)
                {
                    bits = (bits << 16u) | (bits >> 16u);
                    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
                    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
                    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
                    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
                    return float(bits) * 2.3283064365386963e-10;
                }

                float2 Hammersley(uint i, uint N)
                {
                    return float2(float(i) / float(N), RadicalInverse_VdC(i));
                }

                float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
                {
                    float a = roughness * roughness;

                    float phi = 2.0 * PI * Xi.x;
                    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
                    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

                    float3 H;
                    H.x = cos(phi) * sinTheta;
                    H.y = sin(phi) * sinTheta;
                    H.z = cosTheta;

                    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
                    float3 tangent = normalize(cross(up, N));
                    float3 bitangent = cross(N, tangent);

                    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
                }

                [numthreads(8, 8, 1)]
                void main(uint3 id : SV_DispatchThreadID)
                {
                    uint faceSize = (uint)params.z;
                    uint face = (uint)params.w;
                    float roughness = params.x;
                    uint sampleCount = (uint)params.y;

                    if (id.x >= faceSize || id.y >= faceSize)
                        return;

                    float2 uv = (float2(id.xy) + 0.5) / float(faceSize);
                    float3 N = GetCubeDirection(face, uv);
                    float3 R = N;
                    float3 V = R;

                    float3 prefilteredColor = 0;
                    float totalWeight = 0;

                    for (uint i = 0; i < sampleCount; i++)
                    {
                        float2 Xi = Hammersley(i, sampleCount);
                        float3 H = ImportanceSampleGGX(Xi, N, roughness);
                        float3 L = normalize(2.0 * dot(V, H) * H - V);

                        float NdotL = max(dot(N, L), 0.0);
                        if (NdotL > 0.0)
                        {
                            prefilteredColor += EnvMap.SampleLevel(LinearSampler, L, 0).rgb * NdotL;
                            totalWeight += NdotL;
                        }
                    }

                    prefilteredColor /= max(totalWeight, 0.001);
                    Output[uint3(id.xy, face)] = float4(prefilteredColor, 1.0);
                }
            )";

            // BRDF Integration LUT compute shader
            const char* brdfLutCSSource = R"(
                RWTexture2D<float2> Output : register(u0);

                static const float PI = 3.14159265359;

                float RadicalInverse_VdC(uint bits)
                {
                    bits = (bits << 16u) | (bits >> 16u);
                    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
                    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
                    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
                    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
                    return float(bits) * 2.3283064365386963e-10;
                }

                float2 Hammersley(uint i, uint N)
                {
                    return float2(float(i) / float(N), RadicalInverse_VdC(i));
                }

                float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
                {
                    float a = roughness * roughness;
                    float phi = 2.0 * PI * Xi.x;
                    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
                    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

                    float3 H;
                    H.x = cos(phi) * sinTheta;
                    H.y = sin(phi) * sinTheta;
                    H.z = cosTheta;

                    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
                    float3 tangent = normalize(cross(up, N));
                    float3 bitangent = cross(N, tangent);

                    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
                }

                float GeometrySchlickGGX(float NdotV, float roughness)
                {
                    float a = roughness;
                    float k = (a * a) / 2.0;
                    return NdotV / (NdotV * (1.0 - k) + k);
                }

                float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
                {
                    float NdotV = max(dot(N, V), 0.0);
                    float NdotL = max(dot(N, L), 0.0);
                    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
                }

                [numthreads(8, 8, 1)]
                void main(uint3 id : SV_DispatchThreadID)
                {
                    float NdotV = (float(id.x) + 0.5) / 512.0;
                    float roughness = (float(id.y) + 0.5) / 512.0;

                    NdotV = max(NdotV, 0.001);

                    float3 V;
                    V.x = sqrt(1.0 - NdotV * NdotV);
                    V.y = 0.0;
                    V.z = NdotV;

                    float3 N = float3(0, 0, 1);

                    float A = 0.0;
                    float B = 0.0;

                    const uint SAMPLE_COUNT = 1024;
                    for (uint i = 0; i < SAMPLE_COUNT; i++)
                    {
                        float2 Xi = Hammersley(i, SAMPLE_COUNT);
                        float3 H = ImportanceSampleGGX(Xi, N, roughness);
                        float3 L = normalize(2.0 * dot(V, H) * H - V);

                        float NdotL = max(L.z, 0.0);
                        float NdotH = max(H.z, 0.0);
                        float VdotH = max(dot(V, H), 0.0);

                        if (NdotL > 0.0)
                        {
                            float G = GeometrySmith(N, V, L, roughness);
                            float G_Vis = (G * VdotH) / (NdotH * NdotV);
                            float Fc = pow(1.0 - VdotH, 5.0);

                            A += (1.0 - Fc) * G_Vis;
                            B += Fc * G_Vis;
                        }
                    }

                    A /= float(SAMPLE_COUNT);
                    B /= float(SAMPLE_COUNT);

                    Output[id.xy] = float2(A, B);
                }
            )";

            ComPtr<ID3DBlob> blob, errorBlob;

            HRESULT hr = D3DCompile(irradianceCSSource, strlen(irradianceCSSource),
                                     "IrradianceCS", nullptr, nullptr, "main", "cs_5_0",
                                     0, 0, &blob, &errorBlob);
            if (FAILED(hr)) return false;
            m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                           nullptr, &m_irradianceCS);

            hr = D3DCompile(prefilterCSSource, strlen(prefilterCSSource),
                             "PrefilterCS", nullptr, nullptr, "main", "cs_5_0",
                             0, 0, &blob, &errorBlob);
            if (FAILED(hr)) return false;
            m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                           nullptr, &m_prefilterCS);

            hr = D3DCompile(brdfLutCSSource, strlen(brdfLutCSSource),
                             "BRDFLUTCS", nullptr, nullptr, "main", "cs_5_0",
                             0, 0, &blob, &errorBlob);
            if (FAILED(hr)) return false;
            m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                           nullptr, &m_brdfLutCS);

            // Create constant buffer
            D3D11_BUFFER_DESC cbDesc = {};
            cbDesc.ByteWidth = sizeof(IBLCB);
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            m_device->CreateBuffer(&cbDesc, nullptr, &m_iblCB);

            // Create linear sampler
            D3D11_SAMPLER_DESC sampDesc = {};
            sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            m_device->CreateSamplerState(&sampDesc, &m_linearSampler);

            return true;
        }

        bool GenerateIrradianceMap(ID3D11ShaderResourceView* envSRV)
        {
            uint32_t size = m_settings.irradianceMapSize;

            // Create irradiance cubemap
            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width = size;
            texDesc.Height = size;
            texDesc.MipLevels = 1;
            texDesc.ArraySize = 6;
            texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_DEFAULT;
            texDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            texDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

            HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_irradianceMap);
            if (FAILED(hr)) return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = texDesc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MipLevels = 1;
            m_device->CreateShaderResourceView(m_irradianceMap.Get(), &srvDesc, &m_irradianceSRV);

            // Create per-face UAVs and dispatch
            for (uint32_t face = 0; face < 6; face++)
            {
                ComPtr<ID3D11UnorderedAccessView> uav;
                D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
                uavDesc.Format = texDesc.Format;
                uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                uavDesc.Texture2DArray.FirstArraySlice = face;
                uavDesc.Texture2DArray.ArraySize = 1;
                m_device->CreateUnorderedAccessView(m_irradianceMap.Get(), &uavDesc, &uav);

                UpdateCB(0.0f, 0, size, face);

                m_context->CSSetShader(m_irradianceCS.Get(), nullptr, 0);
                m_context->CSSetConstantBuffers(0, 1, m_iblCB.GetAddressOf());
                m_context->CSSetShaderResources(0, 1, &envSRV);
                m_context->CSSetSamplers(0, 1, m_linearSampler.GetAddressOf());
                m_context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);

                m_context->Dispatch((size + 7) / 8, (size + 7) / 8, 1);

                ID3D11UnorderedAccessView* nullUAV = nullptr;
                m_context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
            }

            return true;
        }

        bool GeneratePrefilteredMap(ID3D11ShaderResourceView* envSRV)
        {
            uint32_t size = m_settings.prefilteredMapSize;
            uint32_t mipLevels = m_settings.prefilteredMipLevels;

            // Create prefiltered cubemap with mips
            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width = size;
            texDesc.Height = size;
            texDesc.MipLevels = mipLevels;
            texDesc.ArraySize = 6;
            texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_DEFAULT;
            texDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            texDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

            HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_prefilteredMap);
            if (FAILED(hr)) return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = texDesc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MipLevels = mipLevels;
            m_device->CreateShaderResourceView(m_prefilteredMap.Get(), &srvDesc, &m_prefilteredSRV);

            for (uint32_t mip = 0; mip < mipLevels; mip++)
            {
                uint32_t mipSize = size >> mip;
                if (mipSize < 1) mipSize = 1;
                float roughness = static_cast<float>(mip) / static_cast<float>(mipLevels - 1);

                for (uint32_t face = 0; face < 6; face++)
                {
                    ComPtr<ID3D11UnorderedAccessView> uav;
                    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
                    uavDesc.Format = texDesc.Format;
                    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                    uavDesc.Texture2DArray.MipSlice = mip;
                    uavDesc.Texture2DArray.FirstArraySlice = face;
                    uavDesc.Texture2DArray.ArraySize = 1;
                    m_device->CreateUnorderedAccessView(m_prefilteredMap.Get(), &uavDesc, &uav);

                    UpdateCB(roughness, m_settings.sampleCount, mipSize, face);

                    m_context->CSSetShader(m_prefilterCS.Get(), nullptr, 0);
                    m_context->CSSetConstantBuffers(0, 1, m_iblCB.GetAddressOf());
                    m_context->CSSetShaderResources(0, 1, &envSRV);
                    m_context->CSSetSamplers(0, 1, m_linearSampler.GetAddressOf());
                    m_context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);

                    m_context->Dispatch((mipSize + 7) / 8, (mipSize + 7) / 8, 1);

                    ID3D11UnorderedAccessView* nullUAV = nullptr;
                    m_context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
                }
            }

            return true;
        }

        bool CreateBRDFLUT()
        {
            uint32_t size = m_settings.brdfLUTSize;

            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width = size;
            texDesc.Height = size;
            texDesc.MipLevels = 1;
            texDesc.ArraySize = 1;
            texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_DEFAULT;
            texDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

            HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_brdfLUT);
            if (FAILED(hr)) return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = texDesc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            m_device->CreateShaderResourceView(m_brdfLUT.Get(), &srvDesc, &m_brdfLUTSRV);

            ComPtr<ID3D11UnorderedAccessView> uav;
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = texDesc.Format;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            m_device->CreateUnorderedAccessView(m_brdfLUT.Get(), &uavDesc, &uav);

            m_context->CSSetShader(m_brdfLutCS.Get(), nullptr, 0);
            m_context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
            m_context->Dispatch((size + 7) / 8, (size + 7) / 8, 1);

            ID3D11UnorderedAccessView* nullUAV = nullptr;
            m_context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);

            return true;
        }

        void UpdateCB(float roughness, uint32_t sampleCount, uint32_t faceSize, uint32_t faceIndex)
        {
            IBLCB cb;
            cb.params = {roughness, static_cast<float>(sampleCount),
                         static_cast<float>(faceSize), static_cast<float>(faceIndex)};
            cb.params2 = {0, 0, 0, 0};

            D3D11_MAPPED_SUBRESOURCE mapped;
            m_context->Map(m_iblCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            memcpy(mapped.pData, &cb, sizeof(cb));
            m_context->Unmap(m_iblCB.Get(), 0);
        }

        bool m_initialized = false;
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;

        IBLSettings m_settings;

        ComPtr<ID3D11ComputeShader> m_irradianceCS;
        ComPtr<ID3D11ComputeShader> m_prefilterCS;
        ComPtr<ID3D11ComputeShader> m_brdfLutCS;
        ComPtr<ID3D11Buffer> m_iblCB;
        ComPtr<ID3D11SamplerState> m_linearSampler;

        ComPtr<ID3D11Texture2D> m_irradianceMap;
        ComPtr<ID3D11ShaderResourceView> m_irradianceSRV;
        ComPtr<ID3D11Texture2D> m_prefilteredMap;
        ComPtr<ID3D11ShaderResourceView> m_prefilteredSRV;
        ComPtr<ID3D11Texture2D> m_brdfLUT;
        ComPtr<ID3D11ShaderResourceView> m_brdfLUTSRV;
    };

} // namespace Spark::Graphics
