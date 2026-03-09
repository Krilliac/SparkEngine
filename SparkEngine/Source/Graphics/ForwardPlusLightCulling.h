/**
 * @file ForwardPlusLightCulling.h
 * @brief GPU tile-based light culling for Forward+ and Clustered rendering
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

#include <vector>
#include <algorithm>
#include <cstdint>
#include <string>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace Spark::Graphics
{

    static constexpr uint32_t TILE_SIZE = 16;
    static constexpr uint32_t MAX_LIGHTS_PER_TILE = 256;
    static constexpr uint32_t MAX_TOTAL_LIGHTS = 1024;
    static constexpr uint32_t CLUSTER_X = 16;
    static constexpr uint32_t CLUSTER_Y = 9;
    static constexpr uint32_t CLUSTER_Z = 24;

    enum class LightCullingMode
    {
        ForwardPlus, ///< 2D tile-based culling (screen-space tiles)
        Clustered    ///< 3D clustered culling (frustum voxels)
    };

    struct GPUPointLight
    {
        XMFLOAT3 position;
        float radius;
        XMFLOAT3 color;
        float intensity;
    };

    struct GPUSpotLight
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

    struct LightCullingSettings
    {
        LightCullingMode mode = LightCullingMode::ForwardPlus;
        bool debugVisualization = false;
        float clusterNearPlane = 0.1f;
        float clusterFarPlane = 1000.0f;
    };

    struct LightCullingMetrics
    {
        uint32_t totalLights = 0;
        uint32_t tilesX = 0;
        uint32_t tilesY = 0;
        uint32_t maxLightsInTile = 0;
        float cullingTimeMs = 0.0f;
    };

    /**
     * @class ForwardPlusLightCulling
     * @brief GPU-accelerated tile-based and clustered light culling
     *
     * For Forward+: divides screen into TILE_SIZE×TILE_SIZE tiles, runs a compute
     * shader that tests each light's bounding sphere against the tile frustum, and
     * outputs per-tile light lists. The forward shading pass reads the light list
     * for the pixel's tile to shade only relevant lights.
     *
     * For Clustered: extends the tile grid into depth slices (log distribution),
     * producing a 3D cluster grid of light lists.
     */
    class ForwardPlusLightCulling
    {
      public:
        ForwardPlusLightCulling() = default;
        ~ForwardPlusLightCulling() = default;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width, uint32_t height)
        {
            m_device = device;
            m_context = context;
            m_width = width;
            m_height = height;
            m_tilesX = (width + TILE_SIZE - 1) / TILE_SIZE;
            m_tilesY = (height + TILE_SIZE - 1) / TILE_SIZE;

            if (!CreateBuffers())
                return false;
            if (!CompileShaders())
                return false;

            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            m_lightBuffer.Reset();
            m_lightBufferSRV.Reset();
            m_lightIndexBuffer.Reset();
            m_lightIndexUAV.Reset();
            m_lightIndexSRV.Reset();
            m_lightGridBuffer.Reset();
            m_lightGridUAV.Reset();
            m_lightGridSRV.Reset();
            m_cullingCB.Reset();
            m_tileCullingCS.Reset();
            m_clusterCullingCS.Reset();
            m_initialized = false;
        }

        void Resize(uint32_t width, uint32_t height)
        {
            m_width = width;
            m_height = height;
            m_tilesX = (width + TILE_SIZE - 1) / TILE_SIZE;
            m_tilesY = (height + TILE_SIZE - 1) / TILE_SIZE;
            CreateBuffers();
        }

        /**
         * @brief Upload light data and run GPU culling
         */
        void CullLights(const std::vector<GPUPointLight>& pointLights, const XMMATRIX& view, const XMMATRIX& projection,
                        ID3D11ShaderResourceView* depthSRV)
        {
            if (!m_initialized)
                return;

            UploadLights(pointLights);
            UpdateCullingConstants(view, projection);

            // Clear light index counter (first uint in light index buffer)
            uint32_t clearVal = 0;
            m_context->ClearUnorderedAccessViewUint(m_lightIndexUAV.Get(), reinterpret_cast<const UINT*>(&clearVal));
            uint32_t gridClear[4] = {0, 0, 0, 0};
            m_context->ClearUnorderedAccessViewUint(m_lightGridUAV.Get(), gridClear);

            // Bind resources
            m_context->CSSetConstantBuffers(0, 1, m_cullingCB.GetAddressOf());
            ID3D11ShaderResourceView* srvs[] = {m_lightBufferSRV.Get(), depthSRV};
            m_context->CSSetShaderResources(0, 2, srvs);
            ID3D11UnorderedAccessView* uavs[] = {m_lightIndexUAV.Get(), m_lightGridUAV.Get()};
            m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

            if (m_settings.mode == LightCullingMode::ForwardPlus)
            {
                m_context->CSSetShader(m_tileCullingCS.Get(), nullptr, 0);
                m_context->Dispatch(m_tilesX, m_tilesY, 1);
            }
            else
            {
                m_context->CSSetShader(m_clusterCullingCS.Get(), nullptr, 0);
                m_context->Dispatch(CLUSTER_X, CLUSTER_Y, CLUSTER_Z);
            }

            // Unbind
            ID3D11UnorderedAccessView* nullUAVs[] = {nullptr, nullptr};
            m_context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
            ID3D11ShaderResourceView* nullSRVs[] = {nullptr, nullptr};
            m_context->CSSetShaderResources(0, 2, nullSRVs);
        }

        /** @brief Get light index list SRV for use in forward shading */
        ID3D11ShaderResourceView* GetLightIndexSRV() const { return m_lightIndexSRV.Get(); }

        /** @brief Get per-tile light grid SRV (offset + count per tile) */
        ID3D11ShaderResourceView* GetLightGridSRV() const { return m_lightGridSRV.Get(); }

        /** @brief Get light data SRV for shading */
        ID3D11ShaderResourceView* GetLightBufferSRV() const { return m_lightBufferSRV.Get(); }

        LightCullingSettings& GetSettings() { return m_settings; }
        const LightCullingMetrics& GetMetrics() const { return m_metrics; }

        std::string Console_GetStatus() const
        {
            std::string s = "Forward+ Light Culling:\n";
            s += "  Mode: " +
                 std::string(m_settings.mode == LightCullingMode::ForwardPlus ? "Forward+ (Tiled)" : "Clustered") +
                 "\n";
            s += "  Tiles: " + std::to_string(m_tilesX) + "x" + std::to_string(m_tilesY) + "\n";
            s += "  Lights: " + std::to_string(m_metrics.totalLights) + "\n";
            return s;
        }

      private:
        struct CullingCB
        {
            XMFLOAT4X4 viewMatrix;
            XMFLOAT4X4 projMatrix;
            XMFLOAT4X4 invProjMatrix;
            uint32_t screenWidth;
            uint32_t screenHeight;
            uint32_t tilesX;
            uint32_t tilesY;
            uint32_t numLights;
            float nearPlane;
            float farPlane;
            float padding;
        };

        bool CreateBuffers()
        {
            // Light structured buffer
            D3D11_BUFFER_DESC bufDesc = {};
            bufDesc.ByteWidth = MAX_TOTAL_LIGHTS * sizeof(GPUPointLight);
            bufDesc.Usage = D3D11_USAGE_DYNAMIC;
            bufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bufDesc.StructureByteStride = sizeof(GPUPointLight);
            HRESULT hr = m_device->CreateBuffer(&bufDesc, nullptr, &m_lightBuffer);
            if (FAILED(hr))
                return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.NumElements = MAX_TOTAL_LIGHTS;
            m_device->CreateShaderResourceView(m_lightBuffer.Get(), &srvDesc, &m_lightBufferSRV);

            // Light index list (global append buffer)
            uint32_t totalTiles = m_tilesX * m_tilesY;
            uint32_t indexBufferSize = (1 + totalTiles * MAX_LIGHTS_PER_TILE) * sizeof(uint32_t);

            bufDesc = {};
            bufDesc.ByteWidth = indexBufferSize;
            bufDesc.Usage = D3D11_USAGE_DEFAULT;
            bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            hr = m_device->CreateBuffer(&bufDesc, nullptr, &m_lightIndexBuffer);
            if (FAILED(hr))
                return false;

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.NumElements = indexBufferSize / 4;
            uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            m_device->CreateUnorderedAccessView(m_lightIndexBuffer.Get(), &uavDesc, &m_lightIndexUAV);

            srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            srvDesc.BufferEx.NumElements = indexBufferSize / 4;
            srvDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            m_device->CreateShaderResourceView(m_lightIndexBuffer.Get(), &srvDesc, &m_lightIndexSRV);

            // Per-tile light grid (offset, count per tile)
            bufDesc = {};
            bufDesc.ByteWidth = totalTiles * 2 * sizeof(uint32_t);
            bufDesc.Usage = D3D11_USAGE_DEFAULT;
            bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bufDesc.StructureByteStride = 2 * sizeof(uint32_t);
            hr = m_device->CreateBuffer(&bufDesc, nullptr, &m_lightGridBuffer);
            if (FAILED(hr))
                return false;

            uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.NumElements = totalTiles;
            m_device->CreateUnorderedAccessView(m_lightGridBuffer.Get(), &uavDesc, &m_lightGridUAV);

            srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.NumElements = totalTiles;
            m_device->CreateShaderResourceView(m_lightGridBuffer.Get(), &srvDesc, &m_lightGridSRV);

            // Constant buffer
            bufDesc = {};
            bufDesc.ByteWidth = sizeof(CullingCB);
            bufDesc.Usage = D3D11_USAGE_DYNAMIC;
            bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            m_device->CreateBuffer(&bufDesc, nullptr, &m_cullingCB);

            return true;
        }

        bool CompileShaders()
        {
            // Tile-based light culling compute shader
            const char* tileCullingCS = R"(
                #define TILE_SIZE 16
                #define MAX_LIGHTS_PER_TILE 256

                struct PointLight
                {
                    float3 position;
                    float radius;
                    float3 color;
                    float intensity;
                };

                cbuffer CullingCB : register(b0)
                {
                    float4x4 viewMatrix;
                    float4x4 projMatrix;
                    float4x4 invProjMatrix;
                    uint screenWidth;
                    uint screenHeight;
                    uint tilesX;
                    uint tilesY;
                    uint numLights;
                    float nearPlane;
                    float farPlane;
                    float padding;
                };

                StructuredBuffer<PointLight> Lights : register(t0);
                Texture2D<float> DepthTexture : register(t1);
                RWByteAddressBuffer LightIndexList : register(u0);
                RWStructuredBuffer<uint2> LightGrid : register(u1);

                groupshared uint tileMinZ;
                groupshared uint tileMaxZ;
                groupshared uint tileLightCount;
                groupshared uint tileLightIndices[MAX_LIGHTS_PER_TILE];

                float3 ScreenToView(float2 screenPos, float depth)
                {
                    float2 ndc;
                    ndc.x = screenPos.x / screenWidth * 2.0 - 1.0;
                    ndc.y = 1.0 - screenPos.y / screenHeight * 2.0;
                    float4 clipPos = float4(ndc, depth, 1.0);
                    float4 viewPos = mul(clipPos, invProjMatrix);
                    return viewPos.xyz / viewPos.w;
                }

                bool SphereFrustumIntersect(float3 center, float radius,
                    float4 frustumPlanes[4])
                {
                    for (int i = 0; i < 4; i++)
                    {
                        if (dot(frustumPlanes[i].xyz, center) + frustumPlanes[i].w < -radius)
                            return false;
                    }
                    return true;
                }

                [numthreads(TILE_SIZE, TILE_SIZE, 1)]
                void main(uint3 groupId : SV_GroupID,
                          uint3 threadId : SV_GroupThreadID,
                          uint groupIndex : SV_GroupIndex,
                          uint3 dispatchId : SV_DispatchThreadID)
                {
                    // Initialize shared memory
                    if (groupIndex == 0)
                    {
                        tileMinZ = 0x7F7FFFFF;
                        tileMaxZ = 0;
                        tileLightCount = 0;
                    }
                    GroupMemoryBarrierWithGroupSync();

                    // Calculate tile min/max depth
                    float depth = 1.0;
                    if (dispatchId.x < screenWidth && dispatchId.y < screenHeight)
                    {
                        depth = DepthTexture.Load(int3(dispatchId.xy, 0));
                        uint depthInt = asuint(depth);
                        InterlockedMin(tileMinZ, depthInt);
                        InterlockedMax(tileMaxZ, depthInt);
                    }
                    GroupMemoryBarrierWithGroupSync();

                    float minZ = asfloat(tileMinZ);
                    float maxZ = asfloat(tileMaxZ);

                    // Build tile frustum planes in view space
                    float2 tileMin = float2(groupId.xy) * TILE_SIZE;
                    float2 tileMax = tileMin + float2(TILE_SIZE, TILE_SIZE);

                    float3 corners[4];
                    corners[0] = ScreenToView(float2(tileMin.x, tileMin.y), 1.0);
                    corners[1] = ScreenToView(float2(tileMax.x, tileMin.y), 1.0);
                    corners[2] = ScreenToView(float2(tileMax.x, tileMax.y), 1.0);
                    corners[3] = ScreenToView(float2(tileMin.x, tileMax.y), 1.0);

                    float4 frustumPlanes[4];
                    // Left
                    frustumPlanes[0] = float4(normalize(cross(corners[3], corners[0])), 0);
                    // Right
                    frustumPlanes[1] = float4(normalize(cross(corners[1], corners[2])), 0);
                    // Top
                    frustumPlanes[2] = float4(normalize(cross(corners[0], corners[1])), 0);
                    // Bottom
                    frustumPlanes[3] = float4(normalize(cross(corners[2], corners[3])), 0);

                    // Cull lights against tile
                    uint threadCount = TILE_SIZE * TILE_SIZE;
                    for (uint i = groupIndex; i < numLights; i += threadCount)
                    {
                        PointLight light = Lights[i];

                        // Transform to view space
                        float3 viewPos = mul(float4(light.position, 1.0), viewMatrix).xyz;

                        // Depth range test
                        if (viewPos.z + light.radius < minZ || viewPos.z - light.radius > maxZ)
                            continue;

                        // Frustum test
                        if (SphereFrustumIntersect(viewPos, light.radius, frustumPlanes))
                        {
                            uint idx;
                            InterlockedAdd(tileLightCount, 1, idx);
                            if (idx < MAX_LIGHTS_PER_TILE)
                            {
                                tileLightIndices[idx] = i;
                            }
                        }
                    }
                    GroupMemoryBarrierWithGroupSync();

                    // Write light list to global buffer
                    uint tileIndex = groupId.y * tilesX + groupId.x;

                    if (groupIndex == 0)
                    {
                        uint count = min(tileLightCount, MAX_LIGHTS_PER_TILE);

                        // Atomically allocate space in global light index list
                        uint globalOffset;
                        LightIndexList.InterlockedAdd(0, count, globalOffset);
                        // First uint is counter, so offset by 4 bytes
                        globalOffset += 1;

                        LightGrid[tileIndex] = uint2(globalOffset, count);

                        // Copy tile light indices to global list
                        for (uint j = 0; j < count; j++)
                        {
                            LightIndexList.Store((globalOffset + j) * 4, tileLightIndices[j]);
                        }
                    }
                }
            )";

            ComPtr<ID3DBlob> csBlob, errorBlob;
            HRESULT hr = D3DCompile(tileCullingCS, strlen(tileCullingCS), "TileCullingCS", nullptr, nullptr, "main",
                                    "cs_5_0", 0, 0, &csBlob, &errorBlob);
            if (FAILED(hr))
                return false;

            hr = m_device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr,
                                               &m_tileCullingCS);
            if (FAILED(hr))
                return false;

            // Clustered culling (3D extension)
            const char* clusterCullingCS = R"(
                #define MAX_LIGHTS_PER_CLUSTER 64

                struct PointLight
                {
                    float3 position;
                    float radius;
                    float3 color;
                    float intensity;
                };

                cbuffer CullingCB : register(b0)
                {
                    float4x4 viewMatrix;
                    float4x4 projMatrix;
                    float4x4 invProjMatrix;
                    uint screenWidth;
                    uint screenHeight;
                    uint tilesX;
                    uint tilesY;
                    uint numLights;
                    float nearPlane;
                    float farPlane;
                    float padding;
                };

                StructuredBuffer<PointLight> Lights : register(t0);
                RWByteAddressBuffer LightIndexList : register(u0);
                RWStructuredBuffer<uint2> LightGrid : register(u1);

                // Log-based depth slice distribution
                float ClusterDepth(uint slice, uint numSlices)
                {
                    return nearPlane * pow(farPlane / nearPlane, float(slice) / float(numSlices));
                }

                [numthreads(1, 1, 1)]
                void main(uint3 clusterIdx : SV_DispatchThreadID)
                {
                    // Cluster bounds in view space
                    float clusterNear = ClusterDepth(clusterIdx.z, 24);
                    float clusterFar = ClusterDepth(clusterIdx.z + 1, 24);

                    float2 tileSize = float2(screenWidth, screenHeight) / float2(16, 9);
                    float2 clusterMin = float2(clusterIdx.xy) * tileSize;
                    float2 clusterMax = clusterMin + tileSize;

                    // Convert to NDC then view space AABB
                    float2 ndcMin = clusterMin / float2(screenWidth, screenHeight) * 2.0 - 1.0;
                    float2 ndcMax = clusterMax / float2(screenWidth, screenHeight) * 2.0 - 1.0;
                    ndcMin.y = -ndcMin.y;
                    ndcMax.y = -ndcMax.y;

                    float3 viewMin = float3(ndcMin * clusterNear, clusterNear);
                    float3 viewMax = float3(ndcMax * clusterFar, clusterFar);

                    float3 aabbCenter = (viewMin + viewMax) * 0.5;
                    float3 aabbExtent = (viewMax - viewMin) * 0.5;

                    uint clusterIndex = clusterIdx.z * 16 * 9 + clusterIdx.y * 16 + clusterIdx.x;
                    uint lightCount = 0;
                    uint lightIndices[MAX_LIGHTS_PER_CLUSTER];

                    for (uint i = 0; i < numLights && lightCount < MAX_LIGHTS_PER_CLUSTER; i++)
                    {
                        float3 viewPos = mul(float4(Lights[i].position, 1.0), viewMatrix).xyz;
                        float r = Lights[i].radius;

                        // AABB-sphere intersection
                        float3 closest = clamp(viewPos, viewMin, viewMax);
                        float3 diff = viewPos - closest;
                        if (dot(diff, diff) <= r * r)
                        {
                            lightIndices[lightCount++] = i;
                        }
                    }

                    // Allocate and write
                    uint globalOffset;
                    LightIndexList.InterlockedAdd(0, lightCount, globalOffset);
                    globalOffset += 1;

                    LightGrid[clusterIndex] = uint2(globalOffset, lightCount);

                    for (uint j = 0; j < lightCount; j++)
                    {
                        LightIndexList.Store((globalOffset + j) * 4, lightIndices[j]);
                    }
                }
            )";

            hr = D3DCompile(clusterCullingCS, strlen(clusterCullingCS), "ClusterCullingCS", nullptr, nullptr, "main",
                            "cs_5_0", 0, 0, &csBlob, &errorBlob);
            if (FAILED(hr))
                return false;

            hr = m_device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr,
                                               &m_clusterCullingCS);
            return SUCCEEDED(hr);
        }

        void UploadLights(const std::vector<GPUPointLight>& lights)
        {
            D3D11_MAPPED_SUBRESOURCE mapped;
            m_context->Map(m_lightBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            uint32_t count = static_cast<uint32_t>(std::min(lights.size(), static_cast<size_t>(MAX_TOTAL_LIGHTS)));
            memcpy(mapped.pData, lights.data(), count * sizeof(GPUPointLight));
            m_context->Unmap(m_lightBuffer.Get(), 0);

            m_metrics.totalLights = count;
        }

        void UpdateCullingConstants(const XMMATRIX& view, const XMMATRIX& proj)
        {
            XMMATRIX invProj = XMMatrixInverse(nullptr, proj);

            CullingCB cb;
            XMStoreFloat4x4(&cb.viewMatrix, XMMatrixTranspose(view));
            XMStoreFloat4x4(&cb.projMatrix, XMMatrixTranspose(proj));
            XMStoreFloat4x4(&cb.invProjMatrix, XMMatrixTranspose(invProj));
            cb.screenWidth = m_width;
            cb.screenHeight = m_height;
            cb.tilesX = m_tilesX;
            cb.tilesY = m_tilesY;
            cb.numLights = m_metrics.totalLights;
            cb.nearPlane = m_settings.clusterNearPlane;
            cb.farPlane = m_settings.clusterFarPlane;
            cb.padding = 0;

            D3D11_MAPPED_SUBRESOURCE mapped;
            m_context->Map(m_cullingCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            memcpy(mapped.pData, &cb, sizeof(cb));
            m_context->Unmap(m_cullingCB.Get(), 0);
        }

        bool m_initialized = false;
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        uint32_t m_width = 1920, m_height = 1080;
        uint32_t m_tilesX = 0, m_tilesY = 0;

        LightCullingSettings m_settings;
        LightCullingMetrics m_metrics;

        ComPtr<ID3D11Buffer> m_lightBuffer;
        ComPtr<ID3D11ShaderResourceView> m_lightBufferSRV;
        ComPtr<ID3D11Buffer> m_lightIndexBuffer;
        ComPtr<ID3D11UnorderedAccessView> m_lightIndexUAV;
        ComPtr<ID3D11ShaderResourceView> m_lightIndexSRV;
        ComPtr<ID3D11Buffer> m_lightGridBuffer;
        ComPtr<ID3D11UnorderedAccessView> m_lightGridUAV;
        ComPtr<ID3D11ShaderResourceView> m_lightGridSRV;
        ComPtr<ID3D11Buffer> m_cullingCB;

        ComPtr<ID3D11ComputeShader> m_tileCullingCS;
        ComPtr<ID3D11ComputeShader> m_clusterCullingCS;
    };

} // namespace Spark::Graphics
