/**
 * @file DeferredLightingPass.h
 * @brief Fullscreen deferred lighting accumulation from G-Buffer
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

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace Spark::Graphics
{

    static constexpr uint32_t DEFERRED_MAX_DIR_LIGHTS = 4;
    static constexpr uint32_t DEFERRED_MAX_POINT_LIGHTS = 128;
    static constexpr uint32_t DEFERRED_MAX_SPOT_LIGHTS = 64;

    struct DeferredDirLight
    {
        XMFLOAT3 direction;
        float intensity;
        XMFLOAT3 color;
        float padding;
    };

    struct DeferredPointLight
    {
        XMFLOAT3 position;
        float radius;
        XMFLOAT3 color;
        float intensity;
    };

    struct DeferredSpotLight
    {
        XMFLOAT3 position;
        float radius;
        XMFLOAT3 direction;
        float cosOuterAngle;
        XMFLOAT3 color;
        float cosInnerAngle;
        float intensity;
        float padding[3];
    };

    struct DeferredLightingSettings
    {
        bool enableAmbient = true;
        XMFLOAT3 ambientColor = {0.03f, 0.03f, 0.03f};
        bool enableSSAO = true;
        bool enableIBL = true;
    };

    /**
     * @class DeferredLightingPass
     * @brief Accumulates lighting from the G-Buffer using a fullscreen pass
     *
     * G-Buffer layout (matching GraphicsEngine's MRT setup):
     * - RT0: Albedo.rgb + Metallic (R8G8B8A8_UNORM)
     * - RT1: Normal.xyz encoded (R16G16B16A16_FLOAT)
     * - RT2: WorldPos.xyz + Roughness (R16G16B16A16_FLOAT)
     * - RT3: Emissive.rgb + AO (R8G8B8A8_UNORM)
     * - Depth: D24S8
     */
    class DeferredLightingPass
    {
      public:
        DeferredLightingPass() = default;
        ~DeferredLightingPass() = default;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                        uint32_t width, uint32_t height)
        {
            m_device = device;
            m_context = context;
            m_width = width;
            m_height = height;

            if (!CreateResources())
                return false;
            if (!CompileShaders())
                return false;

            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            m_lightingPS.Reset();
            m_fullscreenVS.Reset();
            m_lightCB.Reset();
            m_cameraCB.Reset();
            m_lightOutputTexture.Reset();
            m_lightOutputRTV.Reset();
            m_lightOutputSRV.Reset();
            m_pointSampler.Reset();
            m_linearSampler.Reset();
            m_initialized = false;
        }

        /**
         * @brief Execute deferred lighting pass
         * @param gbufferSRVs Array of 4 G-Buffer SRVs (albedo, normal, worldPos, emissive)
         * @param depthSRV Depth buffer SRV
         * @param ssaoSRV Optional SSAO occlusion texture
         * @param irradianceSRV Optional IBL irradiance cubemap
         * @param prefilteredSRV Optional IBL prefiltered cubemap
         * @param brdfLUTSRV Optional BRDF integration LUT
         */
        void Execute(ID3D11ShaderResourceView* gbufferSRVs[4],
                     ID3D11ShaderResourceView* depthSRV,
                     ID3D11ShaderResourceView* ssaoSRV,
                     ID3D11ShaderResourceView* irradianceSRV,
                     ID3D11ShaderResourceView* prefilteredSRV,
                     ID3D11ShaderResourceView* brdfLUTSRV,
                     const XMFLOAT3& cameraPosition,
                     const XMMATRIX& invViewProj)
        {
            if (!m_initialized)
                return;

            UpdateCameraConstants(cameraPosition, invViewProj);
            UpdateLightConstants();

            // Set output render target
            m_context->OMSetRenderTargets(1, m_lightOutputRTV.GetAddressOf(), nullptr);

            float clearColor[] = {0, 0, 0, 1};
            m_context->ClearRenderTargetView(m_lightOutputRTV.Get(), clearColor);

            // Bind G-Buffer textures
            ID3D11ShaderResourceView* srvs[8] = {};
            for (int i = 0; i < 4; i++)
                srvs[i] = gbufferSRVs[i];
            srvs[4] = depthSRV;
            srvs[5] = ssaoSRV;
            srvs[6] = irradianceSRV;
            srvs[7] = prefilteredSRV;
            m_context->PSSetShaderResources(0, 8, srvs);

            // BRDF LUT on slot 8
            if (brdfLUTSRV)
                m_context->PSSetShaderResources(8, 1, &brdfLUTSRV);

            // Bind samplers
            ID3D11SamplerState* samplers[] = {m_pointSampler.Get(), m_linearSampler.Get()};
            m_context->PSSetSamplers(0, 2, samplers);

            // Bind constant buffers
            ID3D11Buffer* cbs[] = {m_cameraCB.Get(), m_lightCB.Get()};
            m_context->PSSetConstantBuffers(0, 2, cbs);

            // Draw fullscreen triangle
            m_context->IASetInputLayout(nullptr);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
            m_context->PSSetShader(m_lightingPS.Get(), nullptr, 0);
            m_context->Draw(3, 0);

            // Unbind
            ID3D11ShaderResourceView* nullSRVs[9] = {};
            m_context->PSSetShaderResources(0, 9, nullSRVs);
        }

        void SetDirectionalLights(const DeferredDirLight* lights, uint32_t count)
        {
            m_numDirLights = (count < DEFERRED_MAX_DIR_LIGHTS) ? count : DEFERRED_MAX_DIR_LIGHTS;
            for (uint32_t i = 0; i < m_numDirLights; i++)
                m_dirLights[i] = lights[i];
        }

        void SetPointLights(const DeferredPointLight* lights, uint32_t count)
        {
            m_numPointLights = (count < DEFERRED_MAX_POINT_LIGHTS) ? count : DEFERRED_MAX_POINT_LIGHTS;
            for (uint32_t i = 0; i < m_numPointLights; i++)
                m_pointLights[i] = lights[i];
        }

        void SetSpotLights(const DeferredSpotLight* lights, uint32_t count)
        {
            m_numSpotLights = (count < DEFERRED_MAX_SPOT_LIGHTS) ? count : DEFERRED_MAX_SPOT_LIGHTS;
            for (uint32_t i = 0; i < m_numSpotLights; i++)
                m_spotLights[i] = lights[i];
        }

        ID3D11ShaderResourceView* GetOutputSRV() const { return m_lightOutputSRV.Get(); }
        DeferredLightingSettings& GetSettings() { return m_settings; }

        std::string Console_GetStatus() const
        {
            std::string s = "Deferred Lighting Pass:\n";
            s += "  Dir lights: " + std::to_string(m_numDirLights) + "\n";
            s += "  Point lights: " + std::to_string(m_numPointLights) + "\n";
            s += "  Spot lights: " + std::to_string(m_numSpotLights) + "\n";
            s += "  IBL: " + std::string(m_settings.enableIBL ? "On" : "Off") + "\n";
            return s;
        }

      private:
        struct CameraCB
        {
            XMFLOAT4X4 invViewProj;
            XMFLOAT3 cameraPosition;
            float padding;
            XMFLOAT4 screenParams; // width, height, 1/width, 1/height
        };

        // Light constant buffer (must fit in 4096 float4s = 65536 bytes)
        // Reduced light counts to fit
        struct LightCB
        {
            XMFLOAT4 ambientColor; // xyz=color, w=enable
            uint32_t numDirLights;
            uint32_t numPointLights;
            uint32_t numSpotLights;
            uint32_t enableSSAO;
            DeferredDirLight dirLights[DEFERRED_MAX_DIR_LIGHTS];
            DeferredPointLight pointLights[DEFERRED_MAX_POINT_LIGHTS];
            DeferredSpotLight spotLights[DEFERRED_MAX_SPOT_LIGHTS];
        };

        bool CreateResources()
        {
            // Light accumulation render target
            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width = m_width;
            texDesc.Height = m_height;
            texDesc.MipLevels = 1;
            texDesc.ArraySize = 1;
            texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_DEFAULT;
            texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            m_device->CreateTexture2D(&texDesc, nullptr, &m_lightOutputTexture);
            m_device->CreateRenderTargetView(m_lightOutputTexture.Get(), nullptr, &m_lightOutputRTV);
            m_device->CreateShaderResourceView(m_lightOutputTexture.Get(), nullptr, &m_lightOutputSRV);

            // Constant buffers
            D3D11_BUFFER_DESC cbDesc = {};
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            cbDesc.ByteWidth = sizeof(CameraCB);
            m_device->CreateBuffer(&cbDesc, nullptr, &m_cameraCB);

            cbDesc.ByteWidth = sizeof(LightCB);
            m_device->CreateBuffer(&cbDesc, nullptr, &m_lightCB);

            // Samplers
            D3D11_SAMPLER_DESC sampDesc = {};
            sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            m_device->CreateSamplerState(&sampDesc, &m_pointSampler);

            sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            m_device->CreateSamplerState(&sampDesc, &m_linearSampler);

            return true;
        }

        bool CompileShaders()
        {
            // Fullscreen triangle VS
            const char* vsSource = R"(
                struct VSOutput
                {
                    float4 position : SV_POSITION;
                    float2 uv : TEXCOORD0;
                };

                VSOutput main(uint vertexID : SV_VertexID)
                {
                    VSOutput output;
                    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
                    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
                    return output;
                }
            )";

            ComPtr<ID3DBlob> blob, errorBlob;
            HRESULT hr = D3DCompile(vsSource, strlen(vsSource), "DeferredVS",
                                     nullptr, nullptr, "main", "vs_5_0", 0, 0,
                                     &blob, &errorBlob);
            if (FAILED(hr)) return false;
            m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                          nullptr, &m_fullscreenVS);

            // Deferred lighting pixel shader
            const char* psSource = R"(
                static const float PI = 3.14159265359;
                static const uint MAX_DIR = 4;
                static const uint MAX_POINT = 128;
                static const uint MAX_SPOT = 64;

                struct DirLight
                {
                    float3 direction;
                    float intensity;
                    float3 color;
                    float padding;
                };

                struct PointLight
                {
                    float3 position;
                    float radius;
                    float3 color;
                    float intensity;
                };

                struct SpotLight
                {
                    float3 position;
                    float radius;
                    float3 direction;
                    float cosOuterAngle;
                    float3 color;
                    float cosInnerAngle;
                    float intensity;
                    float3 padding;
                };

                cbuffer CameraCB : register(b0)
                {
                    float4x4 invViewProj;
                    float3 cameraPosition;
                    float cameraPad;
                    float4 screenParams;
                };

                cbuffer LightCB : register(b1)
                {
                    float4 ambientColor;
                    uint numDirLights;
                    uint numPointLights;
                    uint numSpotLights;
                    uint enableSSAO;
                    DirLight dirLights[MAX_DIR];
                    PointLight pointLights[MAX_POINT];
                    SpotLight spotLights[MAX_SPOT];
                };

                // G-Buffer
                Texture2D<float4> GBuffer0 : register(t0); // Albedo.rgb + Metallic
                Texture2D<float4> GBuffer1 : register(t1); // Normal.xyz + unused
                Texture2D<float4> GBuffer2 : register(t2); // WorldPos.xyz + Roughness
                Texture2D<float4> GBuffer3 : register(t3); // Emissive.rgb + AO
                Texture2D<float>  DepthTex  : register(t4);
                Texture2D<float>  SSAOTex   : register(t5);
                TextureCube<float4> IrradianceMap  : register(t6);
                TextureCube<float4> PrefilteredMap : register(t7);
                Texture2D<float2>  BRDFLUT  : register(t8);

                SamplerState PointSamp : register(s0);
                SamplerState LinearSamp : register(s1);

                // PBR functions
                float DistributionGGX(float3 N, float3 H, float roughness)
                {
                    float a = roughness * roughness;
                    float a2 = a * a;
                    float NdotH = max(dot(N, H), 0.0);
                    float NdotH2 = NdotH * NdotH;
                    float denom = NdotH2 * (a2 - 1.0) + 1.0;
                    return a2 / (PI * denom * denom);
                }

                float GeometrySchlickGGX(float NdotV, float roughness)
                {
                    float r = roughness + 1.0;
                    float k = (r * r) / 8.0;
                    return NdotV / (NdotV * (1.0 - k) + k);
                }

                float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
                {
                    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness) *
                           GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
                }

                float3 FresnelSchlick(float cosTheta, float3 F0)
                {
                    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
                }

                float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
                {
                    return F0 + (max(1.0 - roughness, F0) - F0) *
                           pow(saturate(1.0 - cosTheta), 5.0);
                }

                float3 ComputeDirectional(DirLight light, float3 N, float3 V,
                                           float3 albedo, float metallic, float roughness,
                                           float3 F0)
                {
                    float3 L = normalize(-light.direction);
                    float3 H = normalize(V + L);

                    float NdotL = max(dot(N, L), 0.0);
                    float D = DistributionGGX(N, H, roughness);
                    float G = GeometrySmith(N, V, L, roughness);
                    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

                    float3 kD = (1.0 - F) * (1.0 - metallic);
                    float3 specular = D * G * F /
                                      (4.0 * max(dot(N, V), 0.0) * NdotL + 0.001);

                    return (kD * albedo / PI + specular) * light.color *
                           light.intensity * NdotL;
                }

                float3 ComputePoint(PointLight light, float3 worldPos, float3 N, float3 V,
                                     float3 albedo, float metallic, float roughness,
                                     float3 F0)
                {
                    float3 toLight = light.position - worldPos;
                    float dist = length(toLight);
                    if (dist > light.radius) return 0;

                    float3 L = toLight / dist;
                    float3 H = normalize(V + L);

                    float attenuation = 1.0 / (dist * dist + 1.0);
                    float falloff = saturate(1.0 - pow(dist / light.radius, 4.0));
                    falloff = falloff * falloff;
                    attenuation *= falloff;

                    float NdotL = max(dot(N, L), 0.0);
                    float D = DistributionGGX(N, H, roughness);
                    float G = GeometrySmith(N, V, L, roughness);
                    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

                    float3 kD = (1.0 - F) * (1.0 - metallic);
                    float3 specular = D * G * F /
                                      (4.0 * max(dot(N, V), 0.0) * NdotL + 0.001);

                    return (kD * albedo / PI + specular) * light.color *
                           light.intensity * attenuation * NdotL;
                }

                float3 ComputeSpot(SpotLight light, float3 worldPos, float3 N, float3 V,
                                    float3 albedo, float metallic, float roughness,
                                    float3 F0)
                {
                    float3 toLight = light.position - worldPos;
                    float dist = length(toLight);
                    if (dist > light.radius) return 0;

                    float3 L = toLight / dist;
                    float cosAngle = dot(L, normalize(-light.direction));

                    if (cosAngle < light.cosOuterAngle) return 0;

                    float spotFade = saturate((cosAngle - light.cosOuterAngle) /
                                             (light.cosInnerAngle - light.cosOuterAngle));
                    float attenuation = spotFade / (dist * dist + 1.0);

                    float3 H = normalize(V + L);
                    float NdotL = max(dot(N, L), 0.0);
                    float D = DistributionGGX(N, H, roughness);
                    float G = GeometrySmith(N, V, L, roughness);
                    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

                    float3 kD = (1.0 - F) * (1.0 - metallic);
                    float3 specular = D * G * F /
                                      (4.0 * max(dot(N, V), 0.0) * NdotL + 0.001);

                    return (kD * albedo / PI + specular) * light.color *
                           light.intensity * attenuation * NdotL;
                }

                struct PSInput
                {
                    float4 position : SV_POSITION;
                    float2 uv : TEXCOORD0;
                };

                float4 main(PSInput input) : SV_TARGET
                {
                    int2 pixelCoord = int2(input.position.xy);

                    // Sample G-Buffer
                    float4 gb0 = GBuffer0.Load(int3(pixelCoord, 0));
                    float4 gb1 = GBuffer1.Load(int3(pixelCoord, 0));
                    float4 gb2 = GBuffer2.Load(int3(pixelCoord, 0));
                    float4 gb3 = GBuffer3.Load(int3(pixelCoord, 0));

                    float3 albedo = gb0.rgb;
                    float metallic = gb0.a;
                    float3 N = normalize(gb1.xyz * 2.0 - 1.0);
                    float3 worldPos = gb2.xyz;
                    float roughness = gb2.w;
                    float3 emissive = gb3.rgb;
                    float ao = gb3.a;

                    // SSAO
                    if (enableSSAO > 0)
                    {
                        float ssao = SSAOTex.Sample(PointSamp, input.uv);
                        ao *= ssao;
                    }

                    float3 V = normalize(cameraPosition - worldPos);
                    float3 F0 = lerp(0.04, albedo, metallic);

                    // Accumulate direct lighting
                    float3 Lo = 0;

                    for (uint d = 0; d < numDirLights; d++)
                        Lo += ComputeDirectional(dirLights[d], N, V, albedo,
                                                  metallic, roughness, F0);

                    for (uint p = 0; p < numPointLights; p++)
                        Lo += ComputePoint(pointLights[p], worldPos, N, V, albedo,
                                            metallic, roughness, F0);

                    for (uint s = 0; s < numSpotLights; s++)
                        Lo += ComputeSpot(spotLights[s], worldPos, N, V, albedo,
                                           metallic, roughness, F0);

                    // IBL ambient
                    float3 ambient = ambientColor.rgb * albedo * ao;

                    // Image-based lighting (if available)
                    float NdotV = max(dot(N, V), 0.0);
                    float3 R = reflect(-V, N);

                    float3 F = FresnelSchlickRoughness(NdotV, F0, roughness);
                    float3 kD = (1.0 - F) * (1.0 - metallic);

                    float3 irradiance = IrradianceMap.SampleLevel(LinearSamp, N, 0).rgb;
                    float3 diffuseIBL = irradiance * albedo;

                    float3 prefilteredColor = PrefilteredMap.SampleLevel(LinearSamp, R,
                                               roughness * 4.0).rgb;
                    float2 envBRDF = BRDFLUT.Sample(LinearSamp,
                                     float2(NdotV, roughness));
                    float3 specularIBL = prefilteredColor * (F * envBRDF.x + envBRDF.y);

                    ambient += (kD * diffuseIBL + specularIBL) * ao;

                    float3 color = ambient + Lo + emissive;
                    return float4(color, 1.0);
                }
            )";

            hr = D3DCompile(psSource, strlen(psSource), "DeferredLightingPS",
                             nullptr, nullptr, "main", "ps_5_0", 0, 0,
                             &blob, &errorBlob);
            if (FAILED(hr)) return false;

            return SUCCEEDED(m_device->CreatePixelShader(blob->GetBufferPointer(),
                                                          blob->GetBufferSize(),
                                                          nullptr, &m_lightingPS));
        }

        void UpdateCameraConstants(const XMFLOAT3& camPos, const XMMATRIX& invVP)
        {
            CameraCB cb;
            XMStoreFloat4x4(&cb.invViewProj, XMMatrixTranspose(invVP));
            cb.cameraPosition = camPos;
            cb.padding = 0;
            cb.screenParams = {static_cast<float>(m_width), static_cast<float>(m_height),
                               1.0f / m_width, 1.0f / m_height};

            D3D11_MAPPED_SUBRESOURCE mapped;
            m_context->Map(m_cameraCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            memcpy(mapped.pData, &cb, sizeof(cb));
            m_context->Unmap(m_cameraCB.Get(), 0);
        }

        void UpdateLightConstants()
        {
            LightCB cb = {};
            cb.ambientColor = {m_settings.ambientColor.x, m_settings.ambientColor.y,
                               m_settings.ambientColor.z,
                               m_settings.enableAmbient ? 1.0f : 0.0f};
            cb.numDirLights = m_numDirLights;
            cb.numPointLights = m_numPointLights;
            cb.numSpotLights = m_numSpotLights;
            cb.enableSSAO = m_settings.enableSSAO ? 1 : 0;

            for (uint32_t i = 0; i < m_numDirLights; i++)
                cb.dirLights[i] = m_dirLights[i];
            for (uint32_t i = 0; i < m_numPointLights; i++)
                cb.pointLights[i] = m_pointLights[i];
            for (uint32_t i = 0; i < m_numSpotLights; i++)
                cb.spotLights[i] = m_spotLights[i];

            D3D11_MAPPED_SUBRESOURCE mapped;
            m_context->Map(m_lightCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            memcpy(mapped.pData, &cb, sizeof(cb));
            m_context->Unmap(m_lightCB.Get(), 0);
        }

        bool m_initialized = false;
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        uint32_t m_width = 1920, m_height = 1080;

        DeferredLightingSettings m_settings;

        uint32_t m_numDirLights = 0;
        uint32_t m_numPointLights = 0;
        uint32_t m_numSpotLights = 0;
        DeferredDirLight m_dirLights[DEFERRED_MAX_DIR_LIGHTS];
        DeferredPointLight m_pointLights[DEFERRED_MAX_POINT_LIGHTS];
        DeferredSpotLight m_spotLights[DEFERRED_MAX_SPOT_LIGHTS];

        ComPtr<ID3D11PixelShader> m_lightingPS;
        ComPtr<ID3D11VertexShader> m_fullscreenVS;
        ComPtr<ID3D11Buffer> m_lightCB;
        ComPtr<ID3D11Buffer> m_cameraCB;
        ComPtr<ID3D11Texture2D> m_lightOutputTexture;
        ComPtr<ID3D11RenderTargetView> m_lightOutputRTV;
        ComPtr<ID3D11ShaderResourceView> m_lightOutputSRV;
        ComPtr<ID3D11SamplerState> m_pointSampler;
        ComPtr<ID3D11SamplerState> m_linearSampler;
    };

} // namespace Spark::Graphics
