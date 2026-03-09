/**
 * @file TessellationSystem.h
 * @brief Geometry and tessellation shader support for terrain and displacement
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

    enum class TessellationMode
    {
        None,          ///< No tessellation
        Uniform,       ///< Fixed tessellation factor
        DistanceBased, ///< Tessellation decreases with distance from camera
        ScreenSpace    ///< Target a fixed edge length in screen pixels
    };

    struct TessellationSettings
    {
        TessellationMode mode = TessellationMode::DistanceBased;
        float minTessFactor = 1.0f;
        float maxTessFactor = 64.0f;
        float targetEdgeLengthPx = 16.0f; ///< For screen-space mode
        float distanceStart = 10.0f;      ///< Distance at which max tess begins
        float distanceEnd = 100.0f;       ///< Distance at which min tess is used
        float displacementScale = 1.0f;   ///< Height map displacement magnitude
        float displacementBias = 0.0f;    ///< Displacement offset
        bool enablePhongSmoothing = true; ///< PN-triangle smoothing
        float phongAlpha = 0.75f;         ///< Phong smoothing factor
    };

    /**
     * @class TessellationSystem
     * @brief Manages hull/domain/geometry shader creation and binding
     *
     * Supports:
     * - Distance-based adaptive tessellation for terrain
     * - Displacement mapping via height textures
     * - PN-triangle smoothing for curved surfaces
     * - Geometry shader wireframe overlay for debugging
     */
    class TessellationSystem
    {
      public:
        TessellationSystem() = default;
        ~TessellationSystem() = default;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
        {
            m_device = device;
            m_context = context;

            if (!CreateConstantBuffer())
                return false;
            if (!CompileShaders())
                return false;

            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            m_hullShader.Reset();
            m_domainShader.Reset();
            m_wireframeGS.Reset();
            m_tessCB.Reset();
            m_initialized = false;
        }

        /**
         * @brief Bind tessellation stages for displacement-mapped rendering
         */
        void Bind(const XMFLOAT3& cameraPosition, const XMMATRIX& viewProj, ID3D11ShaderResourceView* heightMapSRV)
        {
            if (!m_initialized || m_settings.mode == TessellationMode::None)
                return;

            UpdateConstants(cameraPosition, viewProj);

            m_context->HSSetShader(m_hullShader.Get(), nullptr, 0);
            m_context->DSSetShader(m_domainShader.Get(), nullptr, 0);
            m_context->HSSetConstantBuffers(0, 1, m_tessCB.GetAddressOf());
            m_context->DSSetConstantBuffers(0, 1, m_tessCB.GetAddressOf());

            if (heightMapSRV)
            {
                m_context->DSSetShaderResources(0, 1, &heightMapSRV);
                D3D11_SAMPLER_DESC sampDesc = {};
                sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
                sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
                sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
                ComPtr<ID3D11SamplerState> sampler;
                m_device->CreateSamplerState(&sampDesc, &sampler);
                m_context->DSSetSamplers(0, 1, sampler.GetAddressOf());
            }

            // Set topology to patch list (3 control points per patch)
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        }

        /**
         * @brief Enable wireframe geometry shader overlay
         */
        void BindWireframe()
        {
            if (!m_initialized)
                return;
            m_context->GSSetShader(m_wireframeGS.Get(), nullptr, 0);
        }

        /**
         * @brief Unbind all tessellation/geometry stages
         */
        void Unbind()
        {
            m_context->HSSetShader(nullptr, nullptr, 0);
            m_context->DSSetShader(nullptr, nullptr, 0);
            m_context->GSSetShader(nullptr, nullptr, 0);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }

        TessellationSettings& GetSettings() { return m_settings; }

        std::string Console_GetStatus() const
        {
            std::string s = "Tessellation System:\n";
            s += "  Mode: ";
            switch (m_settings.mode)
            {
            case TessellationMode::None:
                s += "Disabled";
                break;
            case TessellationMode::Uniform:
                s += "Uniform";
                break;
            case TessellationMode::DistanceBased:
                s += "Distance-Based";
                break;
            case TessellationMode::ScreenSpace:
                s += "Screen-Space";
                break;
            }
            s += "\n  Tess range: [" + std::to_string(m_settings.minTessFactor) + ", " +
                 std::to_string(m_settings.maxTessFactor) + "]\n";
            s += "  Displacement: " + std::to_string(m_settings.displacementScale) + "\n";
            return s;
        }

      private:
        struct TessCB
        {
            XMFLOAT4X4 viewProjMatrix;
            XMFLOAT3 cameraPosition;
            float minTessFactor;
            float maxTessFactor;
            float distanceStart;
            float distanceEnd;
            float displacementScale;
            float displacementBias;
            float phongAlpha;
            float tessMode; // 0=uniform, 1=distance, 2=screenSpace
            float targetEdgeLength;
            XMFLOAT4 screenParams; // width, height, 1/w, 1/h
        };

        bool CreateConstantBuffer()
        {
            D3D11_BUFFER_DESC cbDesc = {};
            cbDesc.ByteWidth = sizeof(TessCB);
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            return SUCCEEDED(m_device->CreateBuffer(&cbDesc, nullptr, &m_tessCB));
        }

        bool CompileShaders()
        {
            // Hull shader (patch constant + control point functions)
            const char* hsSource = R"(
                cbuffer TessCB : register(b0)
                {
                    float4x4 viewProjMatrix;
                    float3 cameraPosition;
                    float minTessFactor;
                    float maxTessFactor;
                    float distanceStart;
                    float distanceEnd;
                    float displacementScale;
                    float displacementBias;
                    float phongAlpha;
                    float tessMode;
                    float targetEdgeLength;
                    float4 screenParams;
                };

                struct VSOutput
                {
                    float4 position : SV_POSITION;
                    float3 worldPos : TEXCOORD0;
                    float3 normal : TEXCOORD1;
                    float2 uv : TEXCOORD2;
                };

                struct HSOutput
                {
                    float4 position : SV_POSITION;
                    float3 worldPos : TEXCOORD0;
                    float3 normal : TEXCOORD1;
                    float2 uv : TEXCOORD2;
                };

                struct PatchConstants
                {
                    float edges[3] : SV_TessFactor;
                    float inside : SV_InsideTessFactor;
                };

                float CalcTessFactor(float3 p0, float3 p1)
                {
                    float3 midpoint = (p0 + p1) * 0.5;
                    float dist = distance(midpoint, cameraPosition);

                    if (tessMode < 0.5)
                    {
                        // Uniform
                        return maxTessFactor;
                    }
                    else if (tessMode < 1.5)
                    {
                        // Distance-based
                        float t = saturate((dist - distanceStart) /
                                           (distanceEnd - distanceStart));
                        return lerp(maxTessFactor, minTessFactor, t);
                    }
                    else
                    {
                        // Screen-space
                        float4 clip0 = mul(float4(p0, 1), viewProjMatrix);
                        float4 clip1 = mul(float4(p1, 1), viewProjMatrix);
                        float2 screen0 = clip0.xy / clip0.w * screenParams.xy * 0.5;
                        float2 screen1 = clip1.xy / clip1.w * screenParams.xy * 0.5;
                        float edgeLen = distance(screen0, screen1);
                        return clamp(edgeLen / targetEdgeLength, minTessFactor, maxTessFactor);
                    }
                }

                PatchConstants PatchConstantFunc(
                    InputPatch<VSOutput, 3> patch,
                    uint patchID : SV_PrimitiveID)
                {
                    PatchConstants pc;

                    pc.edges[0] = CalcTessFactor(patch[1].worldPos, patch[2].worldPos);
                    pc.edges[1] = CalcTessFactor(patch[2].worldPos, patch[0].worldPos);
                    pc.edges[2] = CalcTessFactor(patch[0].worldPos, patch[1].worldPos);
                    pc.inside = (pc.edges[0] + pc.edges[1] + pc.edges[2]) / 3.0;

                    return pc;
                }

                [domain("tri")]
                [partitioning("fractional_odd")]
                [outputtopology("triangle_cw")]
                [outputcontrolpoints(3)]
                [patchconstantfunc("PatchConstantFunc")]
                [maxtessfactor(64.0)]
                HSOutput main(
                    InputPatch<VSOutput, 3> patch,
                    uint id : SV_OutputControlPointID,
                    uint patchID : SV_PrimitiveID)
                {
                    HSOutput output;
                    output.position = patch[id].position;
                    output.worldPos = patch[id].worldPos;
                    output.normal = patch[id].normal;
                    output.uv = patch[id].uv;
                    return output;
                }
            )";

            // Domain shader with displacement mapping and PN-triangle smoothing
            const char* dsSource = R"(
                cbuffer TessCB : register(b0)
                {
                    float4x4 viewProjMatrix;
                    float3 cameraPosition;
                    float minTessFactor;
                    float maxTessFactor;
                    float distanceStart;
                    float distanceEnd;
                    float displacementScale;
                    float displacementBias;
                    float phongAlpha;
                    float tessMode;
                    float targetEdgeLength;
                    float4 screenParams;
                };

                Texture2D<float> HeightMap : register(t0);
                SamplerState HeightSampler : register(s0);

                struct HSOutput
                {
                    float4 position : SV_POSITION;
                    float3 worldPos : TEXCOORD0;
                    float3 normal : TEXCOORD1;
                    float2 uv : TEXCOORD2;
                };

                struct PatchConstants
                {
                    float edges[3] : SV_TessFactor;
                    float inside : SV_InsideTessFactor;
                };

                struct DSOutput
                {
                    float4 position : SV_POSITION;
                    float3 worldPos : TEXCOORD0;
                    float3 normal : TEXCOORD1;
                    float2 uv : TEXCOORD2;
                };

                // PN-triangle: project point onto tangent plane
                float3 ProjectToPlane(float3 point, float3 planePoint, float3 planeNormal)
                {
                    return point - dot(point - planePoint, planeNormal) * planeNormal;
                }

                [domain("tri")]
                DSOutput main(
                    PatchConstants pc,
                    float3 bary : SV_DomainLocation,
                    const OutputPatch<HSOutput, 3> patch)
                {
                    DSOutput output;

                    // Barycentric interpolation
                    float3 worldPos = bary.x * patch[0].worldPos +
                                      bary.y * patch[1].worldPos +
                                      bary.z * patch[2].worldPos;

                    float3 normal = normalize(bary.x * patch[0].normal +
                                              bary.y * patch[1].normal +
                                              bary.z * patch[2].normal);

                    float2 uv = bary.x * patch[0].uv +
                                bary.y * patch[1].uv +
                                bary.z * patch[2].uv;

                    // Phong tessellation (PN-triangle smoothing)
                    if (phongAlpha > 0.0)
                    {
                        float3 p0 = ProjectToPlane(worldPos, patch[0].worldPos,
                                                    normalize(patch[0].normal));
                        float3 p1 = ProjectToPlane(worldPos, patch[1].worldPos,
                                                    normalize(patch[1].normal));
                        float3 p2 = ProjectToPlane(worldPos, patch[2].worldPos,
                                                    normalize(patch[2].normal));

                        float3 phongPos = bary.x * p0 + bary.y * p1 + bary.z * p2;
                        worldPos = lerp(worldPos, phongPos, phongAlpha);
                    }

                    // Displacement mapping
                    float height = HeightMap.SampleLevel(HeightSampler, uv, 0);
                    worldPos += normal * (height * displacementScale + displacementBias);

                    output.position = mul(float4(worldPos, 1.0), viewProjMatrix);
                    output.worldPos = worldPos;
                    output.normal = normal;
                    output.uv = uv;

                    return output;
                }
            )";

            // Wireframe geometry shader
            const char* gsSource = R"(
                struct GSInput
                {
                    float4 position : SV_POSITION;
                    float3 worldPos : TEXCOORD0;
                    float3 normal : TEXCOORD1;
                    float2 uv : TEXCOORD2;
                };

                struct GSOutput
                {
                    float4 position : SV_POSITION;
                    float3 worldPos : TEXCOORD0;
                    float3 normal : TEXCOORD1;
                    float2 uv : TEXCOORD2;
                    float3 bary : TEXCOORD3;
                };

                [maxvertexcount(3)]
                void main(triangle GSInput input[3],
                          inout TriangleStream<GSOutput> stream)
                {
                    GSOutput output;

                    for (int i = 0; i < 3; i++)
                    {
                        output.position = input[i].position;
                        output.worldPos = input[i].worldPos;
                        output.normal = input[i].normal;
                        output.uv = input[i].uv;
                        output.bary = float3(i == 0, i == 1, i == 2);
                        stream.Append(output);
                    }
                    stream.RestartStrip();
                }
            )";

            ComPtr<ID3DBlob> blob, errorBlob;

            HRESULT hr = D3DCompile(hsSource, strlen(hsSource), "TessHS", nullptr, nullptr, "main", "hs_5_0", 0, 0,
                                    &blob, &errorBlob);
            if (FAILED(hr))
                return false;
            m_device->CreateHullShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_hullShader);

            hr = D3DCompile(dsSource, strlen(dsSource), "TessDS", nullptr, nullptr, "main", "ds_5_0", 0, 0, &blob,
                            &errorBlob);
            if (FAILED(hr))
                return false;
            m_device->CreateDomainShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_domainShader);

            hr = D3DCompile(gsSource, strlen(gsSource), "WireframeGS", nullptr, nullptr, "main", "gs_5_0", 0, 0, &blob,
                            &errorBlob);
            if (FAILED(hr))
                return false;
            m_device->CreateGeometryShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_wireframeGS);

            return true;
        }

        void UpdateConstants(const XMFLOAT3& camPos, const XMMATRIX& viewProj)
        {
            TessCB cb;
            XMStoreFloat4x4(&cb.viewProjMatrix, XMMatrixTranspose(viewProj));
            cb.cameraPosition = camPos;
            cb.minTessFactor = m_settings.minTessFactor;
            cb.maxTessFactor = m_settings.maxTessFactor;
            cb.distanceStart = m_settings.distanceStart;
            cb.distanceEnd = m_settings.distanceEnd;
            cb.displacementScale = m_settings.displacementScale;
            cb.displacementBias = m_settings.displacementBias;
            cb.phongAlpha = m_settings.enablePhongSmoothing ? m_settings.phongAlpha : 0.0f;
            cb.targetEdgeLength = m_settings.targetEdgeLengthPx;

            switch (m_settings.mode)
            {
            case TessellationMode::Uniform:
                cb.tessMode = 0.0f;
                break;
            case TessellationMode::DistanceBased:
                cb.tessMode = 1.0f;
                break;
            case TessellationMode::ScreenSpace:
                cb.tessMode = 2.0f;
                break;
            default:
                cb.tessMode = 1.0f;
                break;
            }

            cb.screenParams = {1920.0f, 1080.0f, 1.0f / 1920.0f, 1.0f / 1080.0f};

            D3D11_MAPPED_SUBRESOURCE mapped;
            m_context->Map(m_tessCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            memcpy(mapped.pData, &cb, sizeof(cb));
            m_context->Unmap(m_tessCB.Get(), 0);
        }

        bool m_initialized = false;
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;

        TessellationSettings m_settings;

        ComPtr<ID3D11HullShader> m_hullShader;
        ComPtr<ID3D11DomainShader> m_domainShader;
        ComPtr<ID3D11GeometryShader> m_wireframeGS;
        ComPtr<ID3D11Buffer> m_tessCB;
    };

} // namespace Spark::Graphics
