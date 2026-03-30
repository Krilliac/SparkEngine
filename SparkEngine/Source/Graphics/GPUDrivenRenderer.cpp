/**
 * @file GPUDrivenRenderer.cpp
 * @brief GPU-driven indirect rendering pipeline implementation
 *
 * Compiles cull and HiZ-build compute shaders, manages D3D11 structured
 * buffers for instance AABBs and visibility results, dispatches GPU culling,
 * and issues indirect draw calls for visible geometry.
 *
 * Compute shaders:
 *   - Shaders/HLSL/Compute/GPUCull.hlsl   (frustum + HiZ culling)
 *   - Shaders/HLSL/Compute/HiZBuild.hlsl  (HiZ mip chain downsample)
 *
 * @see GPUDrivenRenderer.h, GPUOcclusionCulling.h, GPUSceneBuffer.h
 */
#include "GPUDrivenRenderer.h"
#include "Utils/LogMacros.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#endif
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Spark::Graphics
{
    // Cull constant buffer layout (must match GPUCull.hlsl cbuffer)
#ifdef SPARK_PLATFORM_WINDOWS

    struct alignas(16) CullConstants
    {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4 frustumPlanes[6];
        uint32_t instanceCount, hiZWidth, hiZHeight, enableFrustumCull;
        uint32_t enableHiZCull, pad0, pad1, pad2;
    };

    static_assert(sizeof(CullConstants) % 16 == 0, "CullConstants must be 16-byte aligned");

    // =========================================================================
    // Initialize — create buffers, compile shaders
    // =========================================================================

    bool GPUDrivenRenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t maxInstances)
    {
        if (m_initialized)
            return true;
        if (!device || !context || maxInstances == 0)
            return false;

        m_device = device;
        m_context = context;
        m_maxInstances = maxInstances;

        // Compile compute shaders
        if (!CompileComputeShader(L"Shaders/HLSL/Compute/GPUCull.hlsl", "CSMain", m_cullShader.GetAddressOf()))
            return false;
        if (!CompileComputeShader(L"Shaders/HLSL/Compute/HiZBuild.hlsl", "CSMain", m_hiZBuildShader.GetAddressOf()))
            return false;

        // AABB input buffer (dynamic, CPU-writable structured buffer)
        D3D11_BUFFER_DESC aabbDesc = {};
        aabbDesc.ByteWidth = maxInstances * sizeof(GPUInstanceAABB);
        aabbDesc.Usage = D3D11_USAGE_DYNAMIC;
        aabbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        aabbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        aabbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        aabbDesc.StructureByteStride = sizeof(GPUInstanceAABB);

        HRESULT hr = device->CreateBuffer(&aabbDesc, nullptr, m_aabbBuffer.GetAddressOf());
        if (FAILED(hr))
            return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC aabbSrvDesc = {};
        aabbSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        aabbSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        aabbSrvDesc.Buffer.NumElements = maxInstances;
        hr = device->CreateShaderResourceView(m_aabbBuffer.Get(), &aabbSrvDesc, m_aabbSRV.GetAddressOf());
        if (FAILED(hr))
            return false;

        // Visibility buffer (GPU-writable UAV, one uint per instance, with CPU readback)
        if (!CreateStructuredBuffer(sizeof(uint32_t), maxInstances, m_visibilityBuffer.GetAddressOf(),
                                    m_visibilityUAV.GetAddressOf(), nullptr, true))
            return false;

        // Indirect draw argument buffer
        D3D11_BUFFER_DESC argsDesc = {};
        argsDesc.ByteWidth = sizeof(IndirectDrawArgs);
        argsDesc.Usage = D3D11_USAGE_DEFAULT;
        argsDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        argsDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        hr = device->CreateBuffer(&argsDesc, nullptr, m_indirectArgsBuffer.GetAddressOf());
        if (FAILED(hr))
            return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC argsUavDesc = {};
        argsUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        argsUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        argsUavDesc.Buffer.NumElements = sizeof(IndirectDrawArgs) / sizeof(uint32_t);
        argsUavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        hr = device->CreateUnorderedAccessView(m_indirectArgsBuffer.Get(), &argsUavDesc,
                                               m_indirectArgsUAV.GetAddressOf());
        if (FAILED(hr))
            return false;

        // Cull constant buffer
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(CullConstants);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&cbDesc, nullptr, m_cullConstantBuffer.GetAddressOf());
        if (FAILED(hr))
            return false;

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                       "GPUDrivenRenderer initialized: maxInstances=%u, cull + HiZ shaders compiled", maxInstances);
        return true;
    }

    // =========================================================================
    // BeginFrame — build HiZ mip chain from previous frame's depth
    // =========================================================================

    void GPUDrivenRenderer::BeginFrame(ID3D11ShaderResourceView* depthSRV, uint32_t width, uint32_t height)
    {
        if (!m_initialized || !depthSRV)
            return;

        // Recreate HiZ resources if the depth buffer size changed
        if (width != m_hiZWidth || height != m_hiZHeight)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GPUDrivenRenderer: recreating HiZ resources %ux%u", width,
                           height);
            if (!CreateHiZResources(width, height))
                return;
        }

        // Build each mip: mip 0 copies depth, higher mips downsample (max of 2x2)
        for (int mip = 0; mip < m_hiZMipCount; ++mip)
        {
            ID3D11ShaderResourceView* inputSRV = (mip == 0) ? depthSRV : m_hiZSRV.Get();
            ID3D11UnorderedAccessView* outputUAV = m_hiZMipUAVs[mip].Get();
            m_context->CSSetShaderResources(0, 1, &inputSRV);
            m_context->CSSetUnorderedAccessViews(0, 1, &outputUAV, nullptr);
            m_context->CSSetShader(m_hiZBuildShader.Get(), nullptr, 0);

            uint32_t mw = std::max(1u, m_hiZWidth >> mip);
            uint32_t mh = std::max(1u, m_hiZHeight >> mip);
            m_context->Dispatch((mw + 7) / 8, (mh + 7) / 8, 1);

            // Unbind so the output can be read as input on the next mip
            ID3D11ShaderResourceView* nullSRV = nullptr;
            ID3D11UnorderedAccessView* nullUAV = nullptr;
            m_context->CSSetShaderResources(0, 1, &nullSRV);
            m_context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
        }
    }

    // =========================================================================
    // CullAndDraw — dispatch culling CS and issue indirect draws
    // =========================================================================

    void GPUDrivenRenderer::CullAndDraw(const GPUInstanceAABB* aabbs, uint32_t instanceCount,
                                        const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
                                        ID3D11Buffer* indexBuffer, ID3D11Buffer* vertexBuffer, uint32_t stride,
                                        uint32_t indexCount)
    {
        if (!m_initialized || !aabbs || instanceCount == 0)
            return;

        instanceCount = std::min(instanceCount, m_maxInstances);
        m_stats.totalInstances = instanceCount;

        if (!m_settings.freezeCulling)
        {
            // Upload instance AABBs
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            HRESULT hr = m_context->Map(m_aabbBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(hr) || !mapped.pData)
                return;
            std::memcpy(mapped.pData, aabbs, instanceCount * sizeof(GPUInstanceAABB));
            m_context->Unmap(m_aabbBuffer.Get(), 0);

            // Build cull constants with frustum planes extracted from view-projection
            DirectX::XMMATRIX viewProj = DirectX::XMMatrixMultiply(viewMatrix, projMatrix);
            CullConstants cb = {};
            DirectX::XMStoreFloat4x4(&cb.viewProj, DirectX::XMMatrixTranspose(viewProj));

            DirectX::XMFLOAT4X4 vp;
            DirectX::XMStoreFloat4x4(&vp, viewProj);

            // Extract Left, Right, Bottom, Top, Near, Far planes from the VP matrix
            auto StorePlane = [&](int idx, float x, float y, float z, float w)
            {
                float len = std::sqrt(x * x + y * y + z * z);
                if (len > 0.0001f)
                {
                    float inv = 1.0f / len;
                    cb.frustumPlanes[idx] = {x * inv, y * inv, z * inv, w * inv};
                }
            };
            StorePlane(0, vp._14 + vp._11, vp._24 + vp._21, vp._34 + vp._31, vp._44 + vp._41);
            StorePlane(1, vp._14 - vp._11, vp._24 - vp._21, vp._34 - vp._31, vp._44 - vp._41);
            StorePlane(2, vp._14 + vp._12, vp._24 + vp._22, vp._34 + vp._32, vp._44 + vp._42);
            StorePlane(3, vp._14 - vp._12, vp._24 - vp._22, vp._34 - vp._32, vp._44 - vp._42);
            StorePlane(4, vp._13, vp._23, vp._33, vp._43);
            StorePlane(5, vp._14 - vp._13, vp._24 - vp._23, vp._34 - vp._33, vp._44 - vp._43);

            cb.instanceCount = instanceCount;
            cb.hiZWidth = m_hiZWidth;
            cb.hiZHeight = m_hiZHeight;
            cb.enableFrustumCull = m_settings.enableFrustumCull ? 1u : 0u;
            cb.enableHiZCull = m_settings.enableHiZCull ? 1u : 0u;

            hr = m_context->Map(m_cullConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(hr) || !mapped.pData)
                return;
            std::memcpy(mapped.pData, &cb, sizeof(CullConstants));
            m_context->Unmap(m_cullConstantBuffer.Get(), 0);

            // Clear indirect args (reset instance count to 0)
            IndirectDrawArgs clearArgs = {};
            clearArgs.indexCountPerInstance = indexCount;
            m_context->UpdateSubresource(m_indirectArgsBuffer.Get(), 0, nullptr, &clearArgs, 0, 0);

            // Dispatch cull compute shader (64 threads per group)
            m_context->CSSetShader(m_cullShader.Get(), nullptr, 0);
            ID3D11Buffer* cbs[] = {m_cullConstantBuffer.Get()};
            m_context->CSSetConstantBuffers(0, 1, cbs);
            ID3D11ShaderResourceView* srvs[] = {m_aabbSRV.Get(), m_hiZSRV.Get()};
            m_context->CSSetShaderResources(0, 2, srvs);
            ID3D11UnorderedAccessView* uavs[] = {m_visibilityUAV.Get(), m_indirectArgsUAV.Get()};
            m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

            m_context->Dispatch((instanceCount + 63) / 64, 1, 1);

            // Unbind compute resources
            ID3D11ShaderResourceView* nullSRVs[2] = {};
            ID3D11UnorderedAccessView* nullUAVs[2] = {};
            m_context->CSSetShaderResources(0, 2, nullSRVs);
            m_context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
        }

        // Read back visible count for statistics
        if (m_visibilityStaging)
        {
            m_context->CopyResource(m_visibilityStaging.Get(), m_visibilityBuffer.Get());
            D3D11_MAPPED_SUBRESOURCE rb = {};
            if (SUCCEEDED(m_context->Map(m_visibilityStaging.Get(), 0, D3D11_MAP_READ, 0, &rb)) && rb.pData)
            {
                const auto* vis = static_cast<const uint32_t*>(rb.pData);
                uint32_t count = 0;
                for (uint32_t i = 0; i < m_stats.totalInstances; ++i)
                    if (vis[i] != 0)
                        ++count;
                m_stats.visibleInstances = count;
                m_stats.culledByFrustum = m_stats.totalInstances - count;
                m_stats.culledByHiZ = 0; // GPU merges both tests; total culled reported
                m_context->Unmap(m_visibilityStaging.Get(), 0);
            }
        }

        // Issue indirect draw call for visible geometry
        UINT vertexOffset = 0;
        m_context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &vertexOffset);
        m_context->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, 0);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->DrawIndexedInstancedIndirect(m_indirectArgsBuffer.Get(), 0);
    }

    // =========================================================================
    // Helper: Compile compute shader from file
    // =========================================================================

    bool GPUDrivenRenderer::CompileComputeShader(const std::wstring& path, const std::string& entryPoint,
                                                 ID3D11ComputeShader** shaderOut)
    {
        UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#ifdef _DEBUG
        compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entryPoint.c_str(),
                                        "cs_5_0", compileFlags, 0, shaderBlob.GetAddressOf(), errorBlob.GetAddressOf());
        if (FAILED(hr))
            return false;

        hr = m_device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
                                           shaderOut);
        return SUCCEEDED(hr);
    }

    // =========================================================================
    // Helper: Create structured buffer with optional UAV/SRV
    // =========================================================================

    bool GPUDrivenRenderer::CreateStructuredBuffer(uint32_t elementSize, uint32_t elementCount,
                                                   ID3D11Buffer** bufferOut, ID3D11UnorderedAccessView** uavOut,
                                                   ID3D11ShaderResourceView** srvOut, bool cpuReadback)
    {
        D3D11_BUFFER_DESC bufDesc = {};
        bufDesc.ByteWidth = elementSize * elementCount;
        bufDesc.Usage = D3D11_USAGE_DEFAULT;
        bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (srvOut)
            bufDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bufDesc.StructureByteStride = elementSize;

        HRESULT hr = m_device->CreateBuffer(&bufDesc, nullptr, bufferOut);
        if (FAILED(hr))
            return false;

        if (uavOut)
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.NumElements = elementCount;
            hr = m_device->CreateUnorderedAccessView(*bufferOut, &uavDesc, uavOut);
            if (FAILED(hr))
                return false;
        }

        if (srvOut)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.NumElements = elementCount;
            hr = m_device->CreateShaderResourceView(*bufferOut, &srvDesc, srvOut);
            if (FAILED(hr))
                return false;
        }

        if (cpuReadback)
        {
            D3D11_BUFFER_DESC stagingDesc = {};
            stagingDesc.ByteWidth = elementSize * elementCount;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = m_device->CreateBuffer(&stagingDesc, nullptr, m_visibilityStaging.GetAddressOf());
            if (FAILED(hr))
                return false;
        }

        return true;
    }

    // =========================================================================
    // Helper: Create HiZ mip chain UAV textures
    // =========================================================================

    bool GPUDrivenRenderer::CreateHiZResources(uint32_t width, uint32_t height)
    {
        m_hiZTexture.Reset();
        m_hiZSRV.Reset();
        for (int i = 0; i < kMaxHiZMips; ++i)
            m_hiZMipUAVs[i].Reset();

        m_hiZWidth = width;
        m_hiZHeight = height;
        m_hiZMipCount = 1 + static_cast<int>(std::floor(std::log2(std::max(width, height))));
        m_hiZMipCount = std::min(m_hiZMipCount, kMaxHiZMips);

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = static_cast<UINT>(m_hiZMipCount);
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R32_FLOAT;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(m_device->CreateTexture2D(&texDesc, nullptr, m_hiZTexture.GetAddressOf())))
            return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = static_cast<UINT>(m_hiZMipCount);
        if (FAILED(m_device->CreateShaderResourceView(m_hiZTexture.Get(), &srvDesc, m_hiZSRV.GetAddressOf())))
            return false;

        for (int mip = 0; mip < m_hiZMipCount; ++mip)
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Texture2D.MipSlice = static_cast<UINT>(mip);
            if (FAILED(m_device->CreateUnorderedAccessView(m_hiZTexture.Get(), &uavDesc,
                                                           m_hiZMipUAVs[mip].GetAddressOf())))
                return false;
        }
        return true;
    }

#endif // SPARK_PLATFORM_WINDOWS

    // =========================================================================
    // Shutdown
    // =========================================================================

    void GPUDrivenRenderer::Shutdown()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        m_cullShader.Reset();
        m_hiZBuildShader.Reset();
        m_aabbBuffer.Reset();
        m_aabbSRV.Reset();
        m_visibilityBuffer.Reset();
        m_visibilityUAV.Reset();
        m_visibilityStaging.Reset();
        m_indirectArgsBuffer.Reset();
        m_indirectArgsUAV.Reset();
        m_cullConstantBuffer.Reset();
        m_hiZTexture.Reset();
        m_hiZSRV.Reset();
        for (int i = 0; i < kMaxHiZMips; ++i)
            m_hiZMipUAVs[i].Reset();
        m_device = nullptr;
        m_context = nullptr;
        m_hiZMipCount = 0;
        m_hiZWidth = 0;
        m_hiZHeight = 0;
#endif
        m_stats = {};
        m_maxInstances = 0;
        m_initialized = false;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GPUDrivenRenderer shut down");
    }

    // =========================================================================
    // Console status reporting
    // =========================================================================

    std::string GPUDrivenRenderer::Console_GetStatus() const
    {
        std::string s = "GPU-Driven Renderer: ";
        s += m_initialized ? "active" : "inactive";
        s += " | Instances: " + std::to_string(m_stats.visibleInstances) + "/" + std::to_string(m_stats.totalInstances);
        s += " | Culled: frustum=" + std::to_string(m_stats.culledByFrustum);
        s += " hiz=" + std::to_string(m_stats.culledByHiZ);
        s += " | Settings: frustum=" + std::string(m_settings.enableFrustumCull ? "on" : "off");
        s += " hiz=" + std::string(m_settings.enableHiZCull ? "on" : "off");
        s += " freeze=" + std::string(m_settings.freezeCulling ? "on" : "off") + "\n";
        return s;
    }

} // namespace Spark::Graphics
