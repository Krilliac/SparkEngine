/**
 * @file DXRSupport.cpp
 * @brief DXR implementation using D3D12 raytracing APIs (ID3D12Device5, ID3D12GraphicsCommandList4)
 *
 * All DXR code is conditionally compiled under SPARK_D3D12_SUPPORT.
 * When the macro is not defined, the original stub fallbacks are used.
 */

#include "DXRSupport.h"
#include <sstream>

#ifdef SPARK_D3D12_SUPPORT
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstring>
#include <algorithm>

using Microsoft::WRL::ComPtr;
#endif // SPARK_D3D12_SUPPORT

using namespace DirectX;

namespace Spark::Graphics
{

    // ============================================================================
    // Helper: align a size up to the given alignment
    // ============================================================================

    static constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    // ============================================================================
    // D3D12 Acceleration Structure Alignment
    // ============================================================================

    static constexpr uint64_t D3D12_RAYTRACING_AS_BYTE_ALIGNMENT = 256;

    // ============================================================================
    // Singleton
    // ============================================================================

    DXRManager& DXRManager::GetInstance()
    {
        static DXRManager instance;
        return instance;
    }

#ifdef SPARK_D3D12_SUPPORT

    // ============================================================================
    // Internal state stored as opaque pointers in the header's void* members.
    // We use a file-local struct to hold all D3D12/DXR COM objects.
    // ============================================================================

    struct DXRInternalState
    {
        ComPtr<ID3D12Device5> dxrDevice;
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        ComPtr<ID3D12GraphicsCommandList4> commandList;
        ComPtr<ID3D12CommandQueue> commandQueue;
        ComPtr<ID3D12Fence> fence;
        HANDLE fenceEvent = nullptr;
        uint64_t fenceValue = 0;

        // Global root signature for RT shaders
        ComPtr<ID3D12RootSignature> globalRootSignature;

        // Ray tracing state objects and shader tables
        ComPtr<ID3D12StateObject> reflectionStateObject;
        ComPtr<ID3D12StateObject> shadowStateObject;
        ComPtr<ID3D12StateObject> aoStateObject;
        ComPtr<ID3D12StateObject> giStateObject;

        // Shader table resources
        ComPtr<ID3D12Resource> reflectionShaderTable;
        ComPtr<ID3D12Resource> shadowShaderTable;
        ComPtr<ID3D12Resource> aoShaderTable;
        ComPtr<ID3D12Resource> giShaderTable;

        // Shader table record sizes (aligned to D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT)
        uint64_t shaderTableRecordSize = 0;

        // TLAS resource
        ComPtr<ID3D12Resource> tlasResult;
        ComPtr<ID3D12Resource> tlasScratch;
        ComPtr<ID3D12Resource> tlasInstanceDescs;

        // RT output UAV textures
        ComPtr<ID3D12Resource> reflectionOutput;
        ComPtr<ID3D12Resource> shadowOutput;
        ComPtr<ID3D12Resource> aoOutput;
        ComPtr<ID3D12Resource> giOutput;

        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
    };

    struct BLASResources
    {
        ComPtr<ID3D12Resource> result;
        ComPtr<ID3D12Resource> scratch;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags{};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
    };

    // File-local global DXR state (lifetime managed by Initialize/Shutdown)
    static DXRInternalState* g_dxrState = nullptr;
    static std::vector<BLASResources> g_blasResources;

    // ============================================================================
    // Helpers
    // ============================================================================

    static ComPtr<ID3D12Resource> CreateUAVBuffer(
        ID3D12Device* device, uint64_t size, D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC resourceDesc{};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = flags;

        ComPtr<ID3D12Resource> resource;
        HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState,
                                                     nullptr, IID_PPV_ARGS(&resource));
        if (FAILED(hr))
        {
            return nullptr;
        }
        return resource;
    }

    static ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, uint64_t size)
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC resourceDesc{};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ComPtr<ID3D12Resource> resource;
        HRESULT hr =
            device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource));
        if (FAILED(hr))
        {
            return nullptr;
        }
        return resource;
    }

    static void ExecuteAndWait(DXRInternalState* state)
    {
        state->commandList->Close();

        ID3D12CommandList* cmdLists[] = {state->commandList.Get()};
        state->commandQueue->ExecuteCommandLists(1, cmdLists);

        state->fenceValue++;
        state->commandQueue->Signal(state->fence.Get(), state->fenceValue);

        if (state->fence->GetCompletedValue() < state->fenceValue)
        {
            state->fence->SetEventOnCompletion(state->fenceValue, state->fenceEvent);
            WaitForSingleObject(state->fenceEvent, INFINITE);
        }

        state->commandAllocator->Reset();
        state->commandList->Reset(state->commandAllocator.Get(), nullptr);
    }

    static ComPtr<ID3D12RootSignature> CreateGlobalRootSignature(ID3D12Device* device)
    {
        // Global root signature with:
        // [0] SRV - acceleration structure (t0)
        // [1] UAV descriptor table - output texture (u0)
        // [2] CBV - per-frame constants (b0)
        // [3] SRV descriptor table - GBuffer textures (t1-t4)

        D3D12_DESCRIPTOR_RANGE1 uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.RegisterSpace = 0;
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE1 srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 4;
        srvRange.BaseShaderRegister = 1;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER1 rootParams[4]{};

        // [0] SRV - acceleration structure (inline, t0)
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[0].Descriptor.ShaderRegister = 0;
        rootParams[0].Descriptor.RegisterSpace = 0;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // [1] UAV descriptor table - output (u0)
        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // [2] CBV - per-frame constants (b0)
        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[2].Descriptor.ShaderRegister = 0;
        rootParams[2].Descriptor.RegisterSpace = 0;
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // [3] SRV descriptor table - GBuffer (t1-t4)
        rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[3].DescriptorTable.pDescriptorRanges = &srvRange;
        rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC staticSampler{};
        staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSampler.ShaderRegister = 0;
        staticSampler.RegisterSpace = 0;
        staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rsDesc.Desc_1_1.NumParameters = _countof(rootParams);
        rsDesc.Desc_1_1.pParameters = rootParams;
        rsDesc.Desc_1_1.NumStaticSamplers = 1;
        rsDesc.Desc_1_1.pStaticSamplers = &staticSampler;
        rsDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> serializedRS;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&rsDesc, &serializedRS, &errorBlob);
        if (FAILED(hr))
        {
            return nullptr;
        }

        ComPtr<ID3D12RootSignature> rootSig;
        hr = device->CreateRootSignature(0, serializedRS->GetBufferPointer(), serializedRS->GetBufferSize(),
                                         IID_PPV_ARGS(&rootSig));
        if (FAILED(hr))
        {
            return nullptr;
        }

        return rootSig;
    }

    static ComPtr<ID3D12StateObject> CreateRTStateObject(ID3D12Device5* device,
                                                         ID3D12RootSignature* globalRootSignature,
                                                         const wchar_t* rayGenShader, const wchar_t* missShader,
                                                         const wchar_t* closestHitShader, const wchar_t* hitGroupName)
    {
        // A DXR state object is built from a collection of subobjects:
        // 1. DXIL library (containing ray gen, miss, closest-hit shaders)
        // 2. Hit group definition
        // 3. Shader config (payload + attribute size)
        // 4. Pipeline config (max recursion depth)
        // 5. Global root signature

        constexpr uint32_t SUBOBJECT_COUNT = 5;
        D3D12_STATE_SUBOBJECT subobjects[SUBOBJECT_COUNT]{};

        // -- Subobject 0: DXIL Library --
        // In production, shader bytecode would be loaded from pre-compiled .cso files.
        // Here we set up the export descriptors; the DXIL blob must be supplied
        // by the asset pipeline at runtime.
        D3D12_DXIL_LIBRARY_DESC dxilLibDesc{};
        D3D12_EXPORT_DESC exports[3]{};
        exports[0].Name = rayGenShader;
        exports[0].ExportToRename = nullptr;
        exports[0].Flags = D3D12_EXPORT_FLAG_NONE;
        exports[1].Name = missShader;
        exports[1].ExportToRename = nullptr;
        exports[1].Flags = D3D12_EXPORT_FLAG_NONE;
        exports[2].Name = closestHitShader;
        exports[2].ExportToRename = nullptr;
        exports[2].Flags = D3D12_EXPORT_FLAG_NONE;

        // The DXIL bytecode would be set here from compiled shader library:
        // dxilLibDesc.DXILLibrary.pShaderBytecode = compiledShaderBlob->GetBufferPointer();
        // dxilLibDesc.DXILLibrary.BytecodeLength = compiledShaderBlob->GetBufferSize();
        dxilLibDesc.DXILLibrary.pShaderBytecode = nullptr;
        dxilLibDesc.DXILLibrary.BytecodeLength = 0;
        dxilLibDesc.NumExports = _countof(exports);
        dxilLibDesc.pExports = exports;

        subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
        subobjects[0].pDesc = &dxilLibDesc;

        // -- Subobject 1: Hit Group --
        D3D12_HIT_GROUP_DESC hitGroupDesc{};
        hitGroupDesc.HitGroupExport = hitGroupName;
        hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitGroupDesc.ClosestHitShaderImport = closestHitShader;
        hitGroupDesc.AnyHitShaderImport = nullptr;
        hitGroupDesc.IntersectionShaderImport = nullptr;

        subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
        subobjects[1].pDesc = &hitGroupDesc;

        // -- Subobject 2: Shader Config --
        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
        shaderConfig.MaxPayloadSizeInBytes = 4 * sizeof(float);   // float4 color
        shaderConfig.MaxAttributeSizeInBytes = 2 * sizeof(float); // float2 barycentrics

        subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
        subobjects[2].pDesc = &shaderConfig;

        // -- Subobject 3: Pipeline Config --
        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
        pipelineConfig.MaxTraceRecursionDepth = 2;

        subobjects[3].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
        subobjects[3].pDesc = &pipelineConfig;

        // -- Subobject 4: Global Root Signature --
        D3D12_GLOBAL_ROOT_SIGNATURE globalRSDesc{};
        globalRSDesc.pGlobalRootSignature = globalRootSignature;

        subobjects[4].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
        subobjects[4].pDesc = &globalRSDesc;

        // -- Create State Object --
        D3D12_STATE_OBJECT_DESC stateObjectDesc{};
        stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        stateObjectDesc.NumSubobjects = SUBOBJECT_COUNT;
        stateObjectDesc.pSubobjects = subobjects;

        ComPtr<ID3D12StateObject> stateObject;
        HRESULT hr = device->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&stateObject));
        if (FAILED(hr))
        {
            return nullptr;
        }

        return stateObject;
    }

    static ComPtr<ID3D12Resource> CreateShaderTable(ID3D12Device* device, ID3D12StateObject* stateObject,
                                                    const wchar_t* rayGenName, const wchar_t* missName,
                                                    const wchar_t* hitGroupName, uint64_t& outRecordSize)
    {
        ComPtr<ID3D12StateObjectProperties> stateObjectProps;
        stateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProps));
        if (!stateObjectProps)
        {
            return nullptr;
        }

        // Each shader table record = shader identifier (32 bytes) + root arguments
        const uint64_t shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        outRecordSize = AlignUp(shaderIdentifierSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

        // Shader table layout: [RayGen] [Miss] [HitGroup]
        const uint64_t tableSize = outRecordSize * 3;
        auto shaderTable = CreateUploadBuffer(device, tableSize);
        if (!shaderTable)
        {
            return nullptr;
        }

        // Map and write shader identifiers
        uint8_t* mappedData = nullptr;
        D3D12_RANGE readRange{0, 0};
        shaderTable->Map(0, &readRange, reinterpret_cast<void**>(&mappedData));
        if (!mappedData)
        {
            return nullptr;
        }

        // Record 0: Ray Generation shader
        void* rayGenId = stateObjectProps->GetShaderIdentifier(rayGenName);
        if (rayGenId)
        {
            memcpy(mappedData, rayGenId, shaderIdentifierSize);
        }

        // Record 1: Miss shader
        void* missId = stateObjectProps->GetShaderIdentifier(missName);
        if (missId)
        {
            memcpy(mappedData + outRecordSize, missId, shaderIdentifierSize);
        }

        // Record 2: Hit group
        void* hitGroupId = stateObjectProps->GetShaderIdentifier(hitGroupName);
        if (hitGroupId)
        {
            memcpy(mappedData + outRecordSize * 2, hitGroupId, shaderIdentifierSize);
        }

        shaderTable->Unmap(0, nullptr);
        return shaderTable;
    }

    static void DispatchRaysForPass(DXRInternalState* state, ID3D12StateObject* stateObject,
                                    ID3D12Resource* shaderTable, uint64_t recordSize, ID3D12Resource* outputTexture,
                                    uint32_t width, uint32_t height)
    {
        if (!stateObject || !shaderTable || !outputTexture)
        {
            return;
        }

        D3D12_GPU_VIRTUAL_ADDRESS tableBase = shaderTable->GetGPUVirtualAddress();

        D3D12_DISPATCH_RAYS_DESC dispatchDesc{};

        // Ray generation shader record
        dispatchDesc.RayGenerationShaderRecord.StartAddress = tableBase;
        dispatchDesc.RayGenerationShaderRecord.SizeInBytes = recordSize;

        // Miss shader record
        dispatchDesc.MissShaderTable.StartAddress = tableBase + recordSize;
        dispatchDesc.MissShaderTable.SizeInBytes = recordSize;
        dispatchDesc.MissShaderTable.StrideInBytes = recordSize;

        // Hit group shader record
        dispatchDesc.HitGroupTable.StartAddress = tableBase + recordSize * 2;
        dispatchDesc.HitGroupTable.SizeInBytes = recordSize;
        dispatchDesc.HitGroupTable.StrideInBytes = recordSize;

        // Dispatch dimensions match the output texture
        dispatchDesc.Width = width;
        dispatchDesc.Height = height;
        dispatchDesc.Depth = 1;

        state->commandList->SetPipelineState1(stateObject);
        state->commandList->DispatchRays(&dispatchDesc);
    }

    // ============================================================================
    // Initialize
    // ============================================================================

    bool DXRManager::Initialize(void* d3d12Device)
    {
        if (!d3d12Device)
        {
            m_isAvailable = false;
            m_isInitialized = false;
            return false;
        }

        auto* device = static_cast<ID3D12Device*>(d3d12Device);

        // Step 1: Query for DXR support via ID3D12Device5
        ComPtr<ID3D12Device5> dxrDevice;
        HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxrDevice));
        if (FAILED(hr))
        {
            m_isAvailable = false;
            m_isInitialized = false;
            return false;
        }

        // Step 2: Check for raytracing tier support
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        hr = dxrDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
        if (FAILED(hr) || options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        {
            m_isAvailable = false;
            m_isInitialized = false;
            return false;
        }

        m_isAvailable = true;

        // Step 3: Create internal DXR state
        g_dxrState = new DXRInternalState();
        g_dxrState->dxrDevice = dxrDevice;

        // Step 4: Create command allocator, command list, and command queue for RT work
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_dxrState->commandQueue));
        if (FAILED(hr))
        {
            delete g_dxrState;
            g_dxrState = nullptr;
            m_isAvailable = false;
            m_isInitialized = false;
            return false;
        }

        hr =
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_dxrState->commandAllocator));
        if (FAILED(hr))
        {
            delete g_dxrState;
            g_dxrState = nullptr;
            m_isAvailable = false;
            m_isInitialized = false;
            return false;
        }

        ComPtr<ID3D12GraphicsCommandList4> cmdList4;
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_dxrState->commandAllocator.Get(), nullptr,
                                       IID_PPV_ARGS(&cmdList4));
        if (FAILED(hr))
        {
            delete g_dxrState;
            g_dxrState = nullptr;
            m_isAvailable = false;
            m_isInitialized = false;
            return false;
        }
        g_dxrState->commandList = cmdList4;

        // Step 5: Create fence for GPU synchronization
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_dxrState->fence));
        if (FAILED(hr))
        {
            delete g_dxrState;
            g_dxrState = nullptr;
            m_isAvailable = false;
            m_isInitialized = false;
            return false;
        }
        g_dxrState->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        // Step 6: Create global root signature shared by all RT passes
        g_dxrState->globalRootSignature = CreateGlobalRootSignature(device);
        if (!g_dxrState->globalRootSignature)
        {
            delete g_dxrState;
            g_dxrState = nullptr;
            m_isAvailable = false;
            m_isInitialized = false;
            return false;
        }

        // Step 7: Create DXR state objects (pipelines) for each RT pass
        g_dxrState->reflectionStateObject =
            CreateRTStateObject(dxrDevice.Get(), g_dxrState->globalRootSignature.Get(), L"ReflectionRayGen",
                                L"ReflectionMiss", L"ReflectionClosestHit", L"ReflectionHitGroup");

        g_dxrState->shadowStateObject =
            CreateRTStateObject(dxrDevice.Get(), g_dxrState->globalRootSignature.Get(), L"ShadowRayGen", L"ShadowMiss",
                                L"ShadowClosestHit", L"ShadowHitGroup");

        g_dxrState->aoStateObject = CreateRTStateObject(dxrDevice.Get(), g_dxrState->globalRootSignature.Get(),
                                                        L"AORayGen", L"AOMiss", L"AOClosestHit", L"AOHitGroup");

        g_dxrState->giStateObject = CreateRTStateObject(dxrDevice.Get(), g_dxrState->globalRootSignature.Get(),
                                                        L"GIRayGen", L"GIMiss", L"GIClosestHit", L"GIHitGroup");

        // Step 8: Create shader tables for each pass
        if (g_dxrState->reflectionStateObject)
        {
            g_dxrState->reflectionShaderTable =
                CreateShaderTable(device, g_dxrState->reflectionStateObject.Get(), L"ReflectionRayGen",
                                  L"ReflectionMiss", L"ReflectionHitGroup", g_dxrState->shaderTableRecordSize);
        }

        if (g_dxrState->shadowStateObject)
        {
            uint64_t recordSize = 0;
            g_dxrState->shadowShaderTable =
                CreateShaderTable(device, g_dxrState->shadowStateObject.Get(), L"ShadowRayGen", L"ShadowMiss",
                                  L"ShadowHitGroup", recordSize);
        }

        if (g_dxrState->aoStateObject)
        {
            uint64_t recordSize = 0;
            g_dxrState->aoShaderTable = CreateShaderTable(device, g_dxrState->aoStateObject.Get(), L"AORayGen",
                                                          L"AOMiss", L"AOHitGroup", recordSize);
        }

        if (g_dxrState->giStateObject)
        {
            uint64_t recordSize = 0;
            g_dxrState->giShaderTable = CreateShaderTable(device, g_dxrState->giStateObject.Get(), L"GIRayGen",
                                                          L"GIMiss", L"GIHitGroup", recordSize);
        }

        m_isInitialized = true;
        return true;
    }

    void DXRManager::Shutdown()
    {
        if (g_dxrState)
        {
            // GPU idle before releasing resources
            if (g_dxrState->commandQueue && g_dxrState->fence && g_dxrState->fenceEvent)
            {
                g_dxrState->fenceValue++;
                g_dxrState->commandQueue->Signal(g_dxrState->fence.Get(), g_dxrState->fenceValue);
                if (g_dxrState->fence->GetCompletedValue() < g_dxrState->fenceValue)
                {
                    g_dxrState->fence->SetEventOnCompletion(g_dxrState->fenceValue, g_dxrState->fenceEvent);
                    WaitForSingleObject(g_dxrState->fenceEvent, INFINITE);
                }
                CloseHandle(g_dxrState->fenceEvent);
            }

            delete g_dxrState;
            g_dxrState = nullptr;
        }

        g_blasResources.clear();
        m_blasList.clear();
        m_tlasResource = nullptr;
        m_tlasSize = 0;
        m_tlasInstanceCount = 0;
        m_isInitialized = false;
    }

    // ============================================================================
    // CreateBLAS
    // ============================================================================

    uint32_t DXRManager::CreateBLAS(const BLASDesc& desc)
    {
        if (!m_isInitialized || !g_dxrState)
        {
            return UINT32_MAX;
        }

        auto* device = g_dxrState->dxrDevice.Get();

        BLASResources blasRes{};

        // Set up geometry descriptor for triangle mesh
        blasRes.geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        blasRes.geometryDesc.Flags =
            desc.isOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;

        // Vertex buffer
        blasRes.geometryDesc.Triangles.VertexBuffer.StartAddress = 0; // Set by caller via GPU VA
        blasRes.geometryDesc.Triangles.VertexBuffer.StrideInBytes = desc.vertexStride;
        blasRes.geometryDesc.Triangles.VertexCount = desc.vertexCount;
        blasRes.geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;

        // Index buffer
        if (desc.indexData && desc.indexCount > 0)
        {
            blasRes.geometryDesc.Triangles.IndexBuffer = 0; // Set by caller via GPU VA
            blasRes.geometryDesc.Triangles.IndexCount = desc.indexCount;
            blasRes.geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        }
        else
        {
            blasRes.geometryDesc.Triangles.IndexBuffer = 0;
            blasRes.geometryDesc.Triangles.IndexCount = 0;
            blasRes.geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
        }

        blasRes.geometryDesc.Triangles.Transform3x4 = 0;

        // Configure build inputs
        blasRes.buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        if (desc.allowUpdate)
        {
            blasRes.buildFlags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        }

        blasRes.inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blasRes.inputs.Flags = blasRes.buildFlags;
        blasRes.inputs.NumDescs = 1;
        blasRes.inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blasRes.inputs.pGeometryDescs = &blasRes.geometryDesc;

        // Query prebuild info to determine scratch and result buffer sizes
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&blasRes.inputs, &prebuildInfo);

        // Allocate scratch buffer
        uint64_t scratchSize = AlignUp(prebuildInfo.ScratchDataSizeInBytes, D3D12_RAYTRACING_AS_BYTE_ALIGNMENT);
        blasRes.scratch = CreateUAVBuffer(device, scratchSize, D3D12_RESOURCE_STATE_COMMON);
        if (!blasRes.scratch)
        {
            return UINT32_MAX;
        }

        // Allocate result buffer
        uint64_t resultSize = AlignUp(prebuildInfo.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_AS_BYTE_ALIGNMENT);
        blasRes.result = CreateUAVBuffer(device, resultSize, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        if (!blasRes.result)
        {
            return UINT32_MAX;
        }

        // Build the BLAS
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
        buildDesc.DestAccelerationStructureData = blasRes.result->GetGPUVirtualAddress();
        buildDesc.Inputs = blasRes.inputs;
        buildDesc.ScratchAccelerationStructureData = blasRes.scratch->GetGPUVirtualAddress();
        buildDesc.SourceAccelerationStructureData = 0;

        g_dxrState->commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

        // Insert UAV barrier to ensure BLAS is ready before use
        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = blasRes.result.Get();
        g_dxrState->commandList->ResourceBarrier(1, &uavBarrier);

        ExecuteAndWait(g_dxrState);

        // Store in engine tracking
        BLASData data;
        data.desc = desc;
        data.resource = blasRes.result.Get();
        data.size = resultSize;
        m_blasList.push_back(data);

        g_blasResources.push_back(std::move(blasRes));

        return static_cast<uint32_t>(m_blasList.size() - 1);
    }

    // ============================================================================
    // UpdateBLAS
    // ============================================================================

    void DXRManager::UpdateBLAS(uint32_t blasIndex)
    {
        if (blasIndex >= m_blasList.size() || blasIndex >= g_blasResources.size())
        {
            return;
        }

        if (!m_isInitialized || !g_dxrState)
        {
            return;
        }

        auto& blasRes = g_blasResources[blasIndex];
        const auto& blasData = m_blasList[blasIndex];

        // Only refit if the BLAS was created with ALLOW_UPDATE
        if (!blasData.desc.allowUpdate)
        {
            return;
        }

        // Rebuild (refit) the existing BLAS in place using PERFORM_UPDATE flag
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
        buildDesc.DestAccelerationStructureData = blasRes.result->GetGPUVirtualAddress();

        blasRes.inputs.Flags = blasRes.buildFlags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        buildDesc.Inputs = blasRes.inputs;

        buildDesc.ScratchAccelerationStructureData = blasRes.scratch->GetGPUVirtualAddress();
        buildDesc.SourceAccelerationStructureData = blasRes.result->GetGPUVirtualAddress();

        g_dxrState->commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

        // UAV barrier after refit
        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = blasRes.result.Get();
        g_dxrState->commandList->ResourceBarrier(1, &uavBarrier);

        ExecuteAndWait(g_dxrState);
    }

    // ============================================================================
    // DestroyBLAS
    // ============================================================================

    void DXRManager::DestroyBLAS(uint32_t blasIndex)
    {
        if (blasIndex >= m_blasList.size())
        {
            return;
        }

        // Release D3D12 resources
        if (blasIndex < g_blasResources.size())
        {
            g_blasResources[blasIndex].result.Reset();
            g_blasResources[blasIndex].scratch.Reset();
        }

        m_blasList[blasIndex].resource = nullptr;
        m_blasList[blasIndex].size = 0;
    }

    // ============================================================================
    // BuildTLAS
    // ============================================================================

    void DXRManager::BuildTLAS(const std::vector<BLASInstance>& instances)
    {
        if (!m_isInitialized || !g_dxrState || instances.empty())
        {
            return;
        }

        auto* device = g_dxrState->dxrDevice.Get();
        m_tlasInstanceCount = static_cast<uint32_t>(instances.size());

        // Step 1: Create instance descriptor buffer
        const uint64_t instanceDescSize =
            static_cast<uint64_t>(instances.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        g_dxrState->tlasInstanceDescs = CreateUploadBuffer(device, instanceDescSize);
        if (!g_dxrState->tlasInstanceDescs)
        {
            return;
        }

        // Step 2: Fill instance descriptors
        D3D12_RAYTRACING_INSTANCE_DESC* mappedDescs = nullptr;
        D3D12_RANGE readRange{0, 0};
        g_dxrState->tlasInstanceDescs->Map(0, &readRange, reinterpret_cast<void**>(&mappedDescs));
        if (!mappedDescs)
        {
            return;
        }

        for (uint32_t i = 0; i < instances.size(); ++i)
        {
            const auto& inst = instances[i];

            D3D12_RAYTRACING_INSTANCE_DESC& d3dInst = mappedDescs[i];
            memset(&d3dInst, 0, sizeof(D3D12_RAYTRACING_INSTANCE_DESC));

            // Convert 4x4 transform to 3x4 row-major layout expected by DXR
            const auto& m = inst.transform;
            d3dInst.Transform[0][0] = m._11;
            d3dInst.Transform[0][1] = m._12;
            d3dInst.Transform[0][2] = m._13;
            d3dInst.Transform[0][3] = m._14;
            d3dInst.Transform[1][0] = m._21;
            d3dInst.Transform[1][1] = m._22;
            d3dInst.Transform[1][2] = m._23;
            d3dInst.Transform[1][3] = m._24;
            d3dInst.Transform[2][0] = m._31;
            d3dInst.Transform[2][1] = m._32;
            d3dInst.Transform[2][2] = m._33;
            d3dInst.Transform[2][3] = m._34;

            d3dInst.InstanceID = inst.instanceID;
            d3dInst.InstanceMask = inst.instanceMask;
            d3dInst.InstanceContributionToHitGroupIndex = inst.hitGroupIndex;
            d3dInst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

            // Point to the BLAS GPU address
            if (inst.blasIndex < g_blasResources.size() && g_blasResources[inst.blasIndex].result)
            {
                d3dInst.AccelerationStructure = g_blasResources[inst.blasIndex].result->GetGPUVirtualAddress();
            }
            else
            {
                d3dInst.AccelerationStructure = 0;
            }
        }

        g_dxrState->tlasInstanceDescs->Unmap(0, nullptr);

        // Step 3: Query prebuild info for TLAS
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
        tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        tlasInputs.NumDescs = m_tlasInstanceCount;
        tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlasInputs.InstanceDescs = g_dxrState->tlasInstanceDescs->GetGPUVirtualAddress();

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &prebuildInfo);

        // Step 4: Allocate scratch and result buffers for TLAS
        uint64_t scratchSize = AlignUp(prebuildInfo.ScratchDataSizeInBytes, D3D12_RAYTRACING_AS_BYTE_ALIGNMENT);
        g_dxrState->tlasScratch = CreateUAVBuffer(device, scratchSize, D3D12_RESOURCE_STATE_COMMON);
        if (!g_dxrState->tlasScratch)
        {
            return;
        }

        m_tlasSize = AlignUp(prebuildInfo.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_AS_BYTE_ALIGNMENT);
        g_dxrState->tlasResult =
            CreateUAVBuffer(device, m_tlasSize, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        if (!g_dxrState->tlasResult)
        {
            return;
        }

        m_tlasResource = g_dxrState->tlasResult.Get();

        // Step 5: Build the TLAS
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
        buildDesc.DestAccelerationStructureData = g_dxrState->tlasResult->GetGPUVirtualAddress();
        buildDesc.Inputs = tlasInputs;
        buildDesc.ScratchAccelerationStructureData = g_dxrState->tlasScratch->GetGPUVirtualAddress();
        buildDesc.SourceAccelerationStructureData = 0;

        g_dxrState->commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

        // UAV barrier to ensure TLAS build completes before ray tracing
        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = g_dxrState->tlasResult.Get();
        g_dxrState->commandList->ResourceBarrier(1, &uavBarrier);

        ExecuteAndWait(g_dxrState);
    }

    // ============================================================================
    // TraceReflections
    // ============================================================================

    void DXRManager::TraceReflections(const XMMATRIX& viewProj, const XMFLOAT3& cameraPos)
    {
        (void)viewProj;
        (void)cameraPos;

        if (!m_isInitialized || !HasFeature(m_settings.enabledFeatures, RTFeature::Reflections))
        {
            return;
        }

        if (!g_dxrState || !g_dxrState->reflectionStateObject || !g_dxrState->reflectionShaderTable)
        {
            return;
        }

        if (!g_dxrState->tlasResult)
        {
            return;
        }

        // Bind the global root signature and TLAS
        g_dxrState->commandList->SetComputeRootSignature(g_dxrState->globalRootSignature.Get());
        g_dxrState->commandList->SetComputeRootShaderResourceView(0, g_dxrState->tlasResult->GetGPUVirtualAddress());

        // Dispatch reflection rays
        uint32_t width = static_cast<uint32_t>(static_cast<float>(g_dxrState->outputWidth) * m_settings.renderScale);
        uint32_t height = static_cast<uint32_t>(static_cast<float>(g_dxrState->outputHeight) * m_settings.renderScale);
        width = (std::max)(width, 1u);
        height = (std::max)(height, 1u);

        DispatchRaysForPass(g_dxrState, g_dxrState->reflectionStateObject.Get(),
                            g_dxrState->reflectionShaderTable.Get(), g_dxrState->shaderTableRecordSize,
                            g_dxrState->reflectionOutput.Get(), width, height);

        // UAV barrier on output
        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = g_dxrState->reflectionOutput.Get();
        g_dxrState->commandList->ResourceBarrier(1, &uavBarrier);

        ExecuteAndWait(g_dxrState);
    }

    // ============================================================================
    // TraceShadows
    // ============================================================================

    void DXRManager::TraceShadows(const XMFLOAT3& lightDirection)
    {
        (void)lightDirection;

        if (!m_isInitialized || !HasFeature(m_settings.enabledFeatures, RTFeature::Shadows))
        {
            return;
        }

        if (!g_dxrState || !g_dxrState->shadowStateObject || !g_dxrState->shadowShaderTable)
        {
            return;
        }

        if (!g_dxrState->tlasResult)
        {
            return;
        }

        // Bind the global root signature and TLAS
        g_dxrState->commandList->SetComputeRootSignature(g_dxrState->globalRootSignature.Get());
        g_dxrState->commandList->SetComputeRootShaderResourceView(0, g_dxrState->tlasResult->GetGPUVirtualAddress());

        // Dispatch shadow rays - shadow pass uses full resolution for crisp results
        uint32_t width = g_dxrState->outputWidth;
        uint32_t height = g_dxrState->outputHeight;
        width = (std::max)(width, 1u);
        height = (std::max)(height, 1u);

        DispatchRaysForPass(g_dxrState, g_dxrState->shadowStateObject.Get(), g_dxrState->shadowShaderTable.Get(),
                            g_dxrState->shaderTableRecordSize, g_dxrState->shadowOutput.Get(), width, height);

        // UAV barrier on output
        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = g_dxrState->shadowOutput.Get();
        g_dxrState->commandList->ResourceBarrier(1, &uavBarrier);

        ExecuteAndWait(g_dxrState);
    }

    // ============================================================================
    // TraceAmbientOcclusion
    // ============================================================================

    void DXRManager::TraceAmbientOcclusion(const XMMATRIX& viewProj, const XMFLOAT3& cameraPos)
    {
        (void)viewProj;
        (void)cameraPos;

        if (!m_isInitialized || !HasFeature(m_settings.enabledFeatures, RTFeature::AmbientOcclusion))
        {
            return;
        }

        if (!g_dxrState || !g_dxrState->aoStateObject || !g_dxrState->aoShaderTable)
        {
            return;
        }

        if (!g_dxrState->tlasResult)
        {
            return;
        }

        // Bind the global root signature and TLAS
        g_dxrState->commandList->SetComputeRootSignature(g_dxrState->globalRootSignature.Get());
        g_dxrState->commandList->SetComputeRootShaderResourceView(0, g_dxrState->tlasResult->GetGPUVirtualAddress());

        // AO is typically rendered at reduced resolution for performance
        uint32_t width = static_cast<uint32_t>(static_cast<float>(g_dxrState->outputWidth) * m_settings.renderScale);
        uint32_t height = static_cast<uint32_t>(static_cast<float>(g_dxrState->outputHeight) * m_settings.renderScale);
        width = (std::max)(width, 1u);
        height = (std::max)(height, 1u);

        DispatchRaysForPass(g_dxrState, g_dxrState->aoStateObject.Get(), g_dxrState->aoShaderTable.Get(),
                            g_dxrState->shaderTableRecordSize, g_dxrState->aoOutput.Get(), width, height);

        // UAV barrier on output
        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = g_dxrState->aoOutput.Get();
        g_dxrState->commandList->ResourceBarrier(1, &uavBarrier);

        ExecuteAndWait(g_dxrState);
    }

    // ============================================================================
    // TraceGlobalIllumination
    // ============================================================================

    void DXRManager::TraceGlobalIllumination(const XMMATRIX& viewProj, const XMFLOAT3& cameraPos)
    {
        (void)viewProj;
        (void)cameraPos;

        if (!m_isInitialized || !HasFeature(m_settings.enabledFeatures, RTFeature::GlobalIllumination))
        {
            return;
        }

        if (!g_dxrState || !g_dxrState->giStateObject || !g_dxrState->giShaderTable)
        {
            return;
        }

        if (!g_dxrState->tlasResult)
        {
            return;
        }

        // Bind the global root signature and TLAS
        g_dxrState->commandList->SetComputeRootSignature(g_dxrState->globalRootSignature.Get());
        g_dxrState->commandList->SetComputeRootShaderResourceView(0, g_dxrState->tlasResult->GetGPUVirtualAddress());

        // GI is expensive; always apply render scale
        uint32_t width = static_cast<uint32_t>(static_cast<float>(g_dxrState->outputWidth) * m_settings.renderScale);
        uint32_t height = static_cast<uint32_t>(static_cast<float>(g_dxrState->outputHeight) * m_settings.renderScale);
        width = (std::max)(width, 1u);
        height = (std::max)(height, 1u);

        DispatchRaysForPass(g_dxrState, g_dxrState->giStateObject.Get(), g_dxrState->giShaderTable.Get(),
                            g_dxrState->shaderTableRecordSize, g_dxrState->giOutput.Get(), width, height);

        // UAV barrier on output
        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = g_dxrState->giOutput.Get();
        g_dxrState->commandList->ResourceBarrier(1, &uavBarrier);

        ExecuteAndWait(g_dxrState);
    }

#else // !SPARK_D3D12_SUPPORT — stub fallbacks

    bool DXRManager::Initialize(void* d3d12Device)
    {
        if (!d3d12Device)
        {
            m_isAvailable = false;
            m_isInitialized = false;
            return false;
        }

        // DXR requires a D3D12 backend which is not available in this build.
        m_isAvailable = false;
        m_isInitialized = false;
        return false;
    }

    void DXRManager::Shutdown()
    {
        m_blasList.clear();
        m_tlasResource = nullptr;
        m_isInitialized = false;
    }

    uint32_t DXRManager::CreateBLAS(const BLASDesc& desc)
    {
        BLASData data;
        data.desc = desc;
        data.size = static_cast<uint64_t>(desc.vertexCount) * desc.vertexStride +
                    static_cast<uint64_t>(desc.indexCount) * sizeof(uint32_t);

        m_blasList.push_back(data);
        return static_cast<uint32_t>(m_blasList.size() - 1);
    }

    void DXRManager::UpdateBLAS(uint32_t blasIndex)
    {
        if (blasIndex >= m_blasList.size())
        {
            return;
        }
    }

    void DXRManager::DestroyBLAS(uint32_t blasIndex)
    {
        if (blasIndex >= m_blasList.size())
        {
            return;
        }
        m_blasList[blasIndex].resource = nullptr;
        m_blasList[blasIndex].size = 0;
    }

    void DXRManager::BuildTLAS(const std::vector<BLASInstance>& instances)
    {
        m_tlasInstanceCount = static_cast<uint32_t>(instances.size());
    }

    void DXRManager::TraceReflections(const XMMATRIX& viewProj, const XMFLOAT3& cameraPos)
    {
        (void)viewProj;
        (void)cameraPos;
    }

    void DXRManager::TraceShadows(const XMFLOAT3& lightDirection)
    {
        (void)lightDirection;
    }

    void DXRManager::TraceAmbientOcclusion(const XMMATRIX& viewProj, const XMFLOAT3& cameraPos)
    {
        (void)viewProj;
        (void)cameraPos;
    }

    void DXRManager::TraceGlobalIllumination(const XMMATRIX& viewProj, const XMFLOAT3& cameraPos)
    {
        (void)viewProj;
        (void)cameraPos;
    }

#endif // SPARK_D3D12_SUPPORT

    // ============================================================================
    // Shared code (settings, stats, console) — not D3D12-dependent
    // ============================================================================

    void DXRManager::SetSettings(const DXRSettings& settings)
    {
        m_settings = settings;
    }

    DXRManager::DXRStats DXRManager::GetStats() const
    {
        m_stats.blasCount = static_cast<uint32_t>(m_blasList.size());
        m_stats.tlasInstanceCount = m_tlasInstanceCount;
        m_stats.accelerationStructureMemory = 0;
        for (const auto& blas : m_blasList)
        {
            m_stats.accelerationStructureMemory += blas.size;
        }
        m_stats.accelerationStructureMemory += m_tlasSize;
        return m_stats;
    }

    std::string DXRManager::Console_GetStatus() const
    {
        std::ostringstream ss;
        ss << "=== DXR Status ===\n";
        ss << "Available: " << (m_isAvailable ? "Yes" : "No") << "\n";
        ss << "Initialized: " << (m_isInitialized ? "Yes" : "No") << "\n";
        if (m_isInitialized)
        {
            auto stats = GetStats();
            ss << "BLAS Count: " << stats.blasCount << "\n";
            ss << "TLAS Instances: " << stats.tlasInstanceCount << "\n";
            ss << "AS Memory: " << (stats.accelerationStructureMemory / 1024) << " KB\n";
            ss << "Features:\n";
            ss << "  Reflections: " << (HasFeature(m_settings.enabledFeatures, RTFeature::Reflections) ? "ON" : "OFF")
               << "\n";
            ss << "  Shadows: " << (HasFeature(m_settings.enabledFeatures, RTFeature::Shadows) ? "ON" : "OFF") << "\n";
            ss << "  AO: " << (HasFeature(m_settings.enabledFeatures, RTFeature::AmbientOcclusion) ? "ON" : "OFF")
               << "\n";
            ss << "  GI: " << (HasFeature(m_settings.enabledFeatures, RTFeature::GlobalIllumination) ? "ON" : "OFF")
               << "\n";
        }
        return ss.str();
    }

    void DXRManager::Console_EnableFeature(const std::string& feature, bool enabled)
    {
        RTFeature flag = RTFeature::None;
        if (feature == "reflections")
        {
            flag = RTFeature::Reflections;
        }
        else if (feature == "shadows")
        {
            flag = RTFeature::Shadows;
        }
        else if (feature == "ao")
        {
            flag = RTFeature::AmbientOcclusion;
        }
        else if (feature == "gi")
        {
            flag = RTFeature::GlobalIllumination;
        }
        else
        {
            return;
        }

        if (enabled)
        {
            m_settings.enabledFeatures = m_settings.enabledFeatures | flag;
        }
        else
        {
            m_settings.enabledFeatures = static_cast<RTFeature>(static_cast<uint32_t>(m_settings.enabledFeatures) &
                                                                ~static_cast<uint32_t>(flag));
        }
    }

    void DXRManager::Console_SetQuality(const std::string& quality)
    {
        if (quality == "low")
        {
            m_settings.reflections.samplesPerPixel = 1;
            m_settings.reflections.maxBounces = 1;
            m_settings.shadows.samplesPerPixel = 1;
            m_settings.renderScale = 0.5f;
        }
        else if (quality == "medium")
        {
            m_settings.reflections.samplesPerPixel = 1;
            m_settings.reflections.maxBounces = 1;
            m_settings.shadows.samplesPerPixel = 2;
            m_settings.renderScale = 0.75f;
        }
        else if (quality == "high")
        {
            m_settings.reflections.samplesPerPixel = 2;
            m_settings.reflections.maxBounces = 2;
            m_settings.shadows.samplesPerPixel = 4;
            m_settings.renderScale = 1.0f;
        }
        else if (quality == "ultra")
        {
            m_settings.reflections.samplesPerPixel = 4;
            m_settings.reflections.maxBounces = 3;
            m_settings.shadows.samplesPerPixel = 8;
            m_settings.globalIllumination.maxBounces = 3;
            m_settings.renderScale = 1.0f;
        }
    }

} // namespace Spark::Graphics
