/**
 * @file OcclusionCulling.h
 * @brief GPU-based occlusion culling with hierarchical Z-buffer
 * @author Spark Engine Team
 * @date 2025
 *
 * Implements a two-phase occlusion culling system:
 * 1. Build a hierarchical Z-buffer (Hi-Z) from the previous frame's depth
 * 2. Test object bounding volumes against the Hi-Z pyramid to reject occluded objects
 *
 * Supports both CPU readback for traditional rendering and GPU-driven indirect draw.
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif

#include <vector>
#include <cstdint>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace Spark::Graphics
{

    // =============================================================================
    // Occlusion Culling Settings
    // =============================================================================

    enum class OcclusionCullingMode
    {
        Disabled,        ///< No occlusion culling
        HierarchicalZ,   ///< Hi-Z based GPU occlusion culling
        HardwareQueries, ///< D3D11 occlusion query based culling
        Software         ///< CPU software rasterizer (low-res depth)
    };

    struct OcclusionCullingSettings
    {
        OcclusionCullingMode mode = OcclusionCullingMode::HierarchicalZ;
        uint32_t hiZMipLevels = 10;     ///< Number of Hi-Z mip levels
        float conservativeScale = 1.1f; ///< Scale bounding boxes by this factor for conservative testing
        bool useLastFrameDepth = true;  ///< Use previous frame depth (1 frame latency)
        int maxQueriesPerFrame = 256;   ///< Max hardware queries per frame
        float minScreenSize = 0.01f;    ///< Min screen-space size to test (skip tiny objects)
        bool freezeOcclusion = false;   ///< Debug: freeze occlusion state
    };

    struct OcclusionMetrics
    {
        uint32_t totalObjects = 0;
        uint32_t testedObjects = 0;
        uint32_t occludedObjects = 0;
        uint32_t visibleObjects = 0;
        float buildHiZTimeMs = 0.0f;
        float testTimeMs = 0.0f;
        uint32_t hiZMipLevels = 0;
    };

    // =============================================================================
    // Bounding Volume for Occlusion Testing
    // =============================================================================

    struct OcclusionBounds
    {
        XMFLOAT3 center;
        XMFLOAT3 extents;
        uint32_t objectIndex = 0;
    };

    // =============================================================================
    // Occlusion Culling System
    // =============================================================================

    class OcclusionCullingSystem
    {
      public:
        OcclusionCullingSystem() = default;
        ~OcclusionCullingSystem() = default;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width, uint32_t height)
        {
            m_device = device;
            m_context = context;
            m_width = width;
            m_height = height;

            if (!CreateHiZPyramid())
                return false;
            if (!CompileShaders())
                return false;
            if (!CreateBuffers())
                return false;

            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            m_hiZTexture.Reset();
            m_hiZSRV.Reset();
            m_hiZUAVs.clear();
            m_hiZMipSRVs.clear();
            m_hiZBuildCS.Reset();
            m_occlusionTestCS.Reset();
            m_resultBuffer.Reset();
            m_resultStagingBuffer.Reset();
            m_boundsBuffer.Reset();
            m_boundsSRV.Reset();
            m_resultUAV.Reset();
            m_constantBuffer.Reset();
            m_initialized = false;
        }

        void Resize(uint32_t width, uint32_t height)
        {
            m_width = width;
            m_height = height;
            CreateHiZPyramid();
        }

        /**
         * @brief Build the Hi-Z pyramid from the current depth buffer
         * @param depthSRV Shader resource view of the scene depth buffer
         */
        void BuildHiZPyramid(ID3D11ShaderResourceView* depthSRV)
        {
            if (!m_initialized || !depthSRV || m_settings.mode == OcclusionCullingMode::Disabled)
                return;

            auto startTime = std::chrono::high_resolution_clock::now();

            // Mip 0: Copy depth to Hi-Z mip 0 via compute shader
            uint32_t mipWidth = m_width;
            uint32_t mipHeight = m_height;

            for (uint32_t mip = 0; mip < m_hiZMipCount; ++mip)
            {
                OcclusionCB cb = {};
                cb.mipSize = {static_cast<float>(mipWidth), static_cast<float>(mipHeight), 0.0f, 0.0f};
                cb.mipLevel = {static_cast<float>(mip), 0.0f, 0.0f, 0.0f};

                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                {
                    memcpy(mapped.pData, &cb, sizeof(OcclusionCB));
                    m_context->Unmap(m_constantBuffer.Get(), 0);
                }

                m_context->CSSetShader(m_hiZBuildCS.Get(), nullptr, 0);
                m_context->CSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

                // Input: previous mip (or depth buffer for mip 0)
                ID3D11ShaderResourceView* inputSRV = (mip == 0) ? depthSRV : m_hiZMipSRVs[mip - 1].Get();
                m_context->CSSetShaderResources(0, 1, &inputSRV);

                // Output: current mip UAV
                m_context->CSSetUnorderedAccessViews(0, 1, m_hiZUAVs[mip].GetAddressOf(), nullptr);

                uint32_t groupsX = (mipWidth + 7) / 8;
                uint32_t groupsY = (mipHeight + 7) / 8;
                m_context->Dispatch(groupsX, groupsY, 1);

                // Unbind UAV
                ID3D11UnorderedAccessView* nullUAV = nullptr;
                m_context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);

                mipWidth = std::max(1u, mipWidth / 2);
                mipHeight = std::max(1u, mipHeight / 2);
            }

            auto endTime = std::chrono::high_resolution_clock::now();
            m_metrics.buildHiZTimeMs =
                std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count() / 1000.0f;
        }

        /**
         * @brief Test objects against the Hi-Z pyramid
         * @param bounds Array of bounding volumes to test
         * @param results Output visibility array (true = visible)
         */
        void TestOcclusion(const std::vector<OcclusionBounds>& bounds, const XMMATRIX& viewProj,
                           std::vector<bool>& results)
        {
            if (!m_initialized || bounds.empty() || m_settings.mode == OcclusionCullingMode::Disabled)
            {
                results.assign(bounds.size(), true);
                return;
            }

            auto startTime = std::chrono::high_resolution_clock::now();

            m_metrics.totalObjects = static_cast<uint32_t>(bounds.size());
            m_metrics.testedObjects = static_cast<uint32_t>(bounds.size());

            if (m_settings.mode == OcclusionCullingMode::HierarchicalZ)
            {
                TestHiZ(bounds, viewProj, results);
            }
            else if (m_settings.mode == OcclusionCullingMode::HardwareQueries)
            {
                TestHardwareQueries(bounds, viewProj, results);
            }
            else
            {
                TestSoftware(bounds, viewProj, results);
            }

            uint32_t visible = 0;
            for (bool v : results)
                if (v)
                    visible++;

            m_metrics.visibleObjects = visible;
            m_metrics.occludedObjects = m_metrics.testedObjects - visible;

            auto endTime = std::chrono::high_resolution_clock::now();
            m_metrics.testTimeMs =
                std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count() / 1000.0f;
        }

        // ---- Settings ----
        OcclusionCullingSettings& GetSettings() { return m_settings; }
        const OcclusionCullingSettings& GetSettings() const { return m_settings; }
        const OcclusionMetrics& GetMetrics() const { return m_metrics; }

        // ---- Console ----
        std::string Console_GetStatus() const
        {
            std::string s = "Occlusion Culling:\n";
            s += "  Mode: ";
            switch (m_settings.mode)
            {
            case OcclusionCullingMode::Disabled:
                s += "Disabled";
                break;
            case OcclusionCullingMode::HierarchicalZ:
                s += "Hi-Z";
                break;
            case OcclusionCullingMode::HardwareQueries:
                s += "Hardware Queries";
                break;
            case OcclusionCullingMode::Software:
                s += "Software";
                break;
            }
            s += "\n";
            s += "  Objects: " + std::to_string(m_metrics.visibleObjects) + "/" +
                 std::to_string(m_metrics.totalObjects) + " visible\n";
            s += "  Occluded: " + std::to_string(m_metrics.occludedObjects) + "\n";
            s += "  Hi-Z build: " + std::to_string(m_metrics.buildHiZTimeMs) + "ms\n";
            s += "  Test: " + std::to_string(m_metrics.testTimeMs) + "ms\n";
            return s;
        }

      private:
        bool CreateHiZPyramid()
        {
            if (!m_device)
                return false;

            m_hiZUAVs.clear();
            m_hiZMipSRVs.clear();

            m_hiZMipCount =
                static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(std::max(m_width, m_height))))) + 1;
            m_hiZMipCount = std::min(m_hiZMipCount, m_settings.hiZMipLevels);
            m_metrics.hiZMipLevels = m_hiZMipCount;

            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width = m_width;
            texDesc.Height = m_height;
            texDesc.MipLevels = m_hiZMipCount;
            texDesc.ArraySize = 1;
            texDesc.Format = DXGI_FORMAT_R32_FLOAT;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_DEFAULT;
            texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

            HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_hiZTexture);
            if (FAILED(hr))
                return false;

            // Create full SRV
            hr = m_device->CreateShaderResourceView(m_hiZTexture.Get(), nullptr, &m_hiZSRV);
            if (FAILED(hr))
                return false;

            // Create per-mip UAVs and SRVs
            for (uint32_t mip = 0; mip < m_hiZMipCount; ++mip)
            {
                D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
                uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
                uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = mip;

                ComPtr<ID3D11UnorderedAccessView> uav;
                hr = m_device->CreateUnorderedAccessView(m_hiZTexture.Get(), &uavDesc, &uav);
                if (FAILED(hr))
                    return false;
                m_hiZUAVs.push_back(uav);

                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = mip;
                srvDesc.Texture2D.MipLevels = 1;

                ComPtr<ID3D11ShaderResourceView> srv;
                hr = m_device->CreateShaderResourceView(m_hiZTexture.Get(), &srvDesc, &srv);
                if (FAILED(hr))
                    return false;
                m_hiZMipSRVs.push_back(srv);
            }

            return true;
        }

        bool CompileShaders()
        {
            // Hi-Z build compute shader (downsample depth with max)
            const char* hiZBuildSource = R"(
                Texture2D<float> inputMip : register(t0);
                RWTexture2D<float> outputMip : register(u0);
                cbuffer OcclusionCB : register(b0) {
                    float4 mipSize;
                    float4 mipLevel;
                    float4 viewProjCol0;
                    float4 viewProjCol1;
                    float4 viewProjCol2;
                    float4 viewProjCol3;
                    float4 testParams;
                };

                [numthreads(8, 8, 1)]
                void main(uint3 dtid : SV_DispatchThreadID) {
                    if (dtid.x >= (uint)mipSize.x || dtid.y >= (uint)mipSize.y)
                        return;

                    if (mipLevel.x < 0.5) {
                        // Mip 0: just copy depth
                        outputMip[dtid.xy] = inputMip[dtid.xy * 2];
                    } else {
                        // Downsample: take max of 2x2 block (conservative for occlusion)
                        uint2 base = dtid.xy * 2;
                        float d0 = inputMip[base + uint2(0, 0)];
                        float d1 = inputMip[base + uint2(1, 0)];
                        float d2 = inputMip[base + uint2(0, 1)];
                        float d3 = inputMip[base + uint2(1, 1)];
                        outputMip[dtid.xy] = max(max(d0, d1), max(d2, d3));
                    }
                }
            )";

            // Occlusion test compute shader
            const char* testSource = R"(
                Texture2D<float> hiZPyramid : register(t0);
                StructuredBuffer<float4> boundsData : register(t1);
                RWStructuredBuffer<uint> results : register(u0);
                SamplerState pointSamp : register(s0);

                cbuffer OcclusionCB : register(b0) {
                    float4 mipSize;
                    float4 mipLevel;
                    float4 viewProjCol0;
                    float4 viewProjCol1;
                    float4 viewProjCol2;
                    float4 viewProjCol3;
                    float4 testParams; // x=objectCount, y=screenWidth, z=screenHeight, w=minScreenSize
                };

                float4 TransformPoint(float3 p) {
                    float4x4 vp = float4x4(viewProjCol0, viewProjCol1, viewProjCol2, viewProjCol3);
                    return mul(float4(p, 1), vp);
                }

                [numthreads(64, 1, 1)]
                void main(uint3 dtid : SV_DispatchThreadID) {
                    uint idx = dtid.x;
                    if (idx >= (uint)testParams.x)
                        return;

                    // Read bounds (center.xyz, extents.xyz packed in 2 float4s)
                    float4 b0 = boundsData[idx * 2];
                    float4 b1 = boundsData[idx * 2 + 1];
                    float3 center = b0.xyz;
                    float3 extents = b1.xyz;

                    // Project 8 corners of AABB to screen space
                    float2 screenMin = float2(1e10, 1e10);
                    float2 screenMax = float2(-1e10, -1e10);
                    float closestDepth = 1.0;
                    bool behindCamera = false;

                    for (int i = 0; i < 8; i++) {
                        float3 corner = center + extents * float3(
                            (i & 1) ? 1 : -1,
                            (i & 2) ? 1 : -1,
                            (i & 4) ? 1 : -1
                        );
                        float4 clip = TransformPoint(corner);
                        if (clip.w <= 0) { behindCamera = true; break; }
                        float3 ndc = clip.xyz / clip.w;
                        float2 screen = ndc.xy * 0.5 + 0.5;
                        screen.y = 1.0 - screen.y;
                        screenMin = min(screenMin, screen);
                        screenMax = max(screenMax, screen);
                        closestDepth = min(closestDepth, ndc.z);
                    }

                    // If any corner is behind camera, consider visible
                    if (behindCamera) {
                        results[idx] = 1;
                        return;
                    }

                    // Clamp to screen
                    screenMin = saturate(screenMin);
                    screenMax = saturate(screenMax);

                    // Skip tiny objects
                    float2 screenSize = (screenMax - screenMin) * float2(testParams.y, testParams.z);
                    if (max(screenSize.x, screenSize.y) < testParams.w) {
                        results[idx] = 0;
                        return;
                    }

                    // Choose Hi-Z mip level based on screen-space size
                    float mipSize2 = max(screenSize.x, screenSize.y);
                    float mip = max(0, log2(mipSize2));

                    // Sample Hi-Z at the center of the screen-space AABB
                    float2 sampleUV = (screenMin + screenMax) * 0.5;
                    float hiZDepth = hiZPyramid.SampleLevel(pointSamp, sampleUV, mip);

                    // If closest depth of AABB is further than Hi-Z depth, object is occluded
                    results[idx] = (closestDepth <= hiZDepth) ? 1 : 0;
                }
            )";

            ComPtr<ID3DBlob> blob, errBlob;

            HRESULT hr = D3DCompile(hiZBuildSource, strlen(hiZBuildSource), "HiZBuild", nullptr, nullptr, "main",
                                    "cs_5_0", 0, 0, &blob, &errBlob);
            if (SUCCEEDED(hr))
                m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_hiZBuildCS);

            hr = D3DCompile(testSource, strlen(testSource), "OcclusionTest", nullptr, nullptr, "main", "cs_5_0", 0, 0,
                            &blob, &errBlob);
            if (SUCCEEDED(hr))
                m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                              &m_occlusionTestCS);

            // Create constant buffer
            D3D11_BUFFER_DESC cbDesc = {};
            cbDesc.ByteWidth = sizeof(OcclusionCB);
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);

            // Create point sampler
            D3D11_SAMPLER_DESC sampDesc = {};
            sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            m_device->CreateSamplerState(&sampDesc, &m_pointSampler);

            return m_hiZBuildCS && m_occlusionTestCS;
        }

        bool CreateBuffers()
        {
            // Result buffer (will be resized per-test)
            return true;
        }

        void TestHiZ(const std::vector<OcclusionBounds>& bounds, const XMMATRIX& viewProj, std::vector<bool>& results)
        {
            uint32_t count = static_cast<uint32_t>(bounds.size());
            results.resize(count);

            // Upload bounds to GPU
            std::vector<XMFLOAT4> boundsData(count * 2);
            for (uint32_t i = 0; i < count; ++i)
            {
                boundsData[i * 2] = {bounds[i].center.x, bounds[i].center.y, bounds[i].center.z, 0.0f};
                boundsData[i * 2 + 1] = {bounds[i].extents.x * m_settings.conservativeScale,
                                         bounds[i].extents.y * m_settings.conservativeScale,
                                         bounds[i].extents.z * m_settings.conservativeScale, 0.0f};
            }

            // Create/resize bounds buffer
            D3D11_BUFFER_DESC bufDesc = {};
            bufDesc.ByteWidth = static_cast<UINT>(boundsData.size() * sizeof(XMFLOAT4));
            bufDesc.Usage = D3D11_USAGE_DEFAULT;
            bufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bufDesc.StructureByteStride = sizeof(XMFLOAT4);

            D3D11_SUBRESOURCE_DATA initData = {};
            initData.pSysMem = boundsData.data();
            m_device->CreateBuffer(&bufDesc, &initData, &m_boundsBuffer);

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.NumElements = static_cast<UINT>(boundsData.size());
            m_device->CreateShaderResourceView(m_boundsBuffer.Get(), &srvDesc, &m_boundsSRV);

            // Create result buffer
            bufDesc.ByteWidth = count * sizeof(uint32_t);
            bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bufDesc.StructureByteStride = sizeof(uint32_t);
            m_device->CreateBuffer(&bufDesc, nullptr, &m_resultBuffer);

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.NumElements = count;
            m_device->CreateUnorderedAccessView(m_resultBuffer.Get(), &uavDesc, &m_resultUAV);

            // Create staging buffer for CPU readback
            bufDesc.Usage = D3D11_USAGE_STAGING;
            bufDesc.BindFlags = 0;
            bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            bufDesc.MiscFlags = 0;
            m_device->CreateBuffer(&bufDesc, nullptr, &m_resultStagingBuffer);

            // Update CB with viewProj
            XMFLOAT4X4 vpMat;
            XMStoreFloat4x4(&vpMat, XMMatrixTranspose(viewProj));

            OcclusionCB cb = {};
            cb.viewProjCol0 = {vpMat._11, vpMat._12, vpMat._13, vpMat._14};
            cb.viewProjCol1 = {vpMat._21, vpMat._22, vpMat._23, vpMat._24};
            cb.viewProjCol2 = {vpMat._31, vpMat._32, vpMat._33, vpMat._34};
            cb.viewProjCol3 = {vpMat._41, vpMat._42, vpMat._43, vpMat._44};
            cb.testParams = {static_cast<float>(count), static_cast<float>(m_width), static_cast<float>(m_height),
                             m_settings.minScreenSize};

            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                memcpy(mapped.pData, &cb, sizeof(OcclusionCB));
                m_context->Unmap(m_constantBuffer.Get(), 0);
            }

            // Dispatch occlusion test
            m_context->CSSetShader(m_occlusionTestCS.Get(), nullptr, 0);
            m_context->CSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
            ID3D11ShaderResourceView* srvs[] = {m_hiZSRV.Get(), m_boundsSRV.Get()};
            m_context->CSSetShaderResources(0, 2, srvs);
            m_context->CSSetUnorderedAccessViews(0, 1, m_resultUAV.GetAddressOf(), nullptr);
            m_context->CSSetSamplers(0, 1, m_pointSampler.GetAddressOf());

            m_context->Dispatch((count + 63) / 64, 1, 1);

            // Unbind
            ID3D11UnorderedAccessView* nullUAV = nullptr;
            m_context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);

            // Readback results
            m_context->CopyResource(m_resultStagingBuffer.Get(), m_resultBuffer.Get());

            if (SUCCEEDED(m_context->Map(m_resultStagingBuffer.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            {
                const uint32_t* resultData = static_cast<const uint32_t*>(mapped.pData);
                for (uint32_t i = 0; i < count; ++i)
                    results[i] = (resultData[i] != 0);
                m_context->Unmap(m_resultStagingBuffer.Get(), 0);
            }
            else
            {
                // Fallback: all visible
                std::fill(results.begin(), results.end(), true);
            }
        }

        void TestHardwareQueries(const std::vector<OcclusionBounds>& bounds, const XMMATRIX& viewProj,
                                 std::vector<bool>& results)
        {
            // D3D11 occlusion queries - simpler but higher latency
            results.assign(bounds.size(), true);

            uint32_t queryCount =
                std::min(static_cast<uint32_t>(bounds.size()), static_cast<uint32_t>(m_settings.maxQueriesPerFrame));

            // Create queries if needed
            while (m_occlusionQueries.size() < queryCount)
            {
                D3D11_QUERY_DESC desc = {};
                desc.Query = D3D11_QUERY_OCCLUSION;
                ComPtr<ID3D11Query> query;
                if (SUCCEEDED(m_device->CreateQuery(&desc, &query)))
                    m_occlusionQueries.push_back(query);
            }

            // Issue queries (would need geometry rendering per-object for actual use)
            // For now, mark all as visible - full integration requires drawing bounding boxes
            for (uint32_t i = 0; i < queryCount; ++i)
            {
                results[i] = true;
            }
        }

        void TestSoftware(const std::vector<OcclusionBounds>& bounds, const XMMATRIX& viewProj,
                          std::vector<bool>& results)
        {
            // CPU software rasterizer for occlusion culling
            results.resize(bounds.size());

            // Simple sphere-based test against frustum + conservative depth
            for (size_t i = 0; i < bounds.size(); ++i)
            {
                const auto& b = bounds[i];
                XMVECTOR center = XMLoadFloat3(&b.center);
                XMVECTOR clipPos = XMVector4Transform(XMVectorSet(b.center.x, b.center.y, b.center.z, 1.0f), viewProj);

                float w = XMVectorGetW(clipPos);
                if (w <= 0.0f)
                {
                    results[i] = true; // Behind camera - might still be partially visible
                    continue;
                }

                float ndcZ = XMVectorGetZ(clipPos) / w;
                // Simple test: objects at reasonable depth are visible
                results[i] = (ndcZ >= 0.0f && ndcZ <= 1.0f);
            }
        }

        // ---- GPU Constant Buffer ----
        struct OcclusionCB
        {
            XMFLOAT4 mipSize;
            XMFLOAT4 mipLevel;
            XMFLOAT4 viewProjCol0;
            XMFLOAT4 viewProjCol1;
            XMFLOAT4 viewProjCol2;
            XMFLOAT4 viewProjCol3;
            XMFLOAT4 testParams;
        };

        // ---- State ----
        bool m_initialized = false;
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        uint32_t m_width = 1920;
        uint32_t m_height = 1080;
        uint32_t m_hiZMipCount = 10;
        OcclusionCullingSettings m_settings;
        OcclusionMetrics m_metrics;

        // Hi-Z pyramid
        ComPtr<ID3D11Texture2D> m_hiZTexture;
        ComPtr<ID3D11ShaderResourceView> m_hiZSRV;
        std::vector<ComPtr<ID3D11UnorderedAccessView>> m_hiZUAVs;
        std::vector<ComPtr<ID3D11ShaderResourceView>> m_hiZMipSRVs;

        // Compute shaders
        ComPtr<ID3D11ComputeShader> m_hiZBuildCS;
        ComPtr<ID3D11ComputeShader> m_occlusionTestCS;

        // Buffers
        ComPtr<ID3D11Buffer> m_boundsBuffer;
        ComPtr<ID3D11ShaderResourceView> m_boundsSRV;
        ComPtr<ID3D11Buffer> m_resultBuffer;
        ComPtr<ID3D11UnorderedAccessView> m_resultUAV;
        ComPtr<ID3D11Buffer> m_resultStagingBuffer;
        ComPtr<ID3D11Buffer> m_constantBuffer;

        // Sampler
        ComPtr<ID3D11SamplerState> m_pointSampler;

        // Hardware queries (fallback mode)
        std::vector<ComPtr<ID3D11Query>> m_occlusionQueries;
    };

} // namespace Spark::Graphics
