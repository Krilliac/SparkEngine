/**
 * @file AdvancedBRDF.h
 * @brief Advanced BRDF shader terms for PBR materials
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides GPU pixel shader implementations for advanced material BRDF lobes:
 * subsurface scattering (approximation), clearcoat, anisotropy, transmission,
 * sheen, and iridescence. These supplement the standard Cook-Torrance GGX BRDF.
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

    struct AdvancedBRDFParams
    {
        // Subsurface
        XMFLOAT3 subsurfaceColor;
        float subsurfaceRadius;

        // Clearcoat
        float clearcoatFactor;
        float clearcoatRoughness;
        float padding0[2];

        // Anisotropy
        float anisotropyFactor;
        XMFLOAT2 anisotropyDirection;
        float padding1;

        // Transmission
        XMFLOAT3 transmissionColor;
        float transmissionFactor;

        // Sheen
        XMFLOAT3 sheenColor;
        float sheenRoughness;

        // Iridescence
        float iridescenceFactor;
        float iridescenceIOR;
        float iridescenceThickness;
        float padding2;

        // Feature flags (packed as floats for HLSL compatibility)
        float enableSubsurface;
        float enableClearcoat;
        float enableAnisotropy;
        float enableTransmission;
        float enableSheen;
        float enableIridescence;
        float padding3[2];
    };

    /**
     * @class AdvancedBRDFSystem
     * @brief Manages compilation and binding of advanced BRDF shader code
     *
     * The HLSL functions are designed to be called from the main PBR pixel shader.
     * Each BRDF lobe is gated by a feature flag so disabled lobes have zero cost.
     */
    class AdvancedBRDFSystem
    {
      public:
        AdvancedBRDFSystem() = default;
        ~AdvancedBRDFSystem() = default;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
        {
            m_device = device;
            m_context = context;

            if (!CreateConstantBuffer())
                return false;
            if (!CompileShader())
                return false;

            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            m_advancedBRDFPS.Reset();
            m_paramsCB.Reset();
            m_initialized = false;
        }

        /**
         * @brief Bind advanced BRDF parameters and shader for material rendering
         */
        void Bind(const AdvancedBRDFParams& params)
        {
            if (!m_initialized)
                return;

            D3D11_MAPPED_SUBRESOURCE mapped;
            m_context->Map(m_paramsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            memcpy(mapped.pData, &params, sizeof(params));
            m_context->Unmap(m_paramsCB.Get(), 0);

            // Bind to slot b3 (b0=perFrame, b1=perObject, b2=perMaterial)
            m_context->PSSetConstantBuffers(3, 1, m_paramsCB.GetAddressOf());
        }

        /** @brief Get the advanced BRDF pixel shader for compositing */
        ID3D11PixelShader* GetShader() const { return m_advancedBRDFPS.Get(); }

        /**
         * @brief Get the HLSL include string for advanced BRDF functions
         *
         * This can be #included or appended to the main PBR pixel shader source.
         */
        static const char* GetHLSLSource()
        {
            return R"(
                // ============================================================
                // Advanced BRDF Functions
                // ============================================================

                cbuffer AdvancedBRDFCB : register(b3)
                {
                    float3 cb_subsurfaceColor;
                    float cb_subsurfaceRadius;
                    float cb_clearcoatFactor;
                    float cb_clearcoatRoughness;
                    float2 cb_padding0;
                    float cb_anisotropyFactor;
                    float2 cb_anisotropyDirection;
                    float cb_padding1;
                    float3 cb_transmissionColor;
                    float cb_transmissionFactor;
                    float3 cb_sheenColor;
                    float cb_sheenRoughness;
                    float cb_iridescenceFactor;
                    float cb_iridescenceIOR;
                    float cb_iridescenceThickness;
                    float cb_padding2;
                    float cb_enableSubsurface;
                    float cb_enableClearcoat;
                    float cb_enableAnisotropy;
                    float cb_enableTransmission;
                    float cb_enableSheen;
                    float cb_enableIridescence;
                    float2 cb_padding3;
                };

                static const float PI_BRDF = 3.14159265359;

                // ---- Subsurface Scattering (Approximation) ----
                // Burley normalized diffusion profile approximation
                float3 SubsurfaceBRDF(float3 N, float3 L, float3 V, float3 albedo)
                {
                    if (cb_enableSubsurface < 0.5) return 0;

                    float NdotL = dot(N, L);
                    float NdotV = dot(N, V);

                    // Wrap lighting for subsurface approximation
                    float wrapFactor = 0.5;
                    float wrappedNdotL = (NdotL + wrapFactor) / (1.0 + wrapFactor);
                    wrappedNdotL = max(wrappedNdotL, 0.0);

                    // View-dependent subsurface (transmittance approximation)
                    float3 H = normalize(L + N * 0.3);
                    float VdotH = pow(saturate(dot(V, -H)), 3.0);
                    float3 backscatter = VdotH * cb_subsurfaceColor;

                    // Combine forward scatter and backscatter
                    float3 sss = albedo * cb_subsurfaceColor * wrappedNdotL
                                 + backscatter * cb_subsurfaceRadius;

                    return sss;
                }

                // ---- Clearcoat ----
                // Separate GGX specular lobe for clearcoat layer
                float ClearcoatDistribution(float NdotH, float roughness)
                {
                    float a2 = roughness * roughness;
                    a2 = max(a2, 0.001);
                    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
                    return a2 / (PI_BRDF * denom * denom);
                }

                float ClearcoatGeometry(float NdotV, float NdotL, float roughness)
                {
                    float k = roughness * 0.5;
                    float gv = NdotV / (NdotV * (1.0 - k) + k);
                    float gl = NdotL / (NdotL * (1.0 - k) + k);
                    return gv * gl;
                }

                float3 ClearcoatBRDF(float3 N, float3 L, float3 V)
                {
                    if (cb_enableClearcoat < 0.5) return 0;

                    float3 H = normalize(V + L);
                    float NdotH = max(dot(N, H), 0.0);
                    float NdotV = max(dot(N, V), 0.001);
                    float NdotL = max(dot(N, L), 0.0);
                    float VdotH = max(dot(V, H), 0.0);

                    float D = ClearcoatDistribution(NdotH, cb_clearcoatRoughness);
                    float G = ClearcoatGeometry(NdotV, NdotL, cb_clearcoatRoughness);

                    // Fresnel for clearcoat (IOR ~1.5, F0 = 0.04)
                    float F0 = 0.04;
                    float F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

                    float spec = D * G * F / (4.0 * NdotV * NdotL + 0.001);
                    return spec * cb_clearcoatFactor;
                }

                // ---- Anisotropy ----
                // Anisotropic GGX distribution (Burley)
                float AnisotropicDistribution(float3 N, float3 H, float3 T, float3 B,
                                               float roughness, float aniso)
                {
                    float at = max(roughness * (1.0 + aniso), 0.001);
                    float ab = max(roughness * (1.0 - aniso), 0.001);

                    float TdotH = dot(T, H);
                    float BdotH = dot(B, H);
                    float NdotH = dot(N, H);

                    float a2 = at * ab;
                    float3 v = float3(ab * TdotH, at * BdotH, a2 * NdotH);
                    float v2 = dot(v, v);
                    float w2 = a2 / v2;
                    return a2 * w2 * w2 / PI_BRDF;
                }

                float3 AnisotropicBRDF(float3 N, float3 L, float3 V, float3 T, float3 B,
                                        float roughness, float3 F0)
                {
                    if (cb_enableAnisotropy < 0.5) return 0;

                    float3 H = normalize(V + L);
                    float NdotV = max(dot(N, V), 0.001);
                    float NdotL = max(dot(N, L), 0.0);
                    float VdotH = max(dot(V, H), 0.0);

                    float D = AnisotropicDistribution(N, H, T, B, roughness,
                                                       cb_anisotropyFactor);

                    // Standard Smith geometry term
                    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
                    float G = (NdotV / (NdotV * (1 - k) + k)) *
                              (NdotL / (NdotL * (1 - k) + k));

                    float3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

                    return D * G * F / (4.0 * NdotV * NdotL + 0.001);
                }

                // ---- Transmission ----
                // Thin-surface transmission approximation
                float3 TransmissionBRDF(float3 N, float3 L, float3 V, float3 albedo,
                                         float thickness)
                {
                    if (cb_enableTransmission < 0.5) return 0;

                    // Refracted light direction (simplified for thin surfaces)
                    float3 refractDir = refract(-V, N, 1.0 / 1.5);
                    float transmitDot = max(dot(refractDir, L), 0.0);

                    // Beer-Lambert absorption
                    float3 absorption = exp(-thickness * (1.0 - cb_transmissionColor));

                    return albedo * absorption * transmitDot * cb_transmissionFactor;
                }

                // ---- Sheen ----
                // Charlie sheen distribution (cloth/fabric appearance)
                float SheenDistribution(float NdotH, float roughness)
                {
                    float invAlpha = 1.0 / max(roughness, 0.001);
                    float cos2h = NdotH * NdotH;
                    float sin2h = max(1.0 - cos2h, 0.0078125);
                    return (2.0 + invAlpha) * pow(sin2h, invAlpha * 0.5) / (2.0 * PI_BRDF);
                }

                float3 SheenBRDF(float3 N, float3 L, float3 V)
                {
                    if (cb_enableSheen < 0.5) return 0;

                    float3 H = normalize(V + L);
                    float NdotH = max(dot(N, H), 0.0);
                    float NdotL = max(dot(N, L), 0.0);
                    float NdotV = max(dot(N, V), 0.001);

                    float D = SheenDistribution(NdotH, cb_sheenRoughness);
                    // Simplified visibility term for sheen
                    float V_sheen = 1.0 / (4.0 * (NdotL + NdotV - NdotL * NdotV));

                    return cb_sheenColor * D * V_sheen;
                }

                // ---- Iridescence ----
                // Thin-film interference based on thickness and IOR
                float3 IridescenceFresnel(float cosTheta, float ior, float thickness)
                {
                    // Thin-film optical path difference
                    float opd = 2.0 * ior * thickness * cosTheta;

                    // Wavelength-dependent phase shift (RGB approximation)
                    float3 wavelengths = float3(650.0, 510.0, 475.0); // nm
                    float3 phase = 2.0 * PI_BRDF * opd / wavelengths;

                    // Fresnel at outer interface
                    float R0 = (1.0 - ior) / (1.0 + ior);
                    R0 = R0 * R0;
                    float F = R0 + (1.0 - R0) * pow(1.0 - cosTheta, 5.0);

                    // Interference pattern
                    float3 interference = 0.5 + 0.5 * cos(phase);

                    return lerp(F, interference, 0.8);
                }

                float3 IridescenceBRDF(float3 N, float3 V)
                {
                    if (cb_enableIridescence < 0.5) return 0;

                    float NdotV = max(dot(N, V), 0.001);
                    float3 iridescence = IridescenceFresnel(NdotV, cb_iridescenceIOR,
                                                            cb_iridescenceThickness);
                    return iridescence * cb_iridescenceFactor;
                }

                // ---- Combined Advanced BRDF ----
                // Call this from the main PBR shader after the standard Cook-Torrance term
                float3 EvaluateAdvancedBRDF(float3 N, float3 L, float3 V, float3 T, float3 B,
                                             float3 albedo, float3 F0, float roughness)
                {
                    float3 result = 0;

                    result += SubsurfaceBRDF(N, L, V, albedo);
                    result += ClearcoatBRDF(N, L, V);
                    result += AnisotropicBRDF(N, L, V, T, B, roughness, F0);
                    result += TransmissionBRDF(N, L, V, albedo, 1.0);
                    result += SheenBRDF(N, L, V);

                    // Iridescence modifies the base Fresnel, applied multiplicatively
                    float3 iridescence = IridescenceBRDF(N, V);

                    return result + iridescence * albedo;
                }
            )";
        }

        std::string Console_GetStatus() const
        {
            return "Advanced BRDF System: " + std::string(m_initialized ? "Active" : "Inactive") + "\n"
                   "  Lobes: SSS, Clearcoat, Anisotropy, Transmission, Sheen, Iridescence\n";
        }

      private:
        bool CreateConstantBuffer()
        {
            D3D11_BUFFER_DESC cbDesc = {};
            cbDesc.ByteWidth = sizeof(AdvancedBRDFParams);
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            return SUCCEEDED(m_device->CreateBuffer(&cbDesc, nullptr, &m_paramsCB));
        }

        bool CompileShader()
        {
            // Combined advanced BRDF pixel shader
            std::string psSource = std::string(R"(
                Texture2D AlbedoTex : register(t0);
                Texture2D NormalTex : register(t1);
                Texture2D MetallicRoughnessTex : register(t2);
                SamplerState LinearSampler : register(s0);

                struct PSInput
                {
                    float4 position : SV_POSITION;
                    float2 uv : TEXCOORD0;
                    float3 worldPos : TEXCOORD1;
                    float3 normal : TEXCOORD2;
                    float3 tangent : TEXCOORD3;
                    float3 bitangent : TEXCOORD4;
                };
            )") + GetHLSLSource() + R"(
                float4 main(PSInput input) : SV_TARGET
                {
                    float3 N = normalize(input.normal);
                    float3 T = normalize(input.tangent);
                    float3 B = normalize(input.bitangent);
                    float3 V = normalize(float3(0, 0, 1) - input.worldPos); // Simplified

                    float3 albedo = AlbedoTex.Sample(LinearSampler, input.uv).rgb;
                    float4 mr = MetallicRoughnessTex.Sample(LinearSampler, input.uv);
                    float metallic = mr.b;
                    float roughness = mr.g;

                    float3 F0 = lerp(0.04, albedo, metallic);
                    float3 L = normalize(float3(1, 1, -1)); // Placeholder

                    float3 advanced = EvaluateAdvancedBRDF(N, L, V, T, B, albedo, F0, roughness);

                    return float4(advanced, 1.0);
                }
            )";

            ComPtr<ID3DBlob> psBlob, errorBlob;
            HRESULT hr = D3DCompile(psSource.c_str(), psSource.size(), "AdvancedBRDF_PS",
                                     nullptr, nullptr, "main", "ps_5_0", 0, 0,
                                     &psBlob, &errorBlob);
            if (FAILED(hr)) return false;

            return SUCCEEDED(m_device->CreatePixelShader(psBlob->GetBufferPointer(),
                                                          psBlob->GetBufferSize(),
                                                          nullptr, &m_advancedBRDFPS));
        }

        bool m_initialized = false;
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;

        ComPtr<ID3D11PixelShader> m_advancedBRDFPS;
        ComPtr<ID3D11Buffer> m_paramsCB;
    };

} // namespace Spark::Graphics
