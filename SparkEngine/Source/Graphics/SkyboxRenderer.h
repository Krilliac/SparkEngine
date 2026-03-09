/**
 * @file SkyboxRenderer.h
 * @brief Skybox and cubemap rendering system
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

    struct SkyboxSettings
    {
        bool enabled = true;
        float rotation = 0.0f;          ///< Y-axis rotation in radians
        float intensity = 1.0f;         ///< Sky brightness multiplier
        float blurLevel = 0.0f;         ///< Mip level for blurred sky (0=sharp)
        XMFLOAT3 tintColor = {1, 1, 1}; ///< Color tint
        bool renderStars = true;        ///< Render star field at night
        float starDensity = 500.0f;     ///< Number of visible stars
        float starBrightness = 1.0f;    ///< Star brightness multiplier
    };

    class SkyboxRenderer
    {
      public:
        SkyboxRenderer() = default;
        ~SkyboxRenderer() = default;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
        {
            m_device = device;
            m_context = context;

            if (!CompileShaders())
                return false;
            if (!CreateCubeGeometry())
                return false;
            if (!CreateResources())
                return false;

            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            m_skyboxVS.Reset();
            m_skyboxPS.Reset();
            m_proceduralSkyPS.Reset();
            m_cubeVB.Reset();
            m_cubeIB.Reset();
            m_inputLayout.Reset();
            m_constantBuffer.Reset();
            m_depthStencilState.Reset();
            m_rasterizerState.Reset();
            m_linearSampler.Reset();
            m_initialized = false;
        }

        /**
         * @brief Render skybox using a cubemap texture
         */
        void RenderCubemap(ID3D11ShaderResourceView* cubemapSRV, const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix)
        {
            if (!m_initialized || !cubemapSRV)
                return;

            // Remove translation from view matrix (skybox stays centered on camera)
            XMMATRIX viewNoTranslation = viewMatrix;
            viewNoTranslation.r[3] = XMVectorSet(0, 0, 0, 1);

            SkyboxCB cb = {};
            XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(viewNoTranslation * projMatrix));
            cb.params = {m_settings.intensity, m_settings.rotation, m_settings.blurLevel, 0.0f};
            cb.tintColor = {m_settings.tintColor.x, m_settings.tintColor.y, m_settings.tintColor.z, 1.0f};

            UpdateCB(cb);
            SetRenderState();

            m_context->VSSetShader(m_skyboxVS.Get(), nullptr, 0);
            m_context->PSSetShader(m_skyboxPS.Get(), nullptr, 0);
            m_context->PSSetShaderResources(0, 1, &cubemapSRV);
            m_context->PSSetSamplers(0, 1, m_linearSampler.GetAddressOf());
            m_context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
            m_context->VSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

            UINT stride = sizeof(XMFLOAT3);
            UINT offset = 0;
            m_context->IASetVertexBuffers(0, 1, m_cubeVB.GetAddressOf(), &stride, &offset);
            m_context->IASetIndexBuffer(m_cubeIB.Get(), DXGI_FORMAT_R16_UINT, 0);
            m_context->IASetInputLayout(m_inputLayout.Get());
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            m_context->DrawIndexed(36, 0, 0);
        }

        /**
         * @brief Render procedural gradient sky (fallback when no cubemap)
         */
        void RenderProceduralSky(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix, const XMFLOAT3& sunDirection,
                                 float sunIntensity, const XMFLOAT3& skyColor, const XMFLOAT3& horizonColor)
        {
            if (!m_initialized || !m_proceduralSkyPS)
                return;

            XMMATRIX viewNoTranslation = viewMatrix;
            viewNoTranslation.r[3] = XMVectorSet(0, 0, 0, 1);

            SkyboxCB cb = {};
            XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(viewNoTranslation * projMatrix));
            cb.params = {m_settings.intensity, m_settings.rotation, 0.0f, sunIntensity};
            cb.tintColor = {skyColor.x, skyColor.y, skyColor.z, 1.0f};
            cb.sunDirection = {sunDirection.x, sunDirection.y, sunDirection.z, 0.04f};
            cb.horizonColor = {horizonColor.x, horizonColor.y, horizonColor.z, 1.0f};

            UpdateCB(cb);
            SetRenderState();

            m_context->VSSetShader(m_skyboxVS.Get(), nullptr, 0);
            m_context->PSSetShader(m_proceduralSkyPS.Get(), nullptr, 0);
            m_context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
            m_context->VSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

            UINT stride = sizeof(XMFLOAT3);
            UINT offset = 0;
            m_context->IASetVertexBuffers(0, 1, m_cubeVB.GetAddressOf(), &stride, &offset);
            m_context->IASetIndexBuffer(m_cubeIB.Get(), DXGI_FORMAT_R16_UINT, 0);
            m_context->IASetInputLayout(m_inputLayout.Get());
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            m_context->DrawIndexed(36, 0, 0);
        }

        SkyboxSettings& GetSettings() { return m_settings; }

      private:
        struct SkyboxCB
        {
            XMFLOAT4X4 viewProj;
            XMFLOAT4 params; // intensity, rotation, blur, sunIntensity
            XMFLOAT4 tintColor;
            XMFLOAT4 sunDirection; // xyz=dir, w=sunSize
            XMFLOAT4 horizonColor;
        };

        void UpdateCB(const SkyboxCB& cb)
        {
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                memcpy(mapped.pData, &cb, sizeof(SkyboxCB));
                m_context->Unmap(m_constantBuffer.Get(), 0);
            }
        }

        void SetRenderState()
        {
            m_context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
            m_context->RSSetState(m_rasterizerState.Get());
        }

        bool CompileShaders()
        {
            const char* vsSource = R"(
                cbuffer SkyboxCB : register(b0) {
                    float4x4 viewProj;
                    float4 params;
                    float4 tintColor;
                    float4 sunDir;
                    float4 horizonColor;
                };
                struct VSOut { float4 pos : SV_Position; float3 localPos : TEXCOORD0; };
                VSOut main(float3 position : POSITION) {
                    VSOut o;
                    o.localPos = position;
                    float4 clipPos = mul(float4(position, 1.0), viewProj);
                    o.pos = clipPos.xyww; // Depth = 1.0 (far plane)
                    return o;
                }
            )";

            const char* cubemapPS = R"(
                TextureCube skyTexture : register(t0);
                SamplerState linearSampler : register(s0);
                cbuffer SkyboxCB : register(b0) {
                    float4x4 viewProj;
                    float4 params; // intensity, rotation, blur, sunIntensity
                    float4 tintColor;
                    float4 sunDir;
                    float4 horizonColor;
                };
                float4 main(float4 pos : SV_Position, float3 localPos : TEXCOORD0) : SV_Target {
                    // Apply rotation
                    float s = sin(params.y), c = cos(params.y);
                    float3 dir = float3(localPos.x * c - localPos.z * s, localPos.y, localPos.x * s + localPos.z * c);
                    float3 color = skyTexture.SampleLevel(linearSampler, dir, params.z).rgb;
                    color *= params.x * tintColor.rgb;
                    return float4(color, 1);
                }
            )";

            const char* proceduralPS = R"(
                cbuffer SkyboxCB : register(b0) {
                    float4x4 viewProj;
                    float4 params;
                    float4 tintColor;    // skyColor
                    float4 sunDir;       // xyz=direction, w=sunSize
                    float4 horizonColor;
                };
                float4 main(float4 pos : SV_Position, float3 localPos : TEXCOORD0) : SV_Target {
                    float3 dir = normalize(localPos);
                    float upDot = dir.y;

                    // Sky gradient
                    float3 sky = tintColor.rgb;
                    float3 horizon = horizonColor.rgb;
                    float3 ground = float3(0.1, 0.08, 0.06);

                    float3 color;
                    if (upDot > 0) {
                        float t = pow(saturate(upDot), 0.5);
                        color = lerp(horizon, sky, t);
                    } else {
                        float t = pow(saturate(-upDot), 0.3);
                        color = lerp(horizon, ground, t);
                    }

                    // Sun disk
                    float3 sunDirection = normalize(sunDir.xyz);
                    float sunDot = dot(dir, sunDirection);
                    float sunSize = sunDir.w;
                    if (sunDot > 1.0 - sunSize) {
                        float sunEdge = smoothstep(1.0 - sunSize, 1.0 - sunSize * 0.5, sunDot);
                        color += float3(1.0, 0.95, 0.8) * params.w * sunEdge;
                    }
                    // Sun glow
                    float glow = pow(saturate(sunDot), 8.0) * 0.3 * params.w;
                    color += float3(1.0, 0.8, 0.5) * glow;

                    color *= params.x;
                    return float4(color, 1);
                }
            )";

            ComPtr<ID3DBlob> blob, errBlob;

            HRESULT hr = D3DCompile(vsSource, strlen(vsSource), "SkyboxVS", nullptr, nullptr, "main", "vs_5_0", 0, 0,
                                    &blob, &errBlob);
            if (FAILED(hr))
                return false;
            m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_skyboxVS);

            // Input layout
            D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
            m_device->CreateInputLayout(layoutDesc, 1, blob->GetBufferPointer(), blob->GetBufferSize(), &m_inputLayout);

            if (SUCCEEDED(D3DCompile(cubemapPS, strlen(cubemapPS), "SkyboxPS", nullptr, nullptr, "main", "ps_5_0", 0, 0,
                                     &blob, &errBlob)))
                m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_skyboxPS);

            if (SUCCEEDED(D3DCompile(proceduralPS, strlen(proceduralPS), "ProceduralSkyPS", nullptr, nullptr, "main",
                                     "ps_5_0", 0, 0, &blob, &errBlob)))
                m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                            &m_proceduralSkyPS);

            return m_skyboxVS && m_skyboxPS;
        }

        bool CreateCubeGeometry()
        {
            XMFLOAT3 vertices[] = {{-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}, {1, -1, -1},
                                   {-1, -1, 1},  {-1, 1, 1},  {1, 1, 1},  {1, -1, 1}};

            uint16_t indices[] = {
                0, 1, 2, 0, 2, 3, // Front
                4, 6, 5, 4, 7, 6, // Back
                0, 5, 1, 0, 4, 5, // Left
                3, 2, 6, 3, 6, 7, // Right
                1, 5, 6, 1, 6, 2, // Top
                0, 3, 7, 0, 7, 4  // Bottom
            };

            D3D11_BUFFER_DESC vbDesc = {};
            vbDesc.ByteWidth = sizeof(vertices);
            vbDesc.Usage = D3D11_USAGE_DEFAULT;
            vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

            D3D11_SUBRESOURCE_DATA vbData = {};
            vbData.pSysMem = vertices;
            m_device->CreateBuffer(&vbDesc, &vbData, &m_cubeVB);

            D3D11_BUFFER_DESC ibDesc = {};
            ibDesc.ByteWidth = sizeof(indices);
            ibDesc.Usage = D3D11_USAGE_DEFAULT;
            ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

            D3D11_SUBRESOURCE_DATA ibData = {};
            ibData.pSysMem = indices;
            m_device->CreateBuffer(&ibDesc, &ibData, &m_cubeIB);

            return m_cubeVB && m_cubeIB;
        }

        bool CreateResources()
        {
            D3D11_BUFFER_DESC cbDesc = {};
            cbDesc.ByteWidth = sizeof(SkyboxCB);
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);

            // Depth stencil: test but don't write (skybox at max depth)
            D3D11_DEPTH_STENCIL_DESC dsDesc = {};
            dsDesc.DepthEnable = TRUE;
            dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
            m_device->CreateDepthStencilState(&dsDesc, &m_depthStencilState);

            // Rasterizer: cull front faces (we're inside the cube)
            D3D11_RASTERIZER_DESC rsDesc = {};
            rsDesc.FillMode = D3D11_FILL_SOLID;
            rsDesc.CullMode = D3D11_CULL_FRONT;
            rsDesc.FrontCounterClockwise = FALSE;
            rsDesc.DepthClipEnable = FALSE;
            m_device->CreateRasterizerState(&rsDesc, &m_rasterizerState);

            D3D11_SAMPLER_DESC sampDesc = {};
            sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            m_device->CreateSamplerState(&sampDesc, &m_linearSampler);

            return true;
        }

        bool m_initialized = false;
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        SkyboxSettings m_settings;

        ComPtr<ID3D11VertexShader> m_skyboxVS;
        ComPtr<ID3D11PixelShader> m_skyboxPS;
        ComPtr<ID3D11PixelShader> m_proceduralSkyPS;
        ComPtr<ID3D11Buffer> m_cubeVB;
        ComPtr<ID3D11Buffer> m_cubeIB;
        ComPtr<ID3D11InputLayout> m_inputLayout;
        ComPtr<ID3D11Buffer> m_constantBuffer;
        ComPtr<ID3D11DepthStencilState> m_depthStencilState;
        ComPtr<ID3D11RasterizerState> m_rasterizerState;
        ComPtr<ID3D11SamplerState> m_linearSampler;
    };

} // namespace Spark::Graphics
