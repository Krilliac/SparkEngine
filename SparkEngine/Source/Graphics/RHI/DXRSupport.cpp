/**
 * @file DXRSupport.cpp
 * @brief DirectX Raytracing (DXR) implementation with full D3D12 pipeline
 *
 * Implements ray-traced reflections, shadows, AO, and GI using DXR 1.1.
 * Resources are managed via ComPtr — no manual Release() calls.
 */

#include "DXRSupport.h"
#include "../../Utils/Validate.h"
#include <sstream>
#include <cstring>
#include <algorithm>

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

using namespace DirectX;

namespace Spark::Graphics
{

#ifdef SPARK_PLATFORM_WINDOWS

    static const char* k_shaderPaths[] = {
        "Shaders/HLSL/RayTracing/DXRReflections.hlsl",
        "Shaders/HLSL/RayTracing/DXRShadows.hlsl",
        "Shaders/HLSL/RayTracing/DXRAO.hlsl",
        "Shaders/HLSL/RayTracing/DXRGI.hlsl",
    };

    // Pre-compiled DXIL blobs that the build system writes alongside the
    // HLSL files. We never call DXC at runtime — production builds have a
    // shader-compile step that produces these .cso files, and developer
    // workflows simply re-run that step. If the file is missing, the
    // matching PSO build fails and the trace dispatch becomes a no-op.
    static const wchar_t* k_dxilPaths[] = {
        L"Shaders/HLSL/RayTracing/DXRReflections.cso",
        L"Shaders/HLSL/RayTracing/DXRShadows.cso",
        L"Shaders/HLSL/RayTracing/DXRAO.cso",
        L"Shaders/HLSL/RayTracing/DXRGI.cso",
    };

    // Shader entry-point names exactly as they appear in the .hlsl files,
    // indexed in the same order as k_dxilPaths. They differ per library
    // (DXRShadows exports ShadowRayGen/ShadowMiss and has no closest hit), so
    // a single shared name set would leave three of the four PSOs unresolved.
    static const wchar_t* k_rayGenNames[] = {L"RayGen", L"ShadowRayGen", L"AORayGen", L"GIRayGen"};
    static const wchar_t* k_missNames[] = {L"Miss", L"ShadowMiss", L"AOMiss", L"GIMiss"};
    static const wchar_t* k_closestHitNames[] = {L"ClosestHit", nullptr, L"AOClosestHit", L"GIClosestHit"};
    static const wchar_t* k_hitGroupNames[] = {L"ReflectionHitGroup", L"ShadowHitGroup", L"AOHitGroup", L"GIHitGroup"};

    // Shader table record alignment required by DXR spec
    static constexpr UINT k_shaderRecordAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;

    static UINT AlignTo(UINT size, UINT alignment)
    {
        return (size + alignment - 1) & ~(alignment - 1);
    }

    struct DXRInternalState
    {
        ComPtr<ID3D12Device5> dxrDevice;
        ComPtr<ID3D12StateObject> psos[4]; // reflections, shadows, AO, GI
        ComPtr<ID3D12RootSignature> globalRootSignature;
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        ComPtr<ID3D12GraphicsCommandList4> commandList;
        ComPtr<ID3D12CommandQueue> commandQueue;
        ComPtr<ID3D12DescriptorHeap> cbvSrvUavHeap;

        // DXIL library blobs — one per PSO. Held alive for the lifetime
        // of the manager because D3D12_DXIL_LIBRARY_DESC retains a raw
        // pointer into them.
        std::vector<uint8_t> dxilBlobs[4];

        // Per-PSO shader binding tables. Each PSO needs its own rayGen,
        // miss, and hitGroup records — sharing tables across PSOs binds
        // the wrong shader identifiers and causes the dispatch to fault.
        UINT shaderRecordSize = 0;
        ComPtr<ID3D12Resource> rayGenTables[4];
        ComPtr<ID3D12Resource> missTables[4];
        ComPtr<ID3D12Resource> hitGroupTables[4];

        // Per-frame constant buffer (camera + light data) shared by all
        // four trace dispatches in a frame. Mapped persistently for
        // simple update — DXR is dispatched once per effect per frame so
        // a single CB is enough; we recreate when the upload size grows.
        ComPtr<ID3D12Resource> frameConstantBuffer;
        UINT frameConstantBufferSize = 0;

        // Output textures (one UAV per RT effect: reflections, shadows,
        // AO, GI). Created lazily inside DispatchRT once the output
        // dimensions are known so renderer-side resize is automatic.
        ComPtr<ID3D12Resource> outputTextures[4];
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;

        // Acceleration structure resources (owned by ComPtr)
        std::vector<ComPtr<ID3D12Resource>> blasResources;
        ComPtr<ID3D12Resource> tlasResource;

        // Scratch buffer pool — reused across AS builds
        ComPtr<ID3D12Resource> scratchBuffer;
        uint64_t scratchBufferSize = 0;

        // Timing queries
        ComPtr<ID3D12QueryHeap> timestampQueryHeap;
        ComPtr<ID3D12Resource> timestampReadbackBuffer;
        uint64_t gpuTimestampFrequency = 0;
    };

    static std::unique_ptr<DXRInternalState> s_dxrState;

    // Read a pre-compiled .cso DXIL blob from disk into a vector. Returns
    // empty vector on failure (logged once at error level — caller treats
    // as "PSO unavailable" rather than "fatal").
    static std::vector<uint8_t> LoadDXILBlob(const wchar_t* path)
    {
        std::vector<uint8_t> blob;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"rb") != 0 || !f)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "DXR: DXIL blob not found: %ls", path);
            return blob;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0)
        {
            blob.resize(static_cast<size_t>(size));
            fread(blob.data(), 1, blob.size(), f);
        }
        fclose(f);
        return blob;
    }

    // Create an UPLOAD-heap buffer pre-filled with `bytes` bytes from `src`.
    // Used by per-PSO shader-table construction.
    static ComPtr<ID3D12Resource> CreateUploadBufferWithData(ID3D12Device5* device, const void* src, UINT bytes)
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> buf;
        if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf))))
            return nullptr;

        void* mapped = nullptr;
        if (SUCCEEDED(buf->Map(0, nullptr, &mapped)) && mapped)
        {
            memcpy(mapped, src, bytes);
            buf->Unmap(0, nullptr);
        }
        return buf;
    }

    // Lazily allocate the per-effect output texture as a UAV at the
    // current output dimensions. Recreates the texture if the dimensions
    // changed since the last call. The caller (DispatchRT) is responsible
    // for binding the descriptor — this just owns the resource.
    static void EnsureOutputTexture(DXRInternalState& state, uint32_t effectIndex)
    {
        if (effectIndex >= 4)
            return;

        if (state.outputTextures[effectIndex])
        {
            const auto desc = state.outputTextures[effectIndex]->GetDesc();
            if (desc.Width == state.outputWidth && desc.Height == state.outputHeight)
                return; // up to date
            state.outputTextures[effectIndex].Reset();
        }

        if (state.outputWidth == 0 || state.outputHeight == 0)
            return;

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = state.outputWidth;
        desc.Height = state.outputHeight;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        state.dxrDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                 IID_PPV_ARGS(&state.outputTextures[effectIndex]));
    }

    // Ensure scratch buffer is at least `requiredSize` bytes
    static void EnsureScratchBuffer(DXRInternalState& state, uint64_t requiredSize)
    {
        if (state.scratchBufferSize >= requiredSize)
            return;

        state.scratchBuffer.Reset();
        state.scratchBufferSize = std::max(requiredSize, static_cast<uint64_t>(1 << 20)); // min 1MB

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = state.scratchBufferSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        state.dxrDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, IID_PPV_ARGS(&state.scratchBuffer));
    }

    static bool CreateDXRRootSignature(ID3D12Device5* device, DXRInternalState& state)
    {
        D3D12_DESCRIPTOR_RANGE1 srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 4;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE1 uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER1 rootParams[3] = {};
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[2].Descriptor.ShaderRegister = 0;
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = 3;
        desc.Desc_1_1.pParameters = rootParams;

        ComPtr<ID3DBlob> signature, error;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &signature, &error);
        if (FAILED(hr))
        {
            if (error)
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "DXR root sig error: %s",
                                static_cast<const char*>(error->GetBufferPointer()));
            return false;
        }

        return SUCCEEDED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                     IID_PPV_ARGS(&state.globalRootSignature)));
    }

    static ComPtr<ID3D12Resource> BuildBLASResource(DXRInternalState& state, const BLASDesc& desc, uint64_t& outSize)
    {
        D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
        geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geomDesc.Flags = desc.isOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        geomDesc.Triangles.VertexCount = desc.vertexCount;
        geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geomDesc.Triangles.VertexBuffer.StrideInBytes = desc.vertexStride;
        geomDesc.Triangles.IndexCount = desc.indexCount;
        geomDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = 1;
        inputs.pGeometryDescs = &geomDesc;
        inputs.Flags = desc.allowUpdate ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
                                        : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
        state.dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
        outSize = prebuild.ResultDataMaxSizeInBytes;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bufDesc = {};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = prebuild.ResultDataMaxSizeInBytes;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> blasBuffer;
        if (FAILED(state.dxrDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                                            nullptr, IID_PPV_ARGS(&blasBuffer))))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "DXR failed to create BLAS for '%s'", desc.meshName.c_str());
            return nullptr;
        }

        EnsureScratchBuffer(state, prebuild.ScratchDataSizeInBytes);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
        buildDesc.Inputs = inputs;
        buildDesc.DestAccelerationStructureData = blasBuffer->GetGPUVirtualAddress();
        buildDesc.ScratchAccelerationStructureData = state.scratchBuffer->GetGPUVirtualAddress();

        state.commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = blasBuffer.Get();
        state.commandList->ResourceBarrier(1, &barrier);

        return blasBuffer;
    }

    // Common dispatch helper shared by all 4 trace methods. Uses the
    // per-PSO shader tables built in BuildShaderTables and lazily creates
    // the matching output UAV texture for the requested effect.
    static void DispatchRT(DXRInternalState& state, uint32_t psoIndex, uint32_t tsBegin)
    {
        if (psoIndex >= 4)
            return;
        if (!state.psos[psoIndex] || !state.rayGenTables[psoIndex] || !state.missTables[psoIndex] ||
            !state.hitGroupTables[psoIndex])
            return;
        if (state.outputWidth == 0 || state.outputHeight == 0)
            return;

        EnsureOutputTexture(state, psoIndex);
        if (!state.outputTextures[psoIndex])
            return;

        auto* cmdList = state.commandList.Get();

        if (state.timestampQueryHeap)
            cmdList->EndQuery(state.timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBegin);

        // Bind descriptor heap, root signature, and PSO
        ID3D12DescriptorHeap* heaps[] = {state.cbvSrvUavHeap.Get()};
        if (state.cbvSrvUavHeap)
            cmdList->SetDescriptorHeaps(1, heaps);

        cmdList->SetComputeRootSignature(state.globalRootSignature.Get());
        cmdList->SetPipelineState1(state.psos[psoIndex].Get());

        // Bind the per-frame constant buffer at root parameter slot 2 if
        // available. The frame CB is updated by Trace*() methods just
        // before invoking DispatchRT.
        if (state.frameConstantBuffer)
            cmdList->SetComputeRootConstantBufferView(2, state.frameConstantBuffer->GetGPUVirtualAddress());

        const UINT recordSize = AlignTo(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, k_shaderRecordAlignment);

        D3D12_DISPATCH_RAYS_DESC dispatch = {};
        dispatch.Width = state.outputWidth;
        dispatch.Height = state.outputHeight;
        dispatch.Depth = 1;

        dispatch.RayGenerationShaderRecord.StartAddress = state.rayGenTables[psoIndex]->GetGPUVirtualAddress();
        dispatch.RayGenerationShaderRecord.SizeInBytes = recordSize;

        dispatch.MissShaderTable.StartAddress = state.missTables[psoIndex]->GetGPUVirtualAddress();
        dispatch.MissShaderTable.SizeInBytes = recordSize;
        dispatch.MissShaderTable.StrideInBytes = recordSize;

        dispatch.HitGroupTable.StartAddress = state.hitGroupTables[psoIndex]->GetGPUVirtualAddress();
        dispatch.HitGroupTable.SizeInBytes = recordSize;
        dispatch.HitGroupTable.StrideInBytes = recordSize;

        cmdList->DispatchRays(&dispatch);

        if (state.timestampQueryHeap)
            cmdList->EndQuery(state.timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBegin + 1);
    }

    // Update the per-frame constant buffer with view data for an effect.
    // Layout matches the `RTConstants` cbuffer in the four DXR HLSL files.
    static void UpdateFrameConstants(DXRInternalState& state, const XMMATRIX& viewProj, const XMFLOAT3& cameraPos,
                                     const XMFLOAT3& lightDir, float maxDistance, int maxBounces, int samplesPerPixel,
                                     float roughnessThreshold)
    {
        struct RTConstants
        {
            XMFLOAT4X4 invViewProj;
            XMFLOAT3 cameraPosition;
            float maxDistance;
            int maxBounces;
            int samplesPerPixel;
            float roughnessThreshold;
            float padding;
            XMFLOAT3 lightDirection;
            float padding2;
        } cb{};

        XMMATRIX inv = XMMatrixInverse(nullptr, viewProj);
        XMStoreFloat4x4(&cb.invViewProj, XMMatrixTranspose(inv));
        cb.cameraPosition = cameraPos;
        cb.maxDistance = maxDistance;
        cb.maxBounces = maxBounces;
        cb.samplesPerPixel = samplesPerPixel;
        cb.roughnessThreshold = roughnessThreshold;
        cb.lightDirection = lightDir;

        // Allocate / grow the per-frame constant buffer if needed.
        const UINT requiredSize = AlignTo(static_cast<UINT>(sizeof(RTConstants)), 256);
        if (state.frameConstantBufferSize < requiredSize)
        {
            state.frameConstantBuffer.Reset();
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = requiredSize;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(state.dxrDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                IID_PPV_ARGS(&state.frameConstantBuffer))))
                return;
            state.frameConstantBufferSize = requiredSize;
        }

        void* mapped = nullptr;
        if (SUCCEEDED(state.frameConstantBuffer->Map(0, nullptr, &mapped)) && mapped)
        {
            memcpy(mapped, &cb, sizeof(cb));
            state.frameConstantBuffer->Unmap(0, nullptr);
        }
    }

#endif // SPARK_PLATFORM_WINDOWS

    DXRManager& DXRManager::GetInstance()
    {
        static DXRManager instance;
        return instance;
    }

    bool DXRManager::Initialize(void* d3d12Device)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DXRManager::Initialize starting");
        if (!d3d12Device)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "DXRManager::Initialize called with null device");
            m_isAvailable = false;
            m_isInitialized = false;
            return false;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        auto* device = static_cast<ID3D12Device*>(d3d12Device);

        ComPtr<ID3D12Device5> dxrDevice;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxrDevice))))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "DXR: Device does not support ID3D12Device5");
            m_isAvailable = false;
            return false;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
        HRESULT hr = dxrDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
        if (FAILED(hr) || options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "DXR: GPU does not support DXR");
            m_isAvailable = false;
            return false;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DXR: Raytracing Tier %s",
                       options5.RaytracingTier == D3D12_RAYTRACING_TIER_1_0 ? "1.0" : "1.1");

        s_dxrState = std::make_unique<DXRInternalState>();
        s_dxrState->dxrDevice = dxrDevice;

        if (!CreateDXRRootSignature(dxrDevice.Get(), *s_dxrState))
        {
            s_dxrState.reset();
            m_isAvailable = false;
            return false;
        }

        hr = dxrDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&s_dxrState->commandAllocator));
        if (FAILED(hr))
        {
            s_dxrState.reset();
            return false;
        }

        ComPtr<ID3D12GraphicsCommandList> baseCmdList;
        hr = dxrDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_dxrState->commandAllocator.Get(),
                                          nullptr, IID_PPV_ARGS(&baseCmdList));
        if (SUCCEEDED(hr))
            baseCmdList.As(&s_dxrState->commandList);

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = dxrDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&s_dxrState->commandQueue));
        if (SUCCEEDED(hr))
            s_dxrState->commandQueue->GetTimestampFrequency(&s_dxrState->gpuTimestampFrequency);

        // CBV/SRV/UAV descriptor heap for RT bindings
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 16;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        dxrDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&s_dxrState->cbvSrvUavHeap));

        D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
        queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryHeapDesc.Count = 8;
        dxrDevice->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&s_dxrState->timestampQueryHeap));

        m_isAvailable = true;
        m_isInitialized = true;

        BuildRTPSOs();
        BuildShaderTables();

        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DXR: Initialized successfully");
        return true;
#else
        m_isAvailable = false;
        m_isInitialized = false;
        return false;
#endif
    }

    bool DXRManager::BuildRTPSOs()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!s_dxrState || !s_dxrState->dxrDevice || !s_dxrState->globalRootSignature)
            return false;

        // Per-PSO max recursion depths. Reflections need 2 (primary +
        // bounce), shadows/AO need 1 (single visibility ray), GI needs 3
        // (primary + 2 indirect bounces).
        const UINT maxRecursion[] = {2, 1, 1, 3};

        for (int i = 0; i < 4; i++)
        {
            // Load the pre-compiled DXIL blob for this effect. Stored in
            // the persistent dxilBlobs slot so the raw pointer in the
            // library descriptor stays valid until the PSO finishes
            // compilation.
            s_dxrState->dxilBlobs[i] = LoadDXILBlob(k_dxilPaths[i]);
            if (s_dxrState->dxilBlobs[i].empty())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                               "DXR: Skipping PSO %d — DXIL blob unavailable. Trace will be a no-op.", i);
                continue;
            }

            // 5 subobjects: DXIL library, hit group, shader config,
            // global root signature, pipeline config.
            D3D12_STATE_SUBOBJECT subobjects[5] = {};

            // Export this library's own entry points by name. Each PSO has its
            // own DXIL library, so names never collide across PSOs. The shadow
            // library is visibility-only and exports no closest hit.
            D3D12_EXPORT_DESC exportDescs[3] = {};
            UINT exportCount = 0;
            exportDescs[exportCount++] = {k_rayGenNames[i], nullptr, D3D12_EXPORT_FLAG_NONE};
            exportDescs[exportCount++] = {k_missNames[i], nullptr, D3D12_EXPORT_FLAG_NONE};
            const bool hasClosestHit = (k_closestHitNames[i] != nullptr);
            if (hasClosestHit)
                exportDescs[exportCount++] = {k_closestHitNames[i], nullptr, D3D12_EXPORT_FLAG_NONE};

            D3D12_DXIL_LIBRARY_DESC libDesc = {};
            libDesc.DXILLibrary.pShaderBytecode = s_dxrState->dxilBlobs[i].data();
            libDesc.DXILLibrary.BytecodeLength = s_dxrState->dxilBlobs[i].size();
            libDesc.NumExports = exportCount;
            libDesc.pExports = exportDescs;
            subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
            subobjects[0].pDesc = &libDesc;

            D3D12_HIT_GROUP_DESC hitGroupDesc = {};
            hitGroupDesc.HitGroupExport = k_hitGroupNames[i];
            hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
            hitGroupDesc.ClosestHitShaderImport = k_closestHitNames[i];
            subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
            subobjects[1].pDesc = &hitGroupDesc;

            D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
            shaderConfig.MaxPayloadSizeInBytes = 32;
            shaderConfig.MaxAttributeSizeInBytes = 8; // float2 barycentrics
            subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
            subobjects[2].pDesc = &shaderConfig;

            D3D12_GLOBAL_ROOT_SIGNATURE globalRS = {};
            globalRS.pGlobalRootSignature = s_dxrState->globalRootSignature.Get();
            subobjects[3].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
            subobjects[3].pDesc = &globalRS;

            D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
            pipelineConfig.MaxTraceRecursionDepth = maxRecursion[i];
            subobjects[4].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
            subobjects[4].pDesc = &pipelineConfig;

            D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
            stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
            stateObjectDesc.NumSubobjects = 5;
            stateObjectDesc.pSubobjects = subobjects;

            HRESULT hr = s_dxrState->dxrDevice->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&s_dxrState->psos[i]));
            if (FAILED(hr))
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "DXR: Failed to create RTPSO %d (hr=0x%08lX)", i,
                                static_cast<long>(hr));
        }
        return true;
#else
        return false;
#endif
    }

    bool DXRManager::BuildShaderTables()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!s_dxrState || !s_dxrState->dxrDevice)
            return false;

        const UINT recordSize = AlignTo(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, k_shaderRecordAlignment);
        s_dxrState->shaderRecordSize = recordSize;

        bool anyOk = false;
        for (int i = 0; i < 4; ++i)
        {
            if (!s_dxrState->psos[i])
                continue;

            ComPtr<ID3D12StateObjectProperties> props;
            if (FAILED(s_dxrState->psos[i].As(&props)))
                continue;

            auto buildRecord = [&](const wchar_t* name) -> std::vector<uint8_t>
            {
                std::vector<uint8_t> rec(recordSize, 0);
                void* id = props->GetShaderIdentifier(name);
                if (id)
                    memcpy(rec.data(), id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
                return rec;
            };

            auto rayGenRec = buildRecord(k_rayGenNames[i]);
            auto missRec = buildRecord(k_missNames[i]);
            auto hitGroupRec = buildRecord(k_hitGroupNames[i]);

            s_dxrState->rayGenTables[i] =
                CreateUploadBufferWithData(s_dxrState->dxrDevice.Get(), rayGenRec.data(), recordSize);
            s_dxrState->missTables[i] =
                CreateUploadBufferWithData(s_dxrState->dxrDevice.Get(), missRec.data(), recordSize);
            s_dxrState->hitGroupTables[i] =
                CreateUploadBufferWithData(s_dxrState->dxrDevice.Get(), hitGroupRec.data(), recordSize);

            if (s_dxrState->rayGenTables[i] && s_dxrState->missTables[i] && s_dxrState->hitGroupTables[i])
                anyOk = true;
        }
        return anyOk;
#else
        return false;
#endif
    }

    void DXRManager::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DXRManager::Shutdown");
#ifdef SPARK_PLATFORM_WINDOWS
        if (s_dxrState)
        {
            if (s_dxrState->commandQueue && s_dxrState->dxrDevice)
            {
                ComPtr<ID3D12Fence> fence;
                if (SUCCEEDED(s_dxrState->dxrDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
                {
                    s_dxrState->commandQueue->Signal(fence.Get(), 1);
                    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    if (event)
                    {
                        fence->SetEventOnCompletion(1, event);
                        WaitForSingleObject(event, 5000);
                        CloseHandle(event);
                    }
                }
            }
            s_dxrState.reset(); // All ComPtrs release automatically
        }
#endif

        m_blasList.clear();
        m_blasLookup.clear();
        m_tlasInternalIndex = UINT32_MAX;
        m_tlasSize = 0;
        m_tlasInstanceCount = 0;
        m_isInitialized = false;
    }

    uint32_t DXRManager::CreateBLAS(const BLASDesc& desc)
    {
        // Dedup: reuse existing BLAS for same mesh
        if (!desc.meshName.empty())
        {
            auto it = m_blasLookup.find(desc.meshName);
            if (it != m_blasLookup.end())
                return it->second;
        }

        BLASData data;
        data.desc = desc;

#ifdef SPARK_PLATFORM_WINDOWS
        if (m_isInitialized && s_dxrState && s_dxrState->commandList)
        {
            uint64_t blasSize = 0;
            auto blasBuffer = BuildBLASResource(*s_dxrState, desc, blasSize);
            if (blasBuffer)
            {
                data.internalIndex = static_cast<uint32_t>(s_dxrState->blasResources.size());
                s_dxrState->blasResources.push_back(std::move(blasBuffer));
            }
            data.size = blasSize;
        }
        else
#endif
        {
            data.size = static_cast<uint64_t>(desc.vertexCount) * desc.vertexStride +
                        static_cast<uint64_t>(desc.indexCount) * sizeof(uint32_t);
        }

        auto index = static_cast<uint32_t>(m_blasList.size());
        m_blasList.push_back(data);

        if (!desc.meshName.empty())
            m_blasLookup[desc.meshName] = index;

        return index;
    }

    void DXRManager::UpdateBLAS(uint32_t blasIndex)
    {
        if (blasIndex >= m_blasList.size())
            return;

#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_isInitialized || !s_dxrState || !s_dxrState->commandList)
            return;

        auto& blasData = m_blasList[blasIndex];
        if (blasData.internalIndex == UINT32_MAX || !blasData.desc.allowUpdate)
            return;

        auto& blasBuffer = s_dxrState->blasResources[blasData.internalIndex];

        D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
        geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geomDesc.Flags =
            blasData.desc.isOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        geomDesc.Triangles.VertexCount = blasData.desc.vertexCount;
        geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geomDesc.Triangles.VertexBuffer.StrideInBytes = blasData.desc.vertexStride;
        geomDesc.Triangles.IndexCount = blasData.desc.indexCount;
        geomDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = 1;
        inputs.pGeometryDescs = &geomDesc;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
        s_dxrState->dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
        EnsureScratchBuffer(*s_dxrState, prebuild.UpdateScratchDataSizeInBytes);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
        buildDesc.Inputs = inputs;
        buildDesc.DestAccelerationStructureData = blasBuffer->GetGPUVirtualAddress();
        buildDesc.SourceAccelerationStructureData = blasBuffer->GetGPUVirtualAddress();
        buildDesc.ScratchAccelerationStructureData = s_dxrState->scratchBuffer->GetGPUVirtualAddress();

        s_dxrState->commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = blasBuffer.Get();
        s_dxrState->commandList->ResourceBarrier(1, &barrier);
#endif
    }

    void DXRManager::DestroyBLAS(uint32_t blasIndex)
    {
        if (blasIndex >= m_blasList.size())
            return;

#ifdef SPARK_PLATFORM_WINDOWS
        auto& data = m_blasList[blasIndex];
        if (data.internalIndex != UINT32_MAX && s_dxrState && data.internalIndex < s_dxrState->blasResources.size())
        {
            s_dxrState->blasResources[data.internalIndex].Reset();
        }
#endif

        if (!m_blasList[blasIndex].desc.meshName.empty())
            m_blasLookup.erase(m_blasList[blasIndex].desc.meshName);

        m_blasList[blasIndex].internalIndex = UINT32_MAX;
        m_blasList[blasIndex].size = 0;
    }

    void DXRManager::BuildTLAS(const std::vector<BLASInstance>& instances)
    {
        m_tlasInstanceCount = static_cast<uint32_t>(instances.size());

#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_isInitialized || !s_dxrState || instances.empty())
            return;

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs(instances.size());
        for (size_t i = 0; i < instances.size(); i++)
        {
            auto& dst = instanceDescs[i];
            const auto& src = instances[i];

            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 4; col++)
                    dst.Transform[row][col] = src.transform.m[row][col];

            dst.InstanceID = src.instanceID;
            dst.InstanceMask = src.instanceMask;
            dst.InstanceContributionToHitGroupIndex = src.hitGroupIndex;
            dst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

            if (src.blasIndex < m_blasList.size())
            {
                uint32_t intIdx = m_blasList[src.blasIndex].internalIndex;
                if (intIdx != UINT32_MAX && intIdx < s_dxrState->blasResources.size() &&
                    s_dxrState->blasResources[intIdx])
                {
                    dst.AccelerationStructure = s_dxrState->blasResources[intIdx]->GetGPUVirtualAddress();
                }
            }
        }

        auto instanceBufferSize = instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufDesc = {};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = instanceBufferSize;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> instanceBuffer;
        s_dxrState->dxrDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&instanceBuffer));
        void* mapped = nullptr;
        if (FAILED(instanceBuffer->Map(0, nullptr, &mapped)) || !mapped)
            return;
        memcpy(mapped, instanceDescs.data(), instanceBufferSize);
        instanceBuffer->Unmap(0, nullptr);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = static_cast<UINT>(instances.size());
        inputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
        s_dxrState->dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

        // Release old TLAS via ComPtr reset
        if (m_tlasInternalIndex != UINT32_MAX)
            s_dxrState->tlasResource.Reset();

        D3D12_HEAP_PROPERTIES defaultHeap = {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        bufDesc.Width = prebuild.ResultDataMaxSizeInBytes;
        bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        s_dxrState->dxrDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
                                                       IID_PPV_ARGS(&s_dxrState->tlasResource));

        EnsureScratchBuffer(*s_dxrState, prebuild.ScratchDataSizeInBytes);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
        buildDesc.Inputs = inputs;
        buildDesc.DestAccelerationStructureData = s_dxrState->tlasResource->GetGPUVirtualAddress();
        buildDesc.ScratchAccelerationStructureData = s_dxrState->scratchBuffer->GetGPUVirtualAddress();

        s_dxrState->commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = s_dxrState->tlasResource.Get();
        s_dxrState->commandList->ResourceBarrier(1, &barrier);

        m_tlasInternalIndex = 0;
        m_tlasSize = prebuild.ResultDataMaxSizeInBytes;

        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DXR: Built TLAS with %zu instances (%llu KB)", instances.size(),
                       static_cast<unsigned long long>(m_tlasSize / 1024));
#endif
    }

    void DXRManager::TraceReflections(const XMMATRIX& viewProj, const XMFLOAT3& cameraPos)
    {
        if (!m_isInitialized || !HasFeature(m_settings.enabledFeatures, RTFeature::Reflections))
            return;
#ifdef SPARK_PLATFORM_WINDOWS
        if (!s_dxrState || !s_dxrState->psos[0] || !s_dxrState->tlasResource)
            return;
        const XMFLOAT3 dummyLight{0.0f, -1.0f, 0.0f};
        UpdateFrameConstants(*s_dxrState, viewProj, cameraPos, dummyLight, m_settings.reflections.maxDistance,
                             m_settings.reflections.maxBounces, m_settings.reflections.samplesPerPixel,
                             m_settings.reflections.roughnessThreshold);
        DispatchRT(*s_dxrState, 0, 0);
#endif
    }

    void DXRManager::TraceShadows(const XMFLOAT3& lightDirection)
    {
        if (!m_isInitialized || !HasFeature(m_settings.enabledFeatures, RTFeature::Shadows))
            return;
#ifdef SPARK_PLATFORM_WINDOWS
        if (!s_dxrState || !s_dxrState->psos[1] || !s_dxrState->tlasResource)
            return;
        // Shadow shader doesn't read InvViewProj — feed an identity-ish CB.
        XMMATRIX identity = XMMatrixIdentity();
        XMFLOAT3 origin{0.0f, 0.0f, 0.0f};
        UpdateFrameConstants(*s_dxrState, identity, origin, lightDirection, 1000.0f, 1,
                             m_settings.shadows.samplesPerPixel, 0.0f);
        DispatchRT(*s_dxrState, 1, 2);
#endif
    }

    void DXRManager::TraceAmbientOcclusion(const XMMATRIX& viewProj, const XMFLOAT3& cameraPos)
    {
        if (!m_isInitialized || !HasFeature(m_settings.enabledFeatures, RTFeature::AmbientOcclusion))
            return;
#ifdef SPARK_PLATFORM_WINDOWS
        if (!s_dxrState || !s_dxrState->psos[2] || !s_dxrState->tlasResource)
            return;
        const XMFLOAT3 dummyLight{0.0f, -1.0f, 0.0f};
        UpdateFrameConstants(*s_dxrState, viewProj, cameraPos, dummyLight, m_settings.ambientOcclusion.radius, 1,
                             m_settings.ambientOcclusion.samplesPerPixel, 0.0f);
        DispatchRT(*s_dxrState, 2, 4);
#endif
    }

    void DXRManager::TraceGlobalIllumination(const XMMATRIX& viewProj, const XMFLOAT3& cameraPos)
    {
        if (!m_isInitialized || !HasFeature(m_settings.enabledFeatures, RTFeature::GlobalIllumination))
            return;
#ifdef SPARK_PLATFORM_WINDOWS
        if (!s_dxrState || !s_dxrState->psos[3] || !s_dxrState->tlasResource)
            return;
        const XMFLOAT3 dummyLight{0.0f, -1.0f, 0.0f};
        UpdateFrameConstants(*s_dxrState, viewProj, cameraPos, dummyLight, m_settings.globalIllumination.maxDistance,
                             m_settings.globalIllumination.maxBounces, m_settings.globalIllumination.samplesPerPixel,
                             0.0f);
        DispatchRT(*s_dxrState, 3, 6);
#endif
    }

    void DXRManager::SetSettings(const DXRSettings& settings)
    {
        m_settings = settings;
    }

    DXRManager::DXRStats DXRManager::GetStats() const
    {
        m_stats.blasCount = static_cast<uint32_t>(m_blasList.size());
        m_stats.tlasInstanceCount = m_tlasInstanceCount;
        m_stats.accelerationStructureMemory = m_tlasSize;
        for (const auto& blas : m_blasList)
            m_stats.accelerationStructureMemory += blas.size;

#ifdef SPARK_PLATFORM_WINDOWS
        if (s_dxrState && s_dxrState->timestampReadbackBuffer && s_dxrState->gpuTimestampFrequency > 0)
        {
            uint64_t* timestamps = nullptr;
            D3D12_RANGE readRange = {0, sizeof(uint64_t) * 8};
            if (SUCCEEDED(
                    s_dxrState->timestampReadbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(&timestamps))))
            {
                double ticksToMs = 1000.0 / static_cast<double>(s_dxrState->gpuTimestampFrequency);
                m_stats.rtReflectionsTimeMs = static_cast<float>((timestamps[1] - timestamps[0]) * ticksToMs);
                m_stats.rtShadowsTimeMs = static_cast<float>((timestamps[3] - timestamps[2]) * ticksToMs);
                m_stats.rtAOTimeMs = static_cast<float>((timestamps[5] - timestamps[4]) * ticksToMs);
                m_stats.rtGITimeMs = static_cast<float>((timestamps[7] - timestamps[6]) * ticksToMs);
                D3D12_RANGE writeRange = {0, 0};
                s_dxrState->timestampReadbackBuffer->Unmap(0, &writeRange);
            }
        }
#endif
        return m_stats;
    }

    std::string DXRManager::Console_GetStatus() const
    {
        std::ostringstream ss;
        ss << "=== DXR Status ===\n"
           << "Available: " << (m_isAvailable ? "Yes" : "No") << "\n"
           << "Initialized: " << (m_isInitialized ? "Yes" : "No") << "\n";
        if (m_isInitialized)
        {
            auto stats = GetStats();
            ss << "BLAS Count: " << stats.blasCount << "\n"
               << "TLAS Instances: " << stats.tlasInstanceCount << "\n"
               << "AS Memory: " << (stats.accelerationStructureMemory / 1024) << " KB\n"
               << "Reflections: " << (HasFeature(m_settings.enabledFeatures, RTFeature::Reflections) ? "ON" : "OFF")
               << "\n"
               << "Shadows: " << (HasFeature(m_settings.enabledFeatures, RTFeature::Shadows) ? "ON" : "OFF") << "\n"
               << "AO: " << (HasFeature(m_settings.enabledFeatures, RTFeature::AmbientOcclusion) ? "ON" : "OFF") << "\n"
               << "GI: " << (HasFeature(m_settings.enabledFeatures, RTFeature::GlobalIllumination) ? "ON" : "OFF")
               << "\n";
        }
        return ss.str();
    }

    void DXRManager::Console_EnableFeature(const std::string& feature, bool enabled)
    {
        RTFeature flag = RTFeature::None;
        if (feature == "reflections")
            flag = RTFeature::Reflections;
        else if (feature == "shadows")
            flag = RTFeature::Shadows;
        else if (feature == "ao")
            flag = RTFeature::AmbientOcclusion;
        else if (feature == "gi")
            flag = RTFeature::GlobalIllumination;
        else
            return;

        if (enabled)
            m_settings.enabledFeatures = m_settings.enabledFeatures | flag;
        else
            m_settings.enabledFeatures = static_cast<RTFeature>(static_cast<uint32_t>(m_settings.enabledFeatures) &
                                                                ~static_cast<uint32_t>(flag));
    }

    void DXRManager::Console_SetQuality(const std::string& quality)
    {
        if (quality == "low")
        {
            m_settings.reflections = {true, 1, 0.5f, 1, 100.0f, true};
            m_settings.shadows = {true, 1, 0.05f, true};
            m_settings.ambientOcclusion = {true, 3.0f, 1, 1.5f, true};
            m_settings.renderScale = 0.5f;
        }
        else if (quality == "medium")
        {
            m_settings.reflections = {true, 1, 0.5f, 1, 100.0f, true};
            m_settings.shadows = {true, 2, 0.05f, true};
            m_settings.ambientOcclusion = {true, 3.0f, 2, 1.5f, true};
            m_settings.renderScale = 0.75f;
        }
        else if (quality == "high")
        {
            m_settings.reflections = {true, 2, 0.5f, 2, 100.0f, true};
            m_settings.shadows = {true, 4, 0.05f, true};
            m_settings.ambientOcclusion = {true, 3.0f, 4, 1.5f, true};
            m_settings.globalIllumination.samplesPerPixel = 1;
            m_settings.renderScale = 1.0f;
        }
        else if (quality == "ultra")
        {
            m_settings.reflections = {true, 3, 0.5f, 4, 100.0f, true};
            m_settings.shadows = {true, 8, 0.05f, true};
            m_settings.ambientOcclusion = {true, 3.0f, 8, 1.5f, true};
            m_settings.globalIllumination = {true, 3, 2, 50.0f, true, {2.0f, 2.0f, 2.0f}};
            m_settings.renderScale = 1.0f;
        }
    }

} // namespace Spark::Graphics
