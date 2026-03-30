/**
 * @file GPUSkinning.cpp
 * @brief Implementation of GPU compute shader skinning
 * @author Spark Engine Team
 * @date 2026
 */

#include "GPUSkinning.h"
#include "Utils/LogMacros.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3dcompiler.h>
#include <cstring>
#endif

#include <format>

namespace Spark::Graphics
{

#ifdef SPARK_PLATFORM_WINDOWS

    // =========================================================================
    // Shader path and constants
    // =========================================================================

    static const wchar_t* kSkinningShaderPath = L"Shaders/HLSL/Compute/SkinningCS.hlsl";
    static const char* kSkinningEntryPoint = "CSMain";
    static const char* kSkinningShaderModel = "cs_5_0";

    /**
     * @brief Per-dispatch constant buffer layout. Must match SkinningCS.hlsl cbuffer.
     */
    struct alignas(16) SkinningConstants
    {
        uint32_t vertexCount = 0;
        uint32_t boneCount = 0;
        uint32_t padding[2] = {0, 0};
    };

    // =========================================================================
    // Singleton
    // =========================================================================

    GPUSkinning& GPUSkinning::GetInstance()
    {
        static GPUSkinning instance;
        return instance;
    }

    // =========================================================================
    // Initialize / Shutdown
    // =========================================================================

    bool GPUSkinning::Initialize(ID3D11Device* device)
    {
        if (m_initialized)
            return true;

        if (!device)
            return false;

        m_device = device;

        if (!CompileComputeShader(device))
            return false;

        m_initialized = true;
        m_totalDispatches = 0;
        m_dispatchesThisFrame = 0;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GPUSkinning initialized (compute shader compiled)");
        return true;
    }

    void GPUSkinning::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GPUSkinning shutting down (%zu meshes, %u total dispatches)",
                       m_meshEntries.size(), m_totalDispatches);
        m_meshEntries.clear();
        m_skinningShader.Reset();
        m_device = nullptr;
        m_initialized = false;
        m_totalDispatches = 0;
        m_dispatchesThisFrame = 0;
    }

    // =========================================================================
    // Shader compilation
    // =========================================================================

    bool GPUSkinning::CompileComputeShader(ID3D11Device* device)
    {
        ComPtr<ID3DBlob> shaderBlob;
        ComPtr<ID3DBlob> errorBlob;

        UINT compileFlags = 0;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        HRESULT hr = D3DCompileFromFile(kSkinningShaderPath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                        kSkinningEntryPoint, kSkinningShaderModel, compileFlags, 0,
                                        shaderBlob.GetAddressOf(), errorBlob.GetAddressOf());
        if (FAILED(hr))
            return false;

        hr = device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
                                         m_skinningShader.GetAddressOf());
        return SUCCEEDED(hr);
    }

    // =========================================================================
    // Mesh registration
    // =========================================================================

    bool GPUSkinning::RegisterMesh(uint32_t meshId, const SkinningSourceVertex* sourceVertices, uint32_t vertexCount,
                                   uint32_t maxBones)
    {
        if (!m_initialized || !sourceVertices || vertexCount == 0)
            return false;

        if (maxBones > kMaxBonesPerMesh)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "GPUSkinning: mesh %u has %u bones, exceeding max %u", meshId,
                           maxBones, kMaxBonesPerMesh);
            return false;
        }

        // Remove existing entry if re-registering
        m_meshEntries.erase(meshId);

        SkinningMeshEntry entry;
        entry.vertexCount = vertexCount;
        entry.boneCount = maxBones;

        // --- Source vertex structured buffer (immutable) ---
        {
            D3D11_BUFFER_DESC desc = {};
            desc.ByteWidth = vertexCount * sizeof(SkinningSourceVertex);
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = sizeof(SkinningSourceVertex);

            D3D11_SUBRESOURCE_DATA initData = {};
            initData.pSysMem = sourceVertices;

            HRESULT hr = m_device->CreateBuffer(&desc, &initData, entry.sourceVertexBuffer.GetAddressOf());
            if (FAILED(hr))
                return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.NumElements = vertexCount;

            hr = m_device->CreateShaderResourceView(entry.sourceVertexBuffer.Get(), &srvDesc,
                                                    entry.sourceVertexSRV.GetAddressOf());
            if (FAILED(hr))
                return false;
        }

        // --- Bone matrix structured buffer (dynamic, updated each frame) ---
        {
            D3D11_BUFFER_DESC desc = {};
            desc.ByteWidth = maxBones * sizeof(DirectX::XMFLOAT4X4);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = sizeof(DirectX::XMFLOAT4X4);

            HRESULT hr = m_device->CreateBuffer(&desc, nullptr, entry.boneMatrixBuffer.GetAddressOf());
            if (FAILED(hr))
                return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.NumElements = maxBones;

            hr = m_device->CreateShaderResourceView(entry.boneMatrixBuffer.Get(), &srvDesc,
                                                    entry.boneMatrixSRV.GetAddressOf());
            if (FAILED(hr))
                return false;
        }

        // --- Output vertex structured buffer (UAV for compute, SRV for vertex shader) ---
        {
            D3D11_BUFFER_DESC desc = {};
            desc.ByteWidth = vertexCount * sizeof(SkinningOutputVertex);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = sizeof(SkinningOutputVertex);

            HRESULT hr = m_device->CreateBuffer(&desc, nullptr, entry.outputVertexBuffer.GetAddressOf());
            if (FAILED(hr))
                return false;

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.NumElements = vertexCount;

            hr = m_device->CreateUnorderedAccessView(entry.outputVertexBuffer.Get(), &uavDesc,
                                                     entry.outputVertexUAV.GetAddressOf());
            if (FAILED(hr))
                return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.NumElements = vertexCount;

            hr = m_device->CreateShaderResourceView(entry.outputVertexBuffer.Get(), &srvDesc,
                                                    entry.outputVertexSRV.GetAddressOf());
            if (FAILED(hr))
                return false;
        }

        // --- Per-dispatch constant buffer ---
        if (!CreateConstantBuffer(m_device, entry))
            return false;

        m_meshEntries[meshId] = std::move(entry);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GPUSkinning: registered mesh %u (%u vertices, %u bones)", meshId,
                       vertexCount, maxBones);
        return true;
    }

    void GPUSkinning::UnregisterMesh(uint32_t meshId)
    {
        m_meshEntries.erase(meshId);
    }

    // =========================================================================
    // Constant buffer creation
    // =========================================================================

    bool GPUSkinning::CreateConstantBuffer(ID3D11Device* device, SkinningMeshEntry& entry)
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(SkinningConstants);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        SkinningConstants constants;
        constants.vertexCount = entry.vertexCount;
        constants.boneCount = entry.boneCount;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = &constants;

        HRESULT hr = device->CreateBuffer(&desc, &initData, entry.constantBuffer.GetAddressOf());
        return SUCCEEDED(hr);
    }

    // =========================================================================
    // Dispatch
    // =========================================================================

    void GPUSkinning::DispatchSkinning(ID3D11DeviceContext* context, uint32_t meshId,
                                       const DirectX::XMFLOAT4X4* boneMatrices, uint32_t boneCount)
    {
        if (!m_initialized || !context || !boneMatrices)
            return;

        auto it = m_meshEntries.find(meshId);
        if (it == m_meshEntries.end())
            return;

        SkinningMeshEntry& entry = it->second;

        uint32_t uploadCount = (boneCount < entry.boneCount) ? boneCount : entry.boneCount;

        // Upload bone matrices to the dynamic structured buffer
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = context->Map(entry.boneMatrixBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData)
            return;

        std::memcpy(mapped.pData, boneMatrices, uploadCount * sizeof(DirectX::XMFLOAT4X4));
        context->Unmap(entry.boneMatrixBuffer.Get(), 0);

        // Update constant buffer with current counts
        {
            D3D11_MAPPED_SUBRESOURCE cbMapped = {};
            hr = context->Map(entry.constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cbMapped);
            if (FAILED(hr) || !cbMapped.pData)
                return;

            SkinningConstants constants;
            constants.vertexCount = entry.vertexCount;
            constants.boneCount = uploadCount;
            std::memcpy(cbMapped.pData, &constants, sizeof(SkinningConstants));
            context->Unmap(entry.constantBuffer.Get(), 0);
        }

        // Bind compute shader and resources
        context->CSSetShader(m_skinningShader.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[2] = {entry.sourceVertexSRV.Get(), entry.boneMatrixSRV.Get()};
        context->CSSetShaderResources(0, 2, srvs);

        ID3D11UnorderedAccessView* uavs[1] = {entry.outputVertexUAV.Get()};
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        ID3D11Buffer* cbs[1] = {entry.constantBuffer.Get()};
        context->CSSetConstantBuffers(0, 1, cbs);

        // Dispatch: one thread per vertex, grouped into thread groups
        uint32_t threadGroups = (entry.vertexCount + kSkinningThreadGroupSize - 1) / kSkinningThreadGroupSize;
        context->Dispatch(threadGroups, 1, 1);

        // Unbind resources to avoid hazards
        ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
        context->CSSetShaderResources(0, 2, nullSRVs);

        ID3D11UnorderedAccessView* nullUAVs[1] = {nullptr};
        context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

        context->CSSetShader(nullptr, nullptr, 0);

        ++m_totalDispatches;
        ++m_dispatchesThisFrame;
    }

    // =========================================================================
    // Query
    // =========================================================================

    ID3D11ShaderResourceView* GPUSkinning::GetSkinnedBuffer(uint32_t meshId) const
    {
        auto it = m_meshEntries.find(meshId);
        if (it == m_meshEntries.end())
            return nullptr;

        return it->second.outputVertexSRV.Get();
    }

    bool GPUSkinning::IsMeshRegistered(uint32_t meshId) const
    {
        return m_meshEntries.contains(meshId);
    }

    // =========================================================================
    // Console status
    // =========================================================================

    std::string GPUSkinning::Console_GetStatus() const
    {
        if (!m_initialized)
            return "GPU Skinning: Not initialized\n";

        uint32_t totalVertices = 0;
        uint32_t totalBones = 0;
        for (const auto& [id, entry] : m_meshEntries)
        {
            totalVertices += entry.vertexCount;
            totalBones += entry.boneCount;
        }

        size_t bufferMemory = 0;
        for (const auto& [id, entry] : m_meshEntries)
        {
            bufferMemory += entry.vertexCount * sizeof(SkinningSourceVertex);
            bufferMemory += entry.boneCount * sizeof(DirectX::XMFLOAT4X4);
            bufferMemory += entry.vertexCount * sizeof(SkinningOutputVertex);
            bufferMemory += sizeof(SkinningConstants);
        }

        std::string status = "GPU Skinning:\n";
        status += std::format("  Registered meshes: {}\n", m_meshEntries.size());
        status += std::format("  Total vertices:    {}\n", totalVertices);
        status += std::format("  Total bones:       {}\n", totalBones);
        status += std::format("  GPU memory (est):  {} KB\n", bufferMemory / 1024);
        status += std::format("  Total dispatches:  {}\n", m_totalDispatches);
        status += std::format("  Frame dispatches:  {}\n", m_dispatchesThisFrame);
        return status;
    }

#endif // SPARK_PLATFORM_WINDOWS

} // namespace Spark::Graphics
