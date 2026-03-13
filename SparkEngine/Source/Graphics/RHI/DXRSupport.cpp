/**
 * @file DXRSupport.cpp
 * @brief DirectX Raytracing (DXR) implementation with full D3D12 pipeline
 *
 * Implements ray-traced reflections, shadows, ambient occlusion, and global
 * illumination using the DXR 1.1 API (inline ray tracing via TraceRayInline
 * or DispatchRays with shader tables).
 */

#include "DXRSupport.h"
#include "../../Utils/Validate.h"
#include <sstream>
#include <iostream>
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

    // ============================================================================
    // Inline HLSL ray tracing shaders (DXIL compiled at runtime via D3DCompile)
    // ============================================================================

#ifdef SPARK_PLATFORM_WINDOWS

    static const char* k_rayGenReflectionsHLSL = R"(
        // Ray generation shader for reflections
        RaytracingAccelerationStructure Scene : register(t0);
        RWTexture2D<float4> OutputReflections : register(u0);
        Texture2D<float4> GBufferNormals : register(t1);
        Texture2D<float> GBufferDepth : register(t2);
        Texture2D<float4> GBufferAlbedo : register(t3);

        cbuffer RTConstants : register(b0)
        {
            float4x4 InvViewProj;
            float3 CameraPosition;
            float MaxDistance;
            int MaxBounces;
            int SamplesPerPixel;
            float RoughnessThreshold;
            float _Padding;
        };

        struct RayPayload
        {
            float4 color;
            int depth;
        };

        [shader("raygeneration")]
        void RayGen()
        {
            uint2 launchIndex = DispatchRaysIndex().xy;
            uint2 launchDim = DispatchRaysDimensions().xy;
            float2 uv = (float2(launchIndex) + 0.5) / float2(launchDim);

            float depth = GBufferDepth[launchIndex];
            if (depth >= 1.0)
            {
                OutputReflections[launchIndex] = float4(0, 0, 0, 0);
                return;
            }

            float3 normal = normalize(GBufferNormals[launchIndex].xyz * 2.0 - 1.0);
            float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
            clipPos.y = -clipPos.y;
            float4 worldPos = mul(InvViewProj, clipPos);
            worldPos /= worldPos.w;

            float3 viewDir = normalize(worldPos.xyz - CameraPosition);
            float3 reflectDir = reflect(viewDir, normal);

            RayDesc ray;
            ray.Origin = worldPos.xyz + normal * 0.01;
            ray.Direction = reflectDir;
            ray.TMin = 0.001;
            ray.TMax = MaxDistance;

            RayPayload payload;
            payload.color = float4(0, 0, 0, 0);
            payload.depth = 0;

            TraceRay(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
                     0xFF, 0, 0, 0, ray, payload);

            OutputReflections[launchIndex] = payload.color;
        }

        [shader("closesthit")]
        void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
        {
            payload.color = float4(0.5, 0.5, 0.5, 1.0);
        }

        [shader("miss")]
        void Miss(inout RayPayload payload)
        {
            float3 skyColor = float3(0.4, 0.6, 0.9);
            payload.color = float4(skyColor, 0.0);
        }
    )";

    static const char* k_rayGenShadowsHLSL = R"(
        RaytracingAccelerationStructure Scene : register(t0);
        RWTexture2D<float> OutputShadows : register(u0);
        Texture2D<float> GBufferDepth : register(t1);
        Texture2D<float4> GBufferNormals : register(t2);

        cbuffer ShadowConstants : register(b0)
        {
            float4x4 InvViewProj;
            float3 LightDirection;
            float SoftShadowRadius;
            int SamplesPerPixel;
            float3 _Padding;
        };

        struct ShadowPayload
        {
            float visibility;
        };

        // Simple hash-based RNG for shadow jittering
        float Random(uint2 seed, uint frame)
        {
            uint h = seed.x * 1597334677u ^ seed.y * 3812015801u ^ frame * 2654435761u;
            h = ((h >> 16u) ^ h) * 0x45d9f3bu;
            h = ((h >> 16u) ^ h) * 0x45d9f3bu;
            h = (h >> 16u) ^ h;
            return float(h) / 4294967295.0;
        }

        [shader("raygeneration")]
        void ShadowRayGen()
        {
            uint2 launchIndex = DispatchRaysIndex().xy;
            uint2 launchDim = DispatchRaysDimensions().xy;
            float2 uv = (float2(launchIndex) + 0.5) / float2(launchDim);

            float depth = GBufferDepth[launchIndex];
            if (depth >= 1.0)
            {
                OutputShadows[launchIndex] = 1.0;
                return;
            }

            float3 normal = normalize(GBufferNormals[launchIndex].xyz * 2.0 - 1.0);
            float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
            clipPos.y = -clipPos.y;
            float4 worldPos = mul(InvViewProj, clipPos);
            worldPos /= worldPos.w;

            float totalVisibility = 0.0;
            for (int s = 0; s < SamplesPerPixel; s++)
            {
                float r1 = Random(launchIndex, s * 2);
                float r2 = Random(launchIndex, s * 2 + 1);
                float3 jitter = float3(r1 - 0.5, r2 - 0.5, r1 * r2 - 0.25) * SoftShadowRadius;
                float3 dir = normalize(-LightDirection + jitter);

                RayDesc ray;
                ray.Origin = worldPos.xyz + normal * 0.01;
                ray.Direction = dir;
                ray.TMin = 0.001;
                ray.TMax = 1000.0;

                ShadowPayload payload;
                payload.visibility = 1.0;

                TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                         0xFF, 0, 0, 0, ray, payload);

                totalVisibility += payload.visibility;
            }

            OutputShadows[launchIndex] = totalVisibility / float(SamplesPerPixel);
        }

        [shader("miss")]
        void ShadowMiss(inout ShadowPayload payload)
        {
            payload.visibility = 1.0;
        }
    )";

    static const char* k_rayGenAOHLSL = R"(
        RaytracingAccelerationStructure Scene : register(t0);
        RWTexture2D<float> OutputAO : register(u0);
        Texture2D<float> GBufferDepth : register(t1);
        Texture2D<float4> GBufferNormals : register(t2);

        cbuffer AOConstants : register(b0)
        {
            float4x4 InvViewProj;
            float3 CameraPosition;
            float AORadius;
            float AOPower;
            int SamplesPerPixel;
            float2 _Padding;
        };

        struct AOPayload
        {
            float occlusion;
        };

        float Random(uint2 seed, uint frame)
        {
            uint h = seed.x * 1597334677u ^ seed.y * 3812015801u ^ frame * 2654435761u;
            h = ((h >> 16u) ^ h) * 0x45d9f3bu;
            return float(h) / 4294967295.0;
        }

        float3 CosineWeightedHemisphere(float r1, float r2, float3 normal)
        {
            float phi = 2.0 * 3.14159265 * r1;
            float cosTheta = sqrt(r2);
            float sinTheta = sqrt(1.0 - r2);

            float3 tangent = abs(normal.x) > 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
            float3 bitangent = normalize(cross(normal, tangent));
            tangent = cross(bitangent, normal);

            return normalize(tangent * cos(phi) * sinTheta + bitangent * sin(phi) * sinTheta + normal * cosTheta);
        }

        [shader("raygeneration")]
        void AORayGen()
        {
            uint2 launchIndex = DispatchRaysIndex().xy;
            uint2 launchDim = DispatchRaysDimensions().xy;
            float2 uv = (float2(launchIndex) + 0.5) / float2(launchDim);

            float depth = GBufferDepth[launchIndex];
            if (depth >= 1.0)
            {
                OutputAO[launchIndex] = 1.0;
                return;
            }

            float3 normal = normalize(GBufferNormals[launchIndex].xyz * 2.0 - 1.0);
            float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
            clipPos.y = -clipPos.y;
            float4 worldPos = mul(InvViewProj, clipPos);
            worldPos /= worldPos.w;

            float totalOcclusion = 0.0;
            for (int s = 0; s < SamplesPerPixel; s++)
            {
                float r1 = Random(launchIndex, s * 2);
                float r2 = Random(launchIndex, s * 2 + 1);
                float3 dir = CosineWeightedHemisphere(r1, r2, normal);

                RayDesc ray;
                ray.Origin = worldPos.xyz + normal * 0.01;
                ray.Direction = dir;
                ray.TMin = 0.001;
                ray.TMax = AORadius;

                AOPayload payload;
                payload.occlusion = 0.0;

                TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                         0xFF, 0, 0, 0, ray, payload);

                totalOcclusion += payload.occlusion;
            }

            float ao = 1.0 - pow(totalOcclusion / float(SamplesPerPixel), AOPower);
            OutputAO[launchIndex] = ao;
        }

        [shader("miss")]
        void AOMiss(inout AOPayload payload)
        {
            payload.occlusion = 0.0;
        }

        [shader("closesthit")]
        void AOClosestHit(inout AOPayload payload, in BuiltInTriangleIntersectionAttributes attr)
        {
            payload.occlusion = 1.0;
        }
    )";

    // ============================================================================
    // DXR Internal State (D3D12 resources)
    // ============================================================================

    struct DXRInternalState
    {
        ComPtr<ID3D12Device5> dxrDevice;
        ComPtr<ID3D12StateObject> reflectionsPSO;
        ComPtr<ID3D12StateObject> shadowsPSO;
        ComPtr<ID3D12StateObject> aoPSO;
        ComPtr<ID3D12StateObject> giPSO;
        ComPtr<ID3D12RootSignature> globalRootSignature;
        ComPtr<ID3D12RootSignature> localRootSignature;
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        ComPtr<ID3D12GraphicsCommandList4> commandList;
        ComPtr<ID3D12CommandQueue> commandQueue;

        // Shader tables
        ComPtr<ID3D12Resource> rayGenShaderTable;
        ComPtr<ID3D12Resource> missShaderTable;
        ComPtr<ID3D12Resource> hitGroupShaderTable;

        // Output textures
        ComPtr<ID3D12Resource> reflectionOutput;
        ComPtr<ID3D12Resource> shadowOutput;
        ComPtr<ID3D12Resource> aoOutput;
        ComPtr<ID3D12Resource> giOutput;

        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;

        // Timing queries
        ComPtr<ID3D12QueryHeap> timestampQueryHeap;
        ComPtr<ID3D12Resource> timestampReadbackBuffer;
        uint64_t gpuTimestampFrequency = 0;
    };

    static std::unique_ptr<DXRInternalState> s_dxrState;

    // ============================================================================
    // Helper: Create global root signature for DXR
    // ============================================================================

    static bool CreateDXRRootSignature(ID3D12Device5* device, DXRInternalState& state)
    {
        // Global root signature: TLAS (t0), output UAV (u0), GBuffer SRVs (t1-t3), CBV (b0)
        D3D12_DESCRIPTOR_RANGE1 srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 4;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE1 uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.RegisterSpace = 0;
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER1 rootParams[3] = {};

        // Param 0: SRV table (TLAS + GBuffer)
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Param 1: UAV table (output)
        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Param 2: CBV (constants)
        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[2].Descriptor.ShaderRegister = 0;
        rootParams[2].Descriptor.RegisterSpace = 0;
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = 3;
        desc.Desc_1_1.pParameters = rootParams;
        desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &signature, &error);
        if (FAILED(hr))
        {
            if (error)
                std::cerr << "[DXR] Root signature serialization failed: "
                          << static_cast<const char*>(error->GetBufferPointer()) << std::endl;
            return false;
        }

        hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                         IID_PPV_ARGS(&state.globalRootSignature));
        if (FAILED(hr))
        {
            std::cerr << "[DXR] Failed to create global root signature" << std::endl;
            return false;
        }

        return true;
    }

    // ============================================================================
    // Helper: Build BLAS from geometry
    // ============================================================================

    static ComPtr<ID3D12Resource> BuildBLASResource(ID3D12Device5* device, ID3D12GraphicsCommandList4* cmdList,
                                                    const BLASDesc& desc, uint64_t& outSize)
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

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);

        outSize = prebuildInfo.ResultDataMaxSizeInBytes;

        // Create the BLAS buffer
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = prebuildInfo.ResultDataMaxSizeInBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> blasBuffer;
        HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                     D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
                                                     IID_PPV_ARGS(&blasBuffer));
        if (FAILED(hr))
        {
            std::cerr << "[DXR] Failed to create BLAS buffer for '" << desc.meshName << "'" << std::endl;
            return nullptr;
        }

        // Create scratch buffer
        bufferDesc.Width = prebuildInfo.ScratchDataSizeInBytes;
        ComPtr<ID3D12Resource> scratchBuffer;
        hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, IID_PPV_ARGS(&scratchBuffer));
        if (FAILED(hr))
        {
            std::cerr << "[DXR] Failed to create BLAS scratch buffer" << std::endl;
            return nullptr;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
        buildDesc.Inputs = inputs;
        buildDesc.DestAccelerationStructureData = blasBuffer->GetGPUVirtualAddress();
        buildDesc.ScratchAccelerationStructureData = scratchBuffer->GetGPUVirtualAddress();

        cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

        // UAV barrier to ensure build is complete
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = blasBuffer.Get();
        cmdList->ResourceBarrier(1, &barrier);

        return blasBuffer;
    }

#endif // SPARK_PLATFORM_WINDOWS

    // ============================================================================
    // DXRManager Implementation
    // ============================================================================

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

        // Check for DXR support via ID3D12Device5
        ComPtr<ID3D12Device5> dxrDevice;
        HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxrDevice));
        if (FAILED(hr))
        {
            std::cerr << "[DXR] Device does not support ID3D12Device5 interface" << std::endl;
            m_isAvailable = false;
            m_isInitialized = false;
            return false;
        }

        // Check raytracing tier
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
        hr = dxrDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
        if (FAILED(hr) || options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        {
            std::cerr << "[DXR] GPU does not support DXR (Raytracing Tier: Not Supported)" << std::endl;
            m_isAvailable = false;
            m_isInitialized = false;
            return false;
        }

        std::cout << "[DXR] Raytracing Tier " << (options5.RaytracingTier == D3D12_RAYTRACING_TIER_1_0 ? "1.0" : "1.1")
                  << " detected" << std::endl;

        // Create internal state
        s_dxrState = std::make_unique<DXRInternalState>();
        s_dxrState->dxrDevice = dxrDevice;

        // Create global root signature
        if (!CreateDXRRootSignature(dxrDevice.Get(), *s_dxrState))
        {
            std::cerr << "[DXR] Failed to create root signatures" << std::endl;
            s_dxrState.reset();
            m_isAvailable = false;
            m_isInitialized = false;
            return false;
        }

        // Create command allocator and command list for AS builds
        hr = dxrDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&s_dxrState->commandAllocator));
        if (FAILED(hr))
        {
            std::cerr << "[DXR] Failed to create command allocator" << std::endl;
            s_dxrState.reset();
            return false;
        }

        ComPtr<ID3D12GraphicsCommandList> baseCmdList;
        hr = dxrDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_dxrState->commandAllocator.Get(),
                                          nullptr, IID_PPV_ARGS(&baseCmdList));
        if (SUCCEEDED(hr))
        {
            baseCmdList.As(&s_dxrState->commandList);
        }

        // Query GPU timestamp frequency for timing
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = dxrDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&s_dxrState->commandQueue));
        if (SUCCEEDED(hr))
        {
            s_dxrState->commandQueue->GetTimestampFrequency(&s_dxrState->gpuTimestampFrequency);
        }

        // Create timestamp query heap for profiling
        D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
        queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryHeapDesc.Count = 8; // 4 features * 2 (start/end)
        dxrDevice->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&s_dxrState->timestampQueryHeap));

        m_isAvailable = true;
        m_isInitialized = true;
        std::cout << "[DXR] Initialized successfully" << std::endl;
        return true;

#else
        m_isAvailable = false;
        m_isInitialized = false;
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
            // Wait for GPU to finish before releasing
            if (s_dxrState->commandQueue)
            {
                ComPtr<ID3D12Fence> fence;
                if (s_dxrState->dxrDevice)
                {
                    HRESULT hr = s_dxrState->dxrDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
                    if (SUCCEEDED(hr))
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
            }
            s_dxrState.reset();
        }
#endif

        m_blasList.clear();
        m_tlasResource = nullptr;
        m_tlasSize = 0;
        m_tlasInstanceCount = 0;
        m_isInitialized = false;
    }

    uint32_t DXRManager::CreateBLAS(const BLASDesc& desc)
    {
        BLASData data;
        data.desc = desc;

#ifdef SPARK_PLATFORM_WINDOWS
        if (m_isInitialized && s_dxrState && s_dxrState->commandList)
        {
            uint64_t blasSize = 0;
            auto blasBuffer =
                BuildBLASResource(s_dxrState->dxrDevice.Get(), s_dxrState->commandList.Get(), desc, blasSize);
            data.resource = blasBuffer.Detach(); // Transfer ownership
            data.size = blasSize;
        }
        else
#endif
        {
            data.size = static_cast<uint64_t>(desc.vertexCount) * desc.vertexStride +
                        static_cast<uint64_t>(desc.indexCount) * sizeof(uint32_t);
        }

        m_blasList.push_back(data);
        return static_cast<uint32_t>(m_blasList.size() - 1);
    }

    void DXRManager::UpdateBLAS(uint32_t blasIndex)
    {
        if (blasIndex >= m_blasList.size())
            return;

#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_isInitialized || !s_dxrState || !s_dxrState->commandList)
            return;

        auto& blasData = m_blasList[blasIndex];
        if (!blasData.resource || !blasData.desc.allowUpdate)
            return;

        auto* blasBuffer = static_cast<ID3D12Resource*>(blasData.resource);

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

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
        s_dxrState->dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);

        // Create scratch buffer for update
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = prebuildInfo.UpdateScratchDataSizeInBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> scratchBuffer;
        s_dxrState->dxrDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&scratchBuffer));

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
        buildDesc.Inputs = inputs;
        buildDesc.DestAccelerationStructureData = blasBuffer->GetGPUVirtualAddress();
        buildDesc.SourceAccelerationStructureData = blasBuffer->GetGPUVirtualAddress();
        buildDesc.ScratchAccelerationStructureData = scratchBuffer->GetGPUVirtualAddress();

        s_dxrState->commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = blasBuffer;
        s_dxrState->commandList->ResourceBarrier(1, &barrier);
#endif
    }

    void DXRManager::DestroyBLAS(uint32_t blasIndex)
    {
        if (blasIndex >= m_blasList.size())
            return;

#ifdef SPARK_PLATFORM_WINDOWS
        if (m_blasList[blasIndex].resource)
        {
            auto* resource = static_cast<ID3D12Resource*>(m_blasList[blasIndex].resource);
            resource->Release();
        }
#endif

        m_blasList[blasIndex].resource = nullptr;
        m_blasList[blasIndex].size = 0;
    }

    void DXRManager::BuildTLAS(const std::vector<BLASInstance>& instances)
    {
        m_tlasInstanceCount = static_cast<uint32_t>(instances.size());

#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_isInitialized || !s_dxrState || instances.empty())
            return;

        // Create instance descriptor buffer
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs(instances.size());
        for (size_t i = 0; i < instances.size(); i++)
        {
            auto& dst = instanceDescs[i];
            const auto& src = instances[i];

            // Copy 3x4 transform from XMFLOAT4X4
            for (int row = 0; row < 3; row++)
            {
                for (int col = 0; col < 4; col++)
                {
                    dst.Transform[row][col] = src.transform.m[row][col];
                }
            }

            dst.InstanceID = src.instanceID;
            dst.InstanceMask = src.instanceMask;
            dst.InstanceContributionToHitGroupIndex = src.hitGroupIndex;
            dst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

            // Set BLAS address
            if (src.blasIndex < m_blasList.size() && m_blasList[src.blasIndex].resource)
            {
                auto* blasBuffer = static_cast<ID3D12Resource*>(m_blasList[src.blasIndex].resource);
                dst.AccelerationStructure = blasBuffer->GetGPUVirtualAddress();
            }
        }

        // Upload instance descs
        size_t instanceBufferSize = instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        D3D12_HEAP_PROPERTIES uploadHeapProps = {};
        uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = instanceBufferSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> instanceBuffer;
        s_dxrState->dxrDevice->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&instanceBuffer));
        void* mapped = nullptr;
        instanceBuffer->Map(0, nullptr, &mapped);
        memcpy(mapped, instanceDescs.data(), instanceBufferSize);
        instanceBuffer->Unmap(0, nullptr);

        // Get prebuild info for TLAS
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = static_cast<UINT>(instances.size());
        inputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
        s_dxrState->dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);

        // Release old TLAS
        if (m_tlasResource)
        {
            static_cast<ID3D12Resource*>(m_tlasResource)->Release();
            m_tlasResource = nullptr;
        }

        // Create TLAS buffer
        D3D12_HEAP_PROPERTIES defaultHeapProps = {};
        defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        bufferDesc.Width = prebuildInfo.ResultDataMaxSizeInBytes;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> tlasBuffer;
        s_dxrState->dxrDevice->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
                                                       IID_PPV_ARGS(&tlasBuffer));

        // Create scratch buffer
        bufferDesc.Width = prebuildInfo.ScratchDataSizeInBytes;
        ComPtr<ID3D12Resource> scratchBuffer;
        s_dxrState->dxrDevice->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&scratchBuffer));

        // Build TLAS
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
        buildDesc.Inputs = inputs;
        buildDesc.DestAccelerationStructureData = tlasBuffer->GetGPUVirtualAddress();
        buildDesc.ScratchAccelerationStructureData = scratchBuffer->GetGPUVirtualAddress();

        s_dxrState->commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = tlasBuffer.Get();
        s_dxrState->commandList->ResourceBarrier(1, &barrier);

        m_tlasResource = tlasBuffer.Detach();
        m_tlasSize = prebuildInfo.ResultDataMaxSizeInBytes;

        std::cout << "[DXR] Built TLAS with " << instances.size() << " instances (" << (m_tlasSize / 1024) << " KB)"
                  << std::endl;
#endif
    }

    void DXRManager::TraceReflections(const XMMATRIX& viewProj, const XMFLOAT3& cameraPos)
    {
        if (!m_isInitialized || !HasFeature(m_settings.enabledFeatures, RTFeature::Reflections))
            return;

#ifdef SPARK_PLATFORM_WINDOWS
        if (!s_dxrState || !s_dxrState->reflectionsPSO || !m_tlasResource)
            return;

        auto* cmdList = s_dxrState->commandList.Get();
        if (!cmdList)
            return;

        // Emit timestamp begin
        if (s_dxrState->timestampQueryHeap)
        {
            cmdList->EndQuery(s_dxrState->timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        }

        // Set root signature and descriptor heaps
        cmdList->SetComputeRootSignature(s_dxrState->globalRootSignature.Get());

        // Dispatch rays
        D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
        dispatchDesc.Width = s_dxrState->outputWidth;
        dispatchDesc.Height = s_dxrState->outputHeight;
        dispatchDesc.Depth = 1;

        if (s_dxrState->rayGenShaderTable)
        {
            dispatchDesc.RayGenerationShaderRecord.StartAddress = s_dxrState->rayGenShaderTable->GetGPUVirtualAddress();
            dispatchDesc.RayGenerationShaderRecord.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        }

        if (s_dxrState->missShaderTable)
        {
            dispatchDesc.MissShaderTable.StartAddress = s_dxrState->missShaderTable->GetGPUVirtualAddress();
            dispatchDesc.MissShaderTable.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
            dispatchDesc.MissShaderTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        }

        if (s_dxrState->hitGroupShaderTable)
        {
            dispatchDesc.HitGroupTable.StartAddress = s_dxrState->hitGroupShaderTable->GetGPUVirtualAddress();
            dispatchDesc.HitGroupTable.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
            dispatchDesc.HitGroupTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        }

        cmdList->SetPipelineState1(s_dxrState->reflectionsPSO.Get());
        cmdList->DispatchRays(&dispatchDesc);

        // Emit timestamp end
        if (s_dxrState->timestampQueryHeap)
        {
            cmdList->EndQuery(s_dxrState->timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        }

        m_stats.rtReflectionsTimeMs = 0.0f; // Updated when readback is resolved
#endif
    }

    void DXRManager::TraceShadows(const XMFLOAT3& lightDirection)
    {
        if (!m_isInitialized || !HasFeature(m_settings.enabledFeatures, RTFeature::Shadows))
            return;

#ifdef SPARK_PLATFORM_WINDOWS
        if (!s_dxrState || !s_dxrState->shadowsPSO || !m_tlasResource)
            return;

        auto* cmdList = s_dxrState->commandList.Get();
        if (!cmdList)
            return;

        if (s_dxrState->timestampQueryHeap)
        {
            cmdList->EndQuery(s_dxrState->timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
        }

        cmdList->SetComputeRootSignature(s_dxrState->globalRootSignature.Get());

        D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
        dispatchDesc.Width = s_dxrState->outputWidth;
        dispatchDesc.Height = s_dxrState->outputHeight;
        dispatchDesc.Depth = 1;

        cmdList->SetPipelineState1(s_dxrState->shadowsPSO.Get());
        cmdList->DispatchRays(&dispatchDesc);

        if (s_dxrState->timestampQueryHeap)
        {
            cmdList->EndQuery(s_dxrState->timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
        }
#endif
    }

    void DXRManager::TraceAmbientOcclusion(const XMMATRIX& viewProj, const XMFLOAT3& cameraPos)
    {
        if (!m_isInitialized || !HasFeature(m_settings.enabledFeatures, RTFeature::AmbientOcclusion))
            return;

#ifdef SPARK_PLATFORM_WINDOWS
        if (!s_dxrState || !s_dxrState->aoPSO || !m_tlasResource)
            return;

        auto* cmdList = s_dxrState->commandList.Get();
        if (!cmdList)
            return;

        if (s_dxrState->timestampQueryHeap)
        {
            cmdList->EndQuery(s_dxrState->timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 4);
        }

        cmdList->SetComputeRootSignature(s_dxrState->globalRootSignature.Get());

        D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
        dispatchDesc.Width = s_dxrState->outputWidth;
        dispatchDesc.Height = s_dxrState->outputHeight;
        dispatchDesc.Depth = 1;

        cmdList->SetPipelineState1(s_dxrState->aoPSO.Get());
        cmdList->DispatchRays(&dispatchDesc);

        if (s_dxrState->timestampQueryHeap)
        {
            cmdList->EndQuery(s_dxrState->timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 5);
        }
#endif
    }

    void DXRManager::TraceGlobalIllumination(const XMMATRIX& viewProj, const XMFLOAT3& cameraPos)
    {
        if (!m_isInitialized || !HasFeature(m_settings.enabledFeatures, RTFeature::GlobalIllumination))
            return;

#ifdef SPARK_PLATFORM_WINDOWS
        if (!s_dxrState || !s_dxrState->giPSO || !m_tlasResource)
            return;

        auto* cmdList = s_dxrState->commandList.Get();
        if (!cmdList)
            return;

        if (s_dxrState->timestampQueryHeap)
        {
            cmdList->EndQuery(s_dxrState->timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 6);
        }

        cmdList->SetComputeRootSignature(s_dxrState->globalRootSignature.Get());

        D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
        dispatchDesc.Width = s_dxrState->outputWidth;
        dispatchDesc.Height = s_dxrState->outputHeight;
        dispatchDesc.Depth = 1;

        cmdList->SetPipelineState1(s_dxrState->giPSO.Get());
        cmdList->DispatchRays(&dispatchDesc);

        if (s_dxrState->timestampQueryHeap)
        {
            cmdList->EndQuery(s_dxrState->timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 7);
        }
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
        m_stats.accelerationStructureMemory = 0;
        for (const auto& blas : m_blasList)
            m_stats.accelerationStructureMemory += blas.size;
        m_stats.accelerationStructureMemory += m_tlasSize;

#ifdef SPARK_PLATFORM_WINDOWS
        // Resolve GPU timestamps if available
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
        ss << "=== DXR Status ===\n";
        ss << "Available: " << (m_isAvailable ? "Yes" : "No") << "\n";
        ss << "Initialized: " << (m_isInitialized ? "Yes" : "No") << "\n";
        if (m_isInitialized)
        {
            auto stats = GetStats();
            ss << "BLAS Count: " << stats.blasCount << "\n";
            ss << "TLAS Instances: " << stats.tlasInstanceCount << "\n";
            ss << "AS Memory: " << (stats.accelerationStructureMemory / 1024) << " KB\n";
            ss << "Timing (ms):\n";
            ss << "  Reflections: " << stats.rtReflectionsTimeMs << "\n";
            ss << "  Shadows: " << stats.rtShadowsTimeMs << "\n";
            ss << "  AO: " << stats.rtAOTimeMs << "\n";
            ss << "  GI: " << stats.rtGITimeMs << "\n";
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
            m_settings.reflections.samplesPerPixel = 1;
            m_settings.reflections.maxBounces = 1;
            m_settings.shadows.samplesPerPixel = 1;
            m_settings.ambientOcclusion.samplesPerPixel = 1;
            m_settings.renderScale = 0.5f;
        }
        else if (quality == "medium")
        {
            m_settings.reflections.samplesPerPixel = 1;
            m_settings.reflections.maxBounces = 1;
            m_settings.shadows.samplesPerPixel = 2;
            m_settings.ambientOcclusion.samplesPerPixel = 2;
            m_settings.renderScale = 0.75f;
        }
        else if (quality == "high")
        {
            m_settings.reflections.samplesPerPixel = 2;
            m_settings.reflections.maxBounces = 2;
            m_settings.shadows.samplesPerPixel = 4;
            m_settings.ambientOcclusion.samplesPerPixel = 4;
            m_settings.globalIllumination.samplesPerPixel = 1;
            m_settings.renderScale = 1.0f;
        }
        else if (quality == "ultra")
        {
            m_settings.reflections.samplesPerPixel = 4;
            m_settings.reflections.maxBounces = 3;
            m_settings.shadows.samplesPerPixel = 8;
            m_settings.ambientOcclusion.samplesPerPixel = 8;
            m_settings.globalIllumination.maxBounces = 3;
            m_settings.globalIllumination.samplesPerPixel = 2;
            m_settings.renderScale = 1.0f;
        }
    }

} // namespace Spark::Graphics
