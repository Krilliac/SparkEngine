/**
 * @file ScreenSpaceEffectsGPU.h
 * @brief GPU dispatch implementations for SSAO, SSR, and contact shadows
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides the actual GPU compute/pixel shader dispatch for screen-space effects.
 * Uses the configuration from ScreenSpaceEffects.h and executes the shaders on the GPU.
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif

#include "ScreenSpaceEffects.h"
#include <cstdint>
#include <string>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace Spark::Graphics
{

    struct ScreenSpaceGPUMetrics
    {
        float ssaoTimeMs = 0.0f;
        float ssrTimeMs = 0.0f;
        float contactShadowTimeMs = 0.0f;
    };

    /**
     * @class ScreenSpaceEffectsGPU
     * @brief Executes screen-space effects on the GPU via compute/pixel shaders
     */
    class ScreenSpaceEffectsGPU
    {
      public:
        ScreenSpaceEffectsGPU() = default;
        ~ScreenSpaceEffectsGPU() = default;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                        uint32_t width, uint32_t height)
        {
            m_device = device;
            m_context = context;
            m_width = width;
            m_height = height;

            if (!CreateRenderTargets())
                return false;
            if (!CompileShaders())
                return false;
            if (!CreateResources())
                return false;

            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            m_ssaoTexture.Reset();
            m_ssaoRTV.Reset();
            m_ssaoSRV.Reset();
            m_ssaoBlurTexture.Reset();
            m_ssaoBlurRTV.Reset();
            m_ssaoBlurSRV.Reset();
            m_ssrTexture.Reset();
            m_ssrRTV.Reset();
            m_ssrSRV.Reset();
            m_ssaoPS.Reset();
            m_ssaoBlurPS.Reset();
            m_ssrPS.Reset();
            m_contactShadowPS.Reset();
            m_fullscreenVS.Reset();
            m_constantBuffer.Reset();
            m_kernelBuffer.Reset();
            m_noiseTexture.Reset();
            m_noiseSRV.Reset();
            m_initialized = false;
        }

        /**
         * @brief Execute SSAO pass
         * @param depthSRV Scene depth buffer
         * @param normalSRV G-buffer normal map (or reconstructed)
         * @param viewMatrix Current view matrix
         * @param projMatrix Current projection matrix
         */
        void RenderSSAO(ID3D11ShaderResourceView* depthSRV,
                         ID3D11ShaderResourceView* normalSRV,
                         const XMMATRIX& viewMatrix,
                         const XMMATRIX& projMatrix)
        {
            if (!m_initialized || !m_ssaoPS)
                return;

            // Set SSAO render target
            m_context->OMSetRenderTargets(1, m_ssaoRTV.GetAddressOf(), nullptr);

            float clearColor[] = {1, 1, 1, 1};
            m_context->ClearRenderTargetView(m_ssaoRTV.Get(), clearColor);

            // Update CB
            SSGPUCB cb = {};
            cb.screenSize = {static_cast<float>(m_width), static_cast<float>(m_height),
                              1.0f / m_width, 1.0f / m_height};

            XMFLOAT4X4 projMat;
            XMStoreFloat4x4(&projMat, projMatrix);
            cb.ssaoParams = {m_ssaoSettings.radius, m_ssaoSettings.bias,
                              m_ssaoSettings.intensity, m_ssaoSettings.power};
            cb.projParams = {projMat._11, projMat._22, projMat._33, projMat._43};
            cb.sampleParams = {static_cast<float>(m_ssaoSettings.GetSampleCount()),
                                static_cast<float>(m_ssaoSettings.noiseSize), 0.0f, 0.0f};

            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                memcpy(mapped.pData, &cb, sizeof(SSGPUCB));
                m_context->Unmap(m_constantBuffer.Get(), 0);
            }

            m_context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
            m_context->PSSetShader(m_ssaoPS.Get(), nullptr, 0);
            m_context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

            ID3D11ShaderResourceView* srvs[] = {depthSRV, normalSRV, m_noiseSRV.Get()};
            m_context->PSSetShaderResources(0, 3, srvs);

            ID3D11SamplerState* samplers[] = {m_pointClampSampler.Get(), m_linearWrapSampler.Get()};
            m_context->PSSetSamplers(0, 2, samplers);

            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->IASetInputLayout(nullptr);
            m_context->Draw(3, 0);

            // Blur pass
            if (m_ssaoSettings.blur && m_ssaoBlurPS)
            {
                m_context->OMSetRenderTargets(1, m_ssaoBlurRTV.GetAddressOf(), nullptr);
                m_context->PSSetShader(m_ssaoBlurPS.Get(), nullptr, 0);

                ID3D11ShaderResourceView* blurSRV = m_ssaoSRV.Get();
                m_context->PSSetShaderResources(0, 1, &blurSRV);
                m_context->Draw(3, 0);
            }
        }

        /**
         * @brief Execute SSR pass
         * @param colorSRV Scene color buffer
         * @param depthSRV Scene depth buffer
         * @param normalSRV G-buffer normals
         * @param viewMatrix View matrix
         * @param projMatrix Projection matrix
         */
        void RenderSSR(ID3D11ShaderResourceView* colorSRV,
                        ID3D11ShaderResourceView* depthSRV,
                        ID3D11ShaderResourceView* normalSRV,
                        const XMMATRIX& viewMatrix,
                        const XMMATRIX& projMatrix)
        {
            if (!m_initialized || !m_ssrPS)
                return;

            m_context->OMSetRenderTargets(1, m_ssrRTV.GetAddressOf(), nullptr);

            float clearColor[] = {0, 0, 0, 0};
            m_context->ClearRenderTargetView(m_ssrRTV.Get(), clearColor);

            SSGPUCB cb = {};
            cb.screenSize = {static_cast<float>(m_width), static_cast<float>(m_height),
                              1.0f / m_width, 1.0f / m_height};

            XMFLOAT4X4 viewMat, projMat;
            XMStoreFloat4x4(&viewMat, viewMatrix);
            XMStoreFloat4x4(&projMat, projMatrix);

            cb.ssrParams = {m_ssrSettings.maxDistance, m_ssrSettings.stepSize,
                             m_ssrSettings.thickness, static_cast<float>(m_ssrSettings.GetStepCount())};
            cb.ssrParams2 = {m_ssrSettings.fadeStart, m_ssrSettings.fadeEnd,
                              m_ssrSettings.roughnessThreshold, 0.0f};
            cb.projParams = {projMat._11, projMat._22, projMat._33, projMat._43};

            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                memcpy(mapped.pData, &cb, sizeof(SSGPUCB));
                m_context->Unmap(m_constantBuffer.Get(), 0);
            }

            m_context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
            m_context->PSSetShader(m_ssrPS.Get(), nullptr, 0);
            m_context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

            ID3D11ShaderResourceView* srvs[] = {colorSRV, depthSRV, normalSRV};
            m_context->PSSetShaderResources(0, 3, srvs);

            ID3D11SamplerState* samplers[] = {m_pointClampSampler.Get(), m_linearClampSampler.Get()};
            m_context->PSSetSamplers(0, 2, samplers);

            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->IASetInputLayout(nullptr);
            m_context->Draw(3, 0);
        }

        /** @brief Get SSAO output texture */
        ID3D11ShaderResourceView* GetSSAOOutput() const
        {
            return m_ssaoSettings.blur ? m_ssaoBlurSRV.Get() : m_ssaoSRV.Get();
        }

        /** @brief Get SSR output texture */
        ID3D11ShaderResourceView* GetSSROutput() const { return m_ssrSRV.Get(); }

        SSAOSettings& GetSSAOSettings() { return m_ssaoSettings; }
        SSRSettings& GetSSRSettings() { return m_ssrSettings; }
        const ScreenSpaceGPUMetrics& GetMetrics() const { return m_metrics; }

      private:
        bool CreateRenderTargets()
        {
            // SSAO render target (single channel)
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = m_ssaoSettings.IsHalfRes() ? m_width / 2 : m_width;
            desc.Height = m_ssaoSettings.IsHalfRes() ? m_height / 2 : m_height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_ssaoTexture);
            if (FAILED(hr)) return false;
            m_device->CreateRenderTargetView(m_ssaoTexture.Get(), nullptr, &m_ssaoRTV);
            m_device->CreateShaderResourceView(m_ssaoTexture.Get(), nullptr, &m_ssaoSRV);

            // SSAO blur target
            hr = m_device->CreateTexture2D(&desc, nullptr, &m_ssaoBlurTexture);
            if (FAILED(hr)) return false;
            m_device->CreateRenderTargetView(m_ssaoBlurTexture.Get(), nullptr, &m_ssaoBlurRTV);
            m_device->CreateShaderResourceView(m_ssaoBlurTexture.Get(), nullptr, &m_ssaoBlurSRV);

            // SSR render target (full color)
            desc.Width = m_width;
            desc.Height = m_height;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            hr = m_device->CreateTexture2D(&desc, nullptr, &m_ssrTexture);
            if (FAILED(hr)) return false;
            m_device->CreateRenderTargetView(m_ssrTexture.Get(), nullptr, &m_ssrRTV);
            m_device->CreateShaderResourceView(m_ssrTexture.Get(), nullptr, &m_ssrSRV);

            return true;
        }

        bool CompileShaders()
        {
            // Fullscreen VS
            const char* vsSource = R"(
                struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
                VSOut main(uint id : SV_VertexID) {
                    VSOut o;
                    o.uv = float2((id << 1) & 2, id & 2);
                    o.pos = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1);
                    return o;
                }
            )";

            // SSAO pixel shader
            const char* ssaoPS = R"(
                Texture2D depthTex : register(t0);
                Texture2D normalTex : register(t1);
                Texture2D noiseTex : register(t2);
                SamplerState pointSamp : register(s0);
                SamplerState wrapSamp : register(s1);

                cbuffer SSGPUCB : register(b0) {
                    float4 screenSize;
                    float4 ssaoParams;   // radius, bias, intensity, power
                    float4 projParams;   // proj11, proj22, proj33, proj43
                    float4 sampleParams; // sampleCount, noiseSize, 0, 0
                    float4 ssrParams;
                    float4 ssrParams2;
                };

                float LinearizeDepth(float d) {
                    return projParams.w / (d - projParams.z);
                }

                // Hemisphere samples baked into shader
                static const float3 kernel[32] = {
                    float3(0.0296, 0.0437, 0.0017), float3(-0.0283, 0.0183, 0.0389),
                    float3(0.0352, -0.0321, 0.0125), float3(-0.0154, -0.0381, 0.0272),
                    float3(0.0465, 0.0199, 0.0343), float3(-0.0189, 0.0535, 0.0120),
                    float3(0.0621, -0.0123, 0.0452), float3(-0.0423, -0.0578, 0.0091),
                    float3(0.0734, 0.0387, 0.0568), float3(-0.0591, 0.0657, 0.0293),
                    float3(0.0843, -0.0487, 0.0712), float3(-0.0712, -0.0821, 0.0451),
                    float3(0.1023, 0.0567, 0.0834), float3(-0.0878, 0.0943, 0.0521),
                    float3(0.1189, -0.0723, 0.0978), float3(-0.1056, -0.1123, 0.0678),
                    float3(0.1345, 0.0834, 0.1123), float3(-0.1212, 0.1234, 0.0789),
                    float3(0.1567, -0.0956, 0.1345), float3(-0.1389, -0.1456, 0.0923),
                    float3(0.1789, 0.1123, 0.1567), float3(-0.1623, 0.1567, 0.1089),
                    float3(0.2012, -0.1234, 0.1789), float3(-0.1867, -0.1789, 0.1234),
                    float3(0.2345, 0.1456, 0.2012), float3(-0.2123, 0.1923, 0.1423),
                    float3(0.2678, -0.1567, 0.2345), float3(-0.2456, -0.2123, 0.1678),
                    float3(0.3012, 0.1789, 0.2678), float3(-0.2789, 0.2345, 0.1923),
                    float3(0.3456, -0.2012, 0.3012), float3(-0.3234, -0.2567, 0.2234)
                };

                float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                    float depth = depthTex.Sample(pointSamp, uv).r;
                    if (depth >= 1.0) return float4(1, 1, 1, 1);

                    float linearDepth = LinearizeDepth(depth);
                    float3 normal = normalTex.Sample(pointSamp, uv).rgb * 2.0 - 1.0;

                    // Random rotation from noise texture
                    float2 noiseScale = screenSize.xy / sampleParams.y;
                    float3 randomVec = noiseTex.Sample(wrapSamp, uv * noiseScale).rgb * 2.0 - 1.0;

                    // Tangent space basis
                    float3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
                    float3 bitangent = cross(normal, tangent);
                    float3x3 TBN = float3x3(tangent, bitangent, normal);

                    float occlusion = 0.0;
                    int samples = (int)sampleParams.x;
                    float radius = ssaoParams.x;
                    float bias = ssaoParams.y;

                    for (int i = 0; i < samples && i < 32; i++) {
                        float3 sampleDir = mul(kernel[i], TBN);
                        float3 samplePos = float3(uv, linearDepth) + float3(sampleDir.xy * radius / linearDepth, sampleDir.z * radius);

                        // Project sample to screen space
                        float2 sampleUV = samplePos.xy;
                        float sampleExpectedDepth = samplePos.z;

                        float sampleActualDepth = LinearizeDepth(depthTex.Sample(pointSamp, sampleUV).r);

                        float rangeCheck = smoothstep(0.0, 1.0, radius / abs(linearDepth - sampleActualDepth));
                        occlusion += (sampleActualDepth <= sampleExpectedDepth - bias ? 1.0 : 0.0) * rangeCheck;
                    }

                    occlusion = 1.0 - (occlusion / max(samples, 1));
                    occlusion = pow(saturate(occlusion), ssaoParams.w) * ssaoParams.z;
                    return float4(occlusion, occlusion, occlusion, 1);
                }
            )";

            // SSAO blur shader (edge-aware bilateral)
            const char* ssaoBlurPS = R"(
                Texture2D ssaoTex : register(t0);
                SamplerState pointSamp : register(s0);
                cbuffer SSGPUCB : register(b0) { float4 screenSize; float4 ssaoParams; float4 projParams; float4 sampleParams; float4 ssrParams; float4 ssrParams2; };

                float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                    float result = 0;
                    float totalWeight = 0;
                    float centerAO = ssaoTex.Sample(pointSamp, uv).r;

                    for (int x = -2; x <= 2; x++) {
                        for (int y = -2; y <= 2; y++) {
                            float2 offset = float2(x, y) * screenSize.zw;
                            float ao = ssaoTex.Sample(pointSamp, uv + offset).r;
                            float w = exp(-float(x*x + y*y) / 4.0);
                            w *= 1.0 / (1.0 + abs(ao - centerAO) * 8.0);
                            result += ao * w;
                            totalWeight += w;
                        }
                    }
                    result /= totalWeight;
                    return float4(result, result, result, 1);
                }
            )";

            // SSR pixel shader (screen-space ray marching)
            const char* ssrPS = R"(
                Texture2D colorTex : register(t0);
                Texture2D depthTex : register(t1);
                Texture2D normalTex : register(t2);
                SamplerState pointSamp : register(s0);
                SamplerState linearSamp : register(s1);

                cbuffer SSGPUCB : register(b0) {
                    float4 screenSize;
                    float4 ssaoParams;
                    float4 projParams;
                    float4 sampleParams;
                    float4 ssrParams;   // maxDist, stepSize, thickness, maxSteps
                    float4 ssrParams2;  // fadeStart, fadeEnd, roughThresh, 0
                };

                float LinearizeDepth(float d) { return projParams.w / (d - projParams.z); }

                float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                    float depth = depthTex.Sample(pointSamp, uv).r;
                    if (depth >= 1.0) return float4(0, 0, 0, 0);

                    float3 normal = normalize(normalTex.Sample(pointSamp, uv).rgb * 2.0 - 1.0);
                    float linearDepth = LinearizeDepth(depth);

                    // View-space position reconstruction
                    float2 ndc = uv * 2.0 - 1.0;
                    ndc.y = -ndc.y;
                    float3 viewPos = float3(ndc.x / projParams.x, ndc.y / projParams.y, 1.0) * linearDepth;
                    float3 viewDir = normalize(viewPos);
                    float3 reflDir = reflect(viewDir, normal);

                    // Ray march in screen space
                    float stepSize = ssrParams.y;
                    int maxSteps = (int)ssrParams.w;
                    float3 rayPos = viewPos;

                    for (int i = 0; i < maxSteps; i++) {
                        rayPos += reflDir * stepSize;

                        // Project to screen
                        float2 projUV;
                        projUV.x = (rayPos.x * projParams.x / rayPos.z) * 0.5 + 0.5;
                        projUV.y = (-rayPos.y * projParams.y / rayPos.z) * 0.5 + 0.5;

                        if (any(projUV < 0) || any(projUV > 1))
                            return float4(0, 0, 0, 0);

                        float sampleDepth = LinearizeDepth(depthTex.Sample(pointSamp, projUV).r);
                        float depthDiff = rayPos.z - sampleDepth;

                        if (depthDiff > 0 && depthDiff < ssrParams.z) {
                            // Hit! Fetch reflected color
                            float3 hitColor = colorTex.Sample(linearSamp, projUV).rgb;

                            // Edge fade
                            float2 edgeDist = abs(projUV - 0.5) * 2.0;
                            float edgeFade = 1.0 - smoothstep(ssrParams2.x, ssrParams2.y, max(edgeDist.x, edgeDist.y));

                            // Distance fade
                            float distFade = 1.0 - saturate(float(i) / float(maxSteps));

                            return float4(hitColor * edgeFade * distFade, edgeFade * distFade);
                        }

                        // Increase step size over distance
                        stepSize *= 1.05;
                    }

                    return float4(0, 0, 0, 0);
                }
            )";

            // Compile shaders
            ComPtr<ID3DBlob> blob, errBlob;

            if (SUCCEEDED(D3DCompile(vsSource, strlen(vsSource), "SSVS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &blob, &errBlob)))
                m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_fullscreenVS);

            if (SUCCEEDED(D3DCompile(ssaoPS, strlen(ssaoPS), "SSAO", nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errBlob)))
                m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_ssaoPS);

            if (SUCCEEDED(D3DCompile(ssaoBlurPS, strlen(ssaoBlurPS), "SSAOBlur", nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errBlob)))
                m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_ssaoBlurPS);

            if (SUCCEEDED(D3DCompile(ssrPS, strlen(ssrPS), "SSR", nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errBlob)))
                m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_ssrPS);

            return m_fullscreenVS && m_ssaoPS;
        }

        bool CreateResources()
        {
            // Constant buffer
            D3D11_BUFFER_DESC cbDesc = {};
            cbDesc.ByteWidth = sizeof(SSGPUCB);
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);

            // Noise texture (4x4 random rotation vectors)
            auto noiseData = SSAOKernel::GenerateNoiseTexture(4);
            std::vector<uint8_t> noisePixels(4 * 4 * 4);
            for (int i = 0; i < 16; ++i)
            {
                noisePixels[i * 4 + 0] = static_cast<uint8_t>((noiseData[i].x * 0.5f + 0.5f) * 255);
                noisePixels[i * 4 + 1] = static_cast<uint8_t>((noiseData[i].y * 0.5f + 0.5f) * 255);
                noisePixels[i * 4 + 2] = 128;
                noisePixels[i * 4 + 3] = 255;
            }

            D3D11_TEXTURE2D_DESC noiseDesc = {};
            noiseDesc.Width = 4;
            noiseDesc.Height = 4;
            noiseDesc.MipLevels = 1;
            noiseDesc.ArraySize = 1;
            noiseDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            noiseDesc.SampleDesc.Count = 1;
            noiseDesc.Usage = D3D11_USAGE_DEFAULT;
            noiseDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA initData = {};
            initData.pSysMem = noisePixels.data();
            initData.SysMemPitch = 4 * 4;
            m_device->CreateTexture2D(&noiseDesc, &initData, &m_noiseTexture);
            m_device->CreateShaderResourceView(m_noiseTexture.Get(), nullptr, &m_noiseSRV);

            // Samplers
            D3D11_SAMPLER_DESC sampDesc = {};
            sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            m_device->CreateSamplerState(&sampDesc, &m_pointClampSampler);

            sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            m_device->CreateSamplerState(&sampDesc, &m_linearClampSampler);

            sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
            sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
            m_device->CreateSamplerState(&sampDesc, &m_linearWrapSampler);

            return true;
        }

        struct SSGPUCB
        {
            XMFLOAT4 screenSize;
            XMFLOAT4 ssaoParams;
            XMFLOAT4 projParams;
            XMFLOAT4 sampleParams;
            XMFLOAT4 ssrParams;
            XMFLOAT4 ssrParams2;
        };

        bool m_initialized = false;
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        uint32_t m_width = 1920;
        uint32_t m_height = 1080;

        SSAOSettings m_ssaoSettings;
        SSRSettings m_ssrSettings;
        ScreenSpaceGPUMetrics m_metrics;

        // SSAO targets
        ComPtr<ID3D11Texture2D> m_ssaoTexture;
        ComPtr<ID3D11RenderTargetView> m_ssaoRTV;
        ComPtr<ID3D11ShaderResourceView> m_ssaoSRV;
        ComPtr<ID3D11Texture2D> m_ssaoBlurTexture;
        ComPtr<ID3D11RenderTargetView> m_ssaoBlurRTV;
        ComPtr<ID3D11ShaderResourceView> m_ssaoBlurSRV;

        // SSR targets
        ComPtr<ID3D11Texture2D> m_ssrTexture;
        ComPtr<ID3D11RenderTargetView> m_ssrRTV;
        ComPtr<ID3D11ShaderResourceView> m_ssrSRV;

        // Shaders
        ComPtr<ID3D11VertexShader> m_fullscreenVS;
        ComPtr<ID3D11PixelShader> m_ssaoPS;
        ComPtr<ID3D11PixelShader> m_ssaoBlurPS;
        ComPtr<ID3D11PixelShader> m_ssrPS;
        ComPtr<ID3D11PixelShader> m_contactShadowPS;

        // Resources
        ComPtr<ID3D11Buffer> m_constantBuffer;
        ComPtr<ID3D11Buffer> m_kernelBuffer;
        ComPtr<ID3D11Texture2D> m_noiseTexture;
        ComPtr<ID3D11ShaderResourceView> m_noiseSRV;
        ComPtr<ID3D11SamplerState> m_pointClampSampler;
        ComPtr<ID3D11SamplerState> m_linearClampSampler;
        ComPtr<ID3D11SamplerState> m_linearWrapSampler;
    };

} // namespace Spark::Graphics
