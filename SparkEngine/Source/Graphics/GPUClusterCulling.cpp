/**
 * @file GPUClusterCulling.cpp
 * @brief GPU compute shader-based clustered light assignment
 * @author Spark Engine Team
 * @date 2026
 */

#include "GPUClusterCulling.h"

#include <format>
#include <chrono>

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#endif

namespace Spark::Graphics
{

    std::string GPUClusterCulling::Console_GetStatus() const
    {
        if (!m_initialized)
        {
            return "GPUClusterCulling: Not initialized";
        }

        uint32_t totalClusters = m_config.gridX * m_config.gridY * m_config.gridZ;
        return std::format("GPUClusterCulling Status:\n"
                           "  Grid: {}x{}x{} = {} clusters\n"
                           "  Max lights/cluster: {}\n"
                           "  Active lights: {}\n"
                           "  Dispatches: {}\n"
                           "  Last dispatch: {:.2f} ms",
                           m_config.gridX, m_config.gridY, m_config.gridZ, totalClusters, m_config.maxLightsPerCluster,
                           m_stats.activeLights, m_stats.dispatchCount, m_stats.lastDispatchTimeMs);
    }

#ifdef SPARK_PLATFORM_WINDOWS

    bool GPUClusterCulling::Initialize(ID3D11Device* device, const ClusterGridConfig& config)
    {
        if (!device)
            return false;

        m_config = config;
        uint32_t totalClusters = config.gridX * config.gridY * config.gridZ;

        // Compile compute shader
        ComPtr<ID3DBlob> shaderBlob;
        ComPtr<ID3DBlob> errorBlob;
        UINT compileFlags = 0;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        HRESULT hr = D3DCompileFromFile(L"Shaders/HLSL/Compute/ClusterCull.hlsl", nullptr, nullptr, "CSMain", "cs_5_0",
                                        compileFlags, 0, &shaderBlob, &errorBlob);
        if (FAILED(hr))
            return false;

        hr = device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
                                         &m_cullShader);
        if (FAILED(hr))
            return false;

        // Light structured buffer (max 1024 lights)
        constexpr uint32_t kMaxLights = 1024;
        D3D11_BUFFER_DESC bufDesc = {};
        bufDesc.ByteWidth = kMaxLights * sizeof(GPULightData);
        bufDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bufDesc.StructureByteStride = sizeof(GPULightData);
        hr = device->CreateBuffer(&bufDesc, nullptr, &m_lightBuffer);
        if (FAILED(hr))
            return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = kMaxLights;
        hr = device->CreateShaderResourceView(m_lightBuffer.Get(), &srvDesc, &m_lightBufferSRV);
        if (FAILED(hr))
            return false;

        // Cluster AABB buffer
        bufDesc.ByteWidth = totalClusters * sizeof(GPUClusterAABB);
        bufDesc.StructureByteStride = sizeof(GPUClusterAABB);
        hr = device->CreateBuffer(&bufDesc, nullptr, &m_clusterBuffer);
        if (FAILED(hr))
            return false;

        srvDesc.Buffer.NumElements = totalClusters;
        hr = device->CreateShaderResourceView(m_clusterBuffer.Get(), &srvDesc, &m_clusterBufferSRV);
        if (FAILED(hr))
            return false;

        // Output: per-cluster light indices
        uint32_t totalIndices = totalClusters * config.maxLightsPerCluster;
        bufDesc.ByteWidth = totalIndices * sizeof(uint32_t);
        bufDesc.Usage = D3D11_USAGE_DEFAULT;
        bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        bufDesc.CPUAccessFlags = 0;
        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bufDesc.StructureByteStride = sizeof(uint32_t);
        hr = device->CreateBuffer(&bufDesc, nullptr, &m_lightIndicesBuffer);
        if (FAILED(hr))
            return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = totalIndices;
        hr = device->CreateUnorderedAccessView(m_lightIndicesBuffer.Get(), &uavDesc, &m_lightIndicesUAV);
        if (FAILED(hr))
            return false;

        srvDesc.Buffer.NumElements = totalIndices;
        hr = device->CreateShaderResourceView(m_lightIndicesBuffer.Get(), &srvDesc, &m_lightIndicesSRV);
        if (FAILED(hr))
            return false;

        // Output: per-cluster light counts
        bufDesc.ByteWidth = totalClusters * sizeof(uint32_t);
        hr = device->CreateBuffer(&bufDesc, nullptr, &m_lightCountsBuffer);
        if (FAILED(hr))
            return false;

        uavDesc.Buffer.NumElements = totalClusters;
        hr = device->CreateUnorderedAccessView(m_lightCountsBuffer.Get(), &uavDesc, &m_lightCountsUAV);
        if (FAILED(hr))
            return false;

        srvDesc.Buffer.NumElements = totalClusters;
        hr = device->CreateShaderResourceView(m_lightCountsBuffer.Get(), &srvDesc, &m_lightCountsSRV);
        if (FAILED(hr))
            return false;

        // Constant buffer
        struct ClusterConstants
        {
            uint32_t gridX, gridY, gridZ, maxLightsPerCluster;
            uint32_t activeLightCount;
            float nearPlane, farPlane, pad0;
            float inverseProjection[16];
        };

        bufDesc.ByteWidth = sizeof(ClusterConstants);
        bufDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bufDesc.MiscFlags = 0;
        bufDesc.StructureByteStride = 0;
        hr = device->CreateBuffer(&bufDesc, nullptr, &m_constantBuffer);
        if (FAILED(hr))
            return false;

        m_stats.totalClusters = totalClusters;
        m_initialized = true;
        return true;
    }

    void GPUClusterCulling::Shutdown()
    {
        m_cullShader.Reset();
        m_lightBuffer.Reset();
        m_lightBufferSRV.Reset();
        m_clusterBuffer.Reset();
        m_clusterBufferSRV.Reset();
        m_lightIndicesBuffer.Reset();
        m_lightIndicesUAV.Reset();
        m_lightIndicesSRV.Reset();
        m_lightCountsBuffer.Reset();
        m_lightCountsUAV.Reset();
        m_lightCountsSRV.Reset();
        m_constantBuffer.Reset();
        m_initialized = false;
    }

    void GPUClusterCulling::UploadClusterGrid(ID3D11DeviceContext* context, const GPUClusterAABB* clusters,
                                              uint32_t count)
    {
        if (!m_initialized || !context || !clusters)
            return;

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = context->Map(m_clusterBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr) && mapped.pData)
        {
            memcpy(mapped.pData, clusters, count * sizeof(GPUClusterAABB));
            context->Unmap(m_clusterBuffer.Get(), 0);
        }
    }

    void GPUClusterCulling::DispatchCull(ID3D11DeviceContext* context, const GPULightData* lights, uint32_t lightCount,
                                         const float* inverseProjection, float nearPlane, float farPlane)
    {
        if (!m_initialized || !context)
            return;

        auto startTime = std::chrono::high_resolution_clock::now();

        // Upload lights
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = context->Map(m_lightBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr) && mapped.pData)
        {
            memcpy(mapped.pData, lights, lightCount * sizeof(GPULightData));
            context->Unmap(m_lightBuffer.Get(), 0);
        }

        // Update constants
        struct ClusterConstants
        {
            uint32_t gridX, gridY, gridZ, maxLightsPerCluster;
            uint32_t activeLightCount;
            float nearPlane, farPlane, pad0;
            float inverseProjection[16];
        };

        hr = context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr) && mapped.pData)
        {
            auto* cb = static_cast<ClusterConstants*>(mapped.pData);
            cb->gridX = m_config.gridX;
            cb->gridY = m_config.gridY;
            cb->gridZ = m_config.gridZ;
            cb->maxLightsPerCluster = m_config.maxLightsPerCluster;
            cb->activeLightCount = lightCount;
            cb->nearPlane = nearPlane;
            cb->farPlane = farPlane;
            cb->pad0 = 0.0f;
            memcpy(cb->inverseProjection, inverseProjection, 16 * sizeof(float));
            context->Unmap(m_constantBuffer.Get(), 0);
        }

        // Clear light counts to zero
        const UINT clearValues[4] = {0, 0, 0, 0};
        context->ClearUnorderedAccessViewUint(m_lightCountsUAV.Get(), clearValues);

        // Bind resources
        context->CSSetShader(m_cullShader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

        ID3D11ShaderResourceView* srvs[] = {m_lightBufferSRV.Get(), m_clusterBufferSRV.Get()};
        context->CSSetShaderResources(0, 2, srvs);

        ID3D11UnorderedAccessView* uavs[] = {m_lightIndicesUAV.Get(), m_lightCountsUAV.Get()};
        context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

        // Dispatch: one thread group per cluster
        context->Dispatch(m_config.gridX, m_config.gridY, m_config.gridZ);

        // Unbind
        ID3D11ShaderResourceView* nullSRVs[2] = {};
        context->CSSetShaderResources(0, 2, nullSRVs);
        ID3D11UnorderedAccessView* nullUAVs[2] = {};
        context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);

        // Update stats
        auto endTime = std::chrono::high_resolution_clock::now();
        m_stats.lastDispatchTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        m_stats.activeLights = lightCount;
        m_stats.dispatchCount++;
    }

#endif // SPARK_PLATFORM_WINDOWS

} // namespace Spark::Graphics
