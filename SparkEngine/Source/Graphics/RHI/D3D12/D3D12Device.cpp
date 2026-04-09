/**
 * @file D3D12Device.cpp
 * @brief Complete DirectX 12 RHI backend implementation
 * @author Spark Engine Team
 * @date 2026
 *
 * Full D3D12 device implementation: DXGI factory, adapter enumeration,
 * device creation, command queues, descriptor heaps, resource management,
 * command list recording, swap chain, and frame synchronization.
 *
 * RHI Ownership Model: Create*() methods return raw pointers. The RHI device
 * owns the underlying GPU resource. Callers must call the corresponding
 * Destroy*() method to release. This pattern is intentional — it matches
 * the D3D11/D3D12/Vulkan/OpenGL resource lifecycle and avoids forcing
 * std::unique_ptr across the backend-agnostic RHI boundary.
 */

#ifdef _WIN32

#include "D3D12Device.h"
#include "../RHIFactory.h"
#include "../RHIFormatUtils.h"
#include "../../../Utils/LogMacros.h"
#include "../../../Utils/Validate.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <sstream>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Compatibility logging macros — bridge std::format syntax to SPARK_LOG_*
// ---------------------------------------------------------------------------
#define LOG_ERROR(fmt, ...)                                                                                            \
    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "%s", std::format(fmt __VA_OPT__(, ) __VA_ARGS__).c_str())
#define LOG_INFO(fmt, ...)                                                                                             \
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "%s", std::format(fmt __VA_OPT__(, ) __VA_ARGS__).c_str())

// ---------------------------------------------------------------------------
// D3D12CalcSubresource — normally provided by d3dx12.h helper header
// ---------------------------------------------------------------------------
static inline UINT D3D12CalcSubresource(UINT MipSlice, UINT ArraySlice, UINT PlaneSlice, UINT MipLevels, UINT ArraySize)
{
    return MipSlice + ArraySlice * MipLevels + PlaneSlice * MipLevels * ArraySize;
}

namespace Spark
{
    namespace RHI
    {
        namespace D3D12
        {

            // ============================================================================
            // DESCRIPTOR HEAP ALLOCATOR
            // ============================================================================

            bool DescriptorHeapAllocator::Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                     uint32_t descriptorCount, bool shaderVisible)
            {
                D3D12_DESCRIPTOR_HEAP_DESC desc = {};
                desc.Type = type;
                desc.NumDescriptors = descriptorCount;
                desc.Flags =
                    shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

                HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap));
                if (FAILED(hr))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Failed to create descriptor heap (type=%d)",
                                    type);
                    return false;
                }

                m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
                if (shaderVisible)
                    m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
                m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);
                m_capacity = descriptorCount;
                m_nextFreeIndex = 0;
                return true;
            }

            DescriptorAllocation DescriptorHeapAllocator::Allocate(uint32_t count)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                DescriptorAllocation alloc = {};

                // Try free list first for single descriptors
                if (count == 1 && !m_freeList.empty())
                {
                    uint32_t index = m_freeList.back();
                    m_freeList.pop_back();
                    alloc.index = index;
                    alloc.count = 1;
                    alloc.cpuHandle.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_descriptorSize;
                    alloc.gpuHandle.ptr = m_gpuStart.ptr + static_cast<UINT64>(index) * m_descriptorSize;
                    return alloc;
                }

                // Bump allocator
                if (m_nextFreeIndex + count > m_capacity)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Descriptor heap exhausted");
                    return alloc;
                }

                alloc.index = m_nextFreeIndex;
                alloc.count = count;
                alloc.cpuHandle.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(m_nextFreeIndex) * m_descriptorSize;
                alloc.gpuHandle.ptr = m_gpuStart.ptr + static_cast<UINT64>(m_nextFreeIndex) * m_descriptorSize;
                m_nextFreeIndex += count;
                return alloc;
            }

            void DescriptorHeapAllocator::Free(const DescriptorAllocation& allocation)
            {
                if (!allocation.IsValid())
                    return;
                std::lock_guard<std::mutex> lock(m_mutex);
                for (uint32_t i = 0; i < allocation.count; i++)
                    m_freeList.push_back(allocation.index + i);
            }

            // ============================================================================
            // D3D12 FENCE
            // ============================================================================

            D3D12Fence::~D3D12Fence()
            {
                if (m_fenceEvent)
                    CloseHandle(m_fenceEvent);
            }

            bool D3D12Fence::Initialize(ID3D12Device* device, uint64_t initialValue)
            {
                m_currentValue = initialValue;
                HRESULT hr = device->CreateFence(initialValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
                if (FAILED(hr))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Failed to create fence");
                    return false;
                }
                m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                return m_fenceEvent != nullptr;
            }

            uint64_t D3D12Fence::Signal(ID3D12CommandQueue* queue)
            {
                m_currentValue++;
                queue->Signal(m_fence.Get(), m_currentValue);
                return m_currentValue;
            }

            void D3D12Fence::WaitForValue(uint64_t value) const
            {
                if (m_fence->GetCompletedValue() < value)
                {
                    m_fence->SetEventOnCompletion(value, m_fenceEvent);
                    WaitForSingleObject(m_fenceEvent, INFINITE);
                }
            }

            void D3D12Fence::WaitForIdle() const
            {
                WaitForValue(m_currentValue);
            }

            uint64_t D3D12Fence::GetCompletedValue() const
            {
                return m_fence ? m_fence->GetCompletedValue() : 0;
            }

            // ============================================================================
            // D3D12 RESOURCE CONSTRUCTORS
            // ============================================================================

            D3D12Buffer::D3D12Buffer(const RHIBufferDesc& desc, ComPtr<ID3D12Resource> resource,
                                     ComPtr<ID3D12Resource> uploadResource)
                : m_desc(desc), m_resource(std::move(resource)), m_uploadResource(std::move(uploadResource))
            {
            }

            D3D12Texture::D3D12Texture(const RHITextureDesc& desc, ComPtr<ID3D12Resource> resource,
                                       const DescriptorAllocation& srvDescriptor,
                                       const DescriptorAllocation& rtvDescriptor,
                                       const DescriptorAllocation& dsvDescriptor,
                                       const DescriptorAllocation& uavDescriptor)
                : m_desc(desc), m_resource(std::move(resource)), m_srvDescriptor(srvDescriptor),
                  m_rtvDescriptor(rtvDescriptor), m_dsvDescriptor(dsvDescriptor), m_uavDescriptor(uavDescriptor)
            {
            }

            D3D12Shader::D3D12Shader(const RHIShaderDesc& desc, ComPtr<ID3DBlob> bytecodeBlob)
                : m_desc(desc), m_bytecodeBlob(std::move(bytecodeBlob))
            {
            }

            D3D12Sampler::D3D12Sampler(const RHISamplerDesc& desc, const DescriptorAllocation& descriptor)
                : m_desc(desc), m_descriptor(descriptor)
            {
            }

            D3D12PipelineState::D3D12PipelineState(const RHIPipelineStateDesc& desc, ComPtr<ID3D12PipelineState> pso,
                                                   ComPtr<ID3D12RootSignature> rootSignature)
                : m_desc(desc), m_pso(std::move(pso)), m_rootSignature(std::move(rootSignature))
            {
            }

            // ============================================================================
            // D3D12 DEVICE — INITIALIZATION
            // ============================================================================

            D3D12Device::D3D12Device() = default;

            D3D12Device::~D3D12Device()
            {
                Shutdown();
            }

            bool D3D12Device::Initialize(const RHIDeviceDesc& desc)
            {
                SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "D3D12Device::Initialize starting");
                m_debugEnabled = desc.enableDebugLayer;
                if (!CreateDevice(desc))
                    return false;
                if (!CreateCommandQueues())
                    return false;
                if (!CreateDescriptorHeaps())
                    return false;
                if (!CreateFrameResources())
                    return false;

                m_immediateCommandList =
                    std::make_unique<D3D12CommandList>(m_device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);

                DetectCapabilities();
                DetectDXRSupport();
                FinalizeDeviceCapabilities(m_capabilities);

                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "D3D12: Device initialized: %s",
                               m_capabilities.deviceName.c_str());
                return true;
            }

            void D3D12Device::Shutdown()
            {
                SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "D3D12Device::Shutdown");
                WaitForIdle();
                ProcessDeferredReleases();
                m_immediateCommandList.reset();
            }

            bool D3D12Device::CreateDevice(const RHIDeviceDesc& desc)
            {
                // Enable debug layer
                if (desc.enableDebugLayer)
                {
                    ComPtr<ID3D12Debug> debugController;
                    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
                    {
                        debugController->EnableDebugLayer();
                        if (desc.enableGPUValidation)
                        {
                            ComPtr<ID3D12Debug1> debugController1;
                            if (SUCCEEDED(debugController.As(&debugController1)))
                                debugController1->SetEnableGPUBasedValidation(TRUE);
                        }
                    }
                }

                // Create DXGI factory
                UINT factoryFlags = desc.enableDebugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0;
                HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_dxgiFactory));
                if (FAILED(hr))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Failed to create DXGI factory");
                    return false;
                }

                // Enumerate adapters — pick the one with the most VRAM
                ComPtr<IDXGIAdapter1> bestAdapter;
                SIZE_T bestVRAM = 0;
                for (UINT i = 0;; i++)
                {
                    ComPtr<IDXGIAdapter1> adapter;
                    if (m_dxgiFactory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
                        break;
                    DXGI_ADAPTER_DESC1 adapterDesc;
                    adapter->GetDesc1(&adapterDesc);
                    if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                        continue;
                    // Check D3D12 support
                    if (SUCCEEDED(
                            D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr)))
                    {
                        if (adapterDesc.DedicatedVideoMemory > bestVRAM)
                        {
                            bestVRAM = adapterDesc.DedicatedVideoMemory;
                            bestAdapter = adapter;
                        }
                    }
                }

                if (!bestAdapter)
                {
                    // No hardware adapter — fall back to WARP (Microsoft's software rasterizer)
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "D3D12: No hardware GPU found — falling back to WARP software rasterizer");
                    ComPtr<IDXGIAdapter> warpAdapter;
                    hr = m_dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter));
                    if (FAILED(hr))
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: WARP adapter not available");
                        return false;
                    }
                    if (FAILED(warpAdapter.As(&bestAdapter)))
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Failed to query WARP adapter");
                        return false;
                    }
                    m_isSoftwareDevice = true;
                }
                m_adapter = bestAdapter;

                // Create device
                hr = D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
                if (FAILED(hr))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Failed to create D3D12 device");
                    return false;
                }

                // Info queue for debug messages
                if (desc.enableDebugLayer)
                {
                    m_device.As(&m_infoQueue);
                    if (m_infoQueue)
                    {
                        m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
                        m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
                    }
                }

                return true;
            }

            bool D3D12Device::CreateCommandQueues()
            {
                D3D12_COMMAND_QUEUE_DESC queueDesc = {};
                queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
                if (FAILED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_directQueue))))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Failed to create direct command queue");
                    return false;
                }

                queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
                if (FAILED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_copyQueue))))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Failed to create copy command queue");
                    return false;
                }

                queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
                if (FAILED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_computeQueue))))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Failed to create compute command queue");
                    return false;
                }

                return true;
            }

            bool D3D12Device::CreateDescriptorHeaps()
            {
                if (!m_cbvSrvUavHeap.Initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                CBV_SRV_UAV_HEAP_SIZE, true))
                    return false;
                if (!m_rtvHeap.Initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_HEAP_SIZE, false))
                    return false;
                if (!m_dsvHeap.Initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_HEAP_SIZE, false))
                    return false;
                if (!m_samplerHeap.Initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_HEAP_SIZE,
                                              true))
                    return false;
                return true;
            }

            bool D3D12Device::CreateFrameResources()
            {
                for (auto& frame : m_frameResources)
                {
                    HRESULT hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                  IID_PPV_ARGS(&frame.commandAllocator));
                    if (FAILED(hr))
                        return false;
                    frame.fenceValue = 0;
                }
                return m_frameFence.Initialize(m_device.Get(), 0);
            }

            void D3D12Device::DetectCapabilities()
            {
                DXGI_ADAPTER_DESC1 adapterDesc;
                m_adapter->GetDesc1(&adapterDesc);

                // Convert wide string to narrow
                char deviceName[256];
                wcstombs(deviceName, adapterDesc.Description, sizeof(deviceName));
                m_capabilities.deviceName = deviceName;
                m_capabilities.backend = GraphicsBackend::D3D12;
                m_capabilities.dedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
                m_capabilities.sharedSystemMemory = adapterDesc.SharedSystemMemory;

                switch (adapterDesc.VendorId)
                {
                case 0x10DE:
                    m_capabilities.vendorName = "NVIDIA";
                    break;
                case 0x1002:
                    m_capabilities.vendorName = "AMD";
                    break;
                case 0x8086:
                    m_capabilities.vendorName = "Intel";
                    break;
                case 0x1414:
                    m_capabilities.vendorName = "Microsoft";
                    break; // WARP
                default:
                    m_capabilities.vendorName = "Unknown";
                    break;
                }

                m_capabilities.apiVersion = "Direct3D 12.0";
                m_capabilities.isSoftwareDevice = m_isSoftwareDevice;
                m_capabilities.tessellationSupport = true;
                m_capabilities.computeShaderSupport = true;
                m_capabilities.geometryShaderSupport = true;
                m_capabilities.multiDrawIndirectSupport = true; // Always available in D3D12

                // Check feature levels
                D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
                if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
                {
                    m_capabilities.conservativeRasterSupport =
                        (options.ConservativeRasterizationTier != D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED);
                    m_capabilities.bindlessResourceSupport =
                        (options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3);
                }

                D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
                if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
                {
                    m_capabilities.meshShaderSupport =
                        (options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED);
                }

                // D3D12 root descriptors are the equivalent of Vulkan push descriptors —
                // always available in D3D12 via root signature inline descriptors.
                m_capabilities.pushDescriptorSupport = true;

                // Enhanced Barriers (D3D12_OPTIONS12) — modern barrier model that
                // parallels Vulkan's synchronization2. Available in Windows 11 22H2+
                // with Agility SDK 1.706.4+ or compatible drivers.
#ifdef __ID3D12Device10_FWD_DEFINED__
                D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
                if (SUCCEEDED(
                        m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))))
                {
                    m_capabilities.enhancedBarrierSupport = (options12.EnhancedBarriersSupported == TRUE);
                }
#endif

                // GPU Upload Heaps (D3D12_OPTIONS16) — allows GPU to read directly from
                // upload heaps, eliminating the copy step for frequently updated resources.
                // Equivalent to Vulkan 1.4's host image copy capability.
#ifdef __ID3D12Device12_FWD_DEFINED__
                D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16 = {};
                if (SUCCEEDED(
                        m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options16, sizeof(options16))))
                {
                    m_capabilities.hostImageCopySupport = (options16.GPUUploadHeapSupported == TRUE);
                }
#endif

                // Query actual max MSAA sample count
                for (uint32_t samples = 8; samples >= 2; samples /= 2)
                {
                    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msaaLevels = {};
                    msaaLevels.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    msaaLevels.SampleCount = samples;
                    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msaaLevels,
                                                                sizeof(msaaLevels))) &&
                        msaaLevels.NumQualityLevels > 0)
                    {
                        m_capabilities.maxMSAASamples = samples;
                        break;
                    }
                }
            }

            void D3D12Device::DetectDXRSupport()
            {
                if (SUCCEEDED(m_device.As(&m_dxrDevice)))
                {
                    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
                    if (SUCCEEDED(
                            m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
                    {
                        m_dxrSupported = (options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED);
                        m_capabilities.rayTracingSupport = m_dxrSupported;

                        // Populate detailed RT capabilities for HybridRTManager
                        // Per DirectX-Graphics-Samples patterns: check tier for feature level
                        m_capabilities.rayTracing.supportsHardwareRT = m_dxrSupported;
                        m_capabilities.rayTracing.raytracingTier = static_cast<uint32_t>(options5.RaytracingTier);
                        // Tier 1.1 enables inline RT (RayQuery in any shader stage),
                        // GPU-driven DispatchRays, and AddToStateObject for incremental PSO builds
                        m_capabilities.rayTracing.supportsInlineRT =
                            (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1);
                        m_capabilities.rayTracing.maxRecursionDepth =
                            m_dxrSupported ? 31 : 0; // DXR spec max recursion is 31
                        m_capabilities.rayTracing.bestBackend =
                            m_dxrSupported ? RayTracingBackend::HardwareDXR : RayTracingBackend::Software_SDFGI;
                    }
                }
                if (!m_dxrSupported)
                    m_dxrDevice.Reset();

                // VRS detection (independent of DXR — VRS works for rasterization too)
                D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6 = {};
                if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6))))
                {
                    bool hasVRS = (options6.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED);
                    m_capabilities.variableRateShadingSupport = hasVRS;
                    m_capabilities.rayTracing.supportsVRS = hasVRS;
                }
            }

            // ============================================================================
            // D3D12 DEVICE — SWAP CHAIN
            // ============================================================================

            std::unique_ptr<IRHISwapChain> D3D12Device::CreateSwapChain(const RHISwapChainDesc& desc)
            {
                return std::make_unique<D3D12SwapChain>(m_device.Get(), m_directQueue.Get(), m_dxgiFactory.Get(),
                                                        &m_rtvHeap, desc);
            }

            // ============================================================================
            // D3D12 DEVICE — RESOURCE CREATION
            // ============================================================================

            std::unique_ptr<IRHIBuffer> D3D12Device::CreateBuffer(const RHIBufferDesc& desc)
            {
                D3D12_HEAP_PROPERTIES heapProps = {};
                D3D12_RESOURCE_STATES initialState;

                bool isDynamic = (desc.access == RHIBufferAccess::Dynamic || desc.access == RHIBufferAccess::Staging);
                heapProps.Type = isDynamic ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
                initialState = isDynamic ? D3D12_RESOURCE_STATE_GENERIC_READ : GetInitialResourceState(desc.access);

                D3D12_RESOURCE_DESC bufferDesc = {};
                bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bufferDesc.Width = desc.size;
                bufferDesc.Height = 1;
                bufferDesc.DepthOrArraySize = 1;
                bufferDesc.MipLevels = 1;
                bufferDesc.SampleDesc.Count = 1;
                bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                ComPtr<ID3D12Resource> resource;
                HRESULT hr = m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                               initialState, nullptr, IID_PPV_ARGS(&resource));
                if (FAILED(hr))
                    return nullptr;

                ComPtr<ID3D12Resource> uploadResource;
                if (!isDynamic && desc.initialData)
                {
                    // Create upload buffer for initial data
                    D3D12_HEAP_PROPERTIES uploadHeap = {};
                    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
                    hr = m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(&uploadResource));
                    if (SUCCEEDED(hr) && uploadResource)
                    {
                        void* mapped = nullptr;
                        hr = uploadResource->Map(0, nullptr, &mapped);
                        if (SUCCEEDED(hr) && mapped)
                        {
                            memcpy(mapped, desc.initialData, desc.size);
                            uploadResource->Unmap(0, nullptr);
                        }
                    }
                }

                auto buffer = std::make_unique<D3D12Buffer>(desc, std::move(resource), std::move(uploadResource));

                // Map persistent pointer for dynamic buffers
                if (isDynamic)
                {
                    void* mapped = nullptr;
                    hr = buffer->GetD3D12Resource()->Map(0, nullptr, &mapped);
                    if (SUCCEEDED(hr) && mapped)
                    {
                        buffer->SetMappedPointer(mapped);
                        if (desc.initialData)
                            memcpy(mapped, desc.initialData, desc.size);
                    }
                }

                if (!desc.debugName.empty())
                {
                    std::wstring wname(desc.debugName.begin(), desc.debugName.end());
                    buffer->GetD3D12Resource()->SetName(wname.c_str());
                }

                return buffer;
            }

            std::unique_ptr<IRHITexture> D3D12Device::CreateTexture(const RHITextureDesc& desc)
            {
                D3D12_RESOURCE_DESC texDesc = {};
                texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                texDesc.Width = desc.width;
                texDesc.Height = desc.height;
                texDesc.DepthOrArraySize =
                    static_cast<UINT16>(desc.type == RHITextureType::Texture3D ? desc.depth : desc.arraySize);
                texDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
                texDesc.Format = ConvertFormat(desc.format);
                texDesc.SampleDesc.Count = desc.sampleCount;

                if (desc.usage & RHITextureUsage::RenderTarget)
                    texDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                if (desc.usage & RHITextureUsage::DepthStencil)
                    texDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                if (desc.usage & RHITextureUsage::UnorderedAccess)
                    texDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

                D3D12_HEAP_PROPERTIES heapProps = {};
                heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

                D3D12_CLEAR_VALUE* clearValue = nullptr;
                D3D12_CLEAR_VALUE cv = {};
                if (desc.usage & RHITextureUsage::RenderTarget)
                {
                    cv.Format = texDesc.Format;
                    memcpy(cv.Color, desc.clearColor, sizeof(float) * 4);
                    clearValue = &cv;
                }
                else if (desc.usage & RHITextureUsage::DepthStencil)
                {
                    cv.Format = texDesc.Format;
                    cv.DepthStencil.Depth = desc.clearDepth;
                    cv.DepthStencil.Stencil = desc.clearStencil;
                    clearValue = &cv;
                }

                ComPtr<ID3D12Resource> resource;
                HRESULT hr =
                    m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                      D3D12_RESOURCE_STATE_COMMON, clearValue, IID_PPV_ARGS(&resource));
                if (FAILED(hr))
                    return nullptr;

                DescriptorAllocation srvAlloc = {}, rtvAlloc = {}, dsvAlloc = {}, uavAlloc = {};

                if (desc.usage & RHITextureUsage::ShaderResource)
                {
                    srvAlloc = m_cbvSrvUavHeap.Allocate(1);
                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Format = texDesc.Format;
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvDesc.Texture2D.MipLevels = desc.mipLevels;
                    m_device->CreateShaderResourceView(resource.Get(), &srvDesc, srvAlloc.cpuHandle);
                }

                if (desc.usage & RHITextureUsage::RenderTarget)
                {
                    rtvAlloc = m_rtvHeap.Allocate(1);
                    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
                    rtvDesc.Format = texDesc.Format;
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    m_device->CreateRenderTargetView(resource.Get(), &rtvDesc, rtvAlloc.cpuHandle);
                }

                if (desc.usage & RHITextureUsage::DepthStencil)
                {
                    dsvAlloc = m_dsvHeap.Allocate(1);
                    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
                    dsvDesc.Format = texDesc.Format;
                    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                    m_device->CreateDepthStencilView(resource.Get(), &dsvDesc, dsvAlloc.cpuHandle);
                }

                if (desc.usage & RHITextureUsage::UnorderedAccess)
                {
                    uavAlloc = m_cbvSrvUavHeap.Allocate(1);
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
                    uavDesc.Format = texDesc.Format;
                    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    m_device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, uavAlloc.cpuHandle);
                }

                return std::make_unique<D3D12Texture>(desc, std::move(resource), srvAlloc, rtvAlloc, dsvAlloc,
                                                      uavAlloc);
            }

            std::unique_ptr<IRHITexture> D3D12Device::WrapNativeTexture(void* nativeHandle, const RHITextureDesc& desc)
            {
                if (!nativeHandle)
                    return nullptr;

                auto* nativeResource = static_cast<ID3D12Resource*>(nativeHandle);
                ComPtr<ID3D12Resource> resource;
                nativeResource->QueryInterface(IID_PPV_ARGS(&resource));

                DescriptorAllocation srvAlloc, rtvAlloc, dsvAlloc, uavAlloc;
                if (desc.usage & RHITextureUsage::ShaderResource)
                {
                    srvAlloc = m_cbvSrvUavHeap.Allocate(1);
                    if (srvAlloc.IsValid())
                    {
                        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                        srvDesc.Format = ConvertFormat(desc.format);
                        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srvDesc.Texture2D.MipLevels = desc.mipLevels;
                        m_device->CreateShaderResourceView(resource.Get(), &srvDesc, srvAlloc.cpuHandle);
                    }
                }

                return std::make_unique<D3D12Texture>(desc, std::move(resource), srvAlloc, rtvAlloc, dsvAlloc,
                                                      uavAlloc);
            }

            std::unique_ptr<IRHIShader> D3D12Device::CreateShader(const RHIShaderDesc& desc)
            {
                if (desc.bytecode && desc.bytecodeSize > 0)
                {
                    ComPtr<ID3DBlob> blob;
                    HRESULT hr = D3DCreateBlob(desc.bytecodeSize, &blob);
                    if (FAILED(hr))
                        return nullptr;
                    memcpy(blob->GetBufferPointer(), desc.bytecode, desc.bytecodeSize);
                    return std::make_unique<D3D12Shader>(desc, std::move(blob));
                }

                if (!desc.sourceCode.empty())
                {
                    const char* target = nullptr;
                    switch (desc.stage)
                    {
                    case RHIShaderStage::Vertex:
                        target = "vs_5_1";
                        break;
                    case RHIShaderStage::Pixel:
                        target = "ps_5_1";
                        break;
                    case RHIShaderStage::Geometry:
                        target = "gs_5_1";
                        break;
                    case RHIShaderStage::Hull:
                        target = "hs_5_1";
                        break;
                    case RHIShaderStage::Domain:
                        target = "ds_5_1";
                        break;
                    case RHIShaderStage::Compute:
                        target = "cs_5_1";
                        break;
                    case RHIShaderStage::RayGeneration:
                    case RHIShaderStage::ClosestHit:
                    case RHIShaderStage::Miss:
                    case RHIShaderStage::AnyHit:
                    case RHIShaderStage::Intersection:
                    case RHIShaderStage::Callable:
                        target = "lib_6_5"; // DXR shader library target
                        break;
                    }

                    UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
                    if (m_debugEnabled)
                        compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;

                    ComPtr<ID3DBlob> bytecode;
                    ComPtr<ID3DBlob> errors;
                    HRESULT hr = D3DCompile(desc.sourceCode.data(), desc.sourceCode.size(), desc.filePath.c_str(),
                                            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, desc.entryPoint.c_str(), target,
                                            compileFlags, 0, &bytecode, &errors);
                    if (FAILED(hr))
                    {
                        if (errors)
                            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Shader compile error: %s",
                                            static_cast<const char*>(errors->GetBufferPointer()));
                        return nullptr;
                    }
                    return std::make_unique<D3D12Shader>(desc, std::move(bytecode));
                }

                return nullptr;
            }

            std::unique_ptr<IRHISampler> D3D12Device::CreateSampler(const RHISamplerDesc& desc)
            {
                auto alloc = m_samplerHeap.Allocate(1);
                if (!alloc.IsValid())
                    return nullptr;

                D3D12_SAMPLER_DESC samplerDesc = {};
                samplerDesc.Filter = ConvertFilter(desc);
                samplerDesc.AddressU = ConvertAddressMode(desc.addressU);
                samplerDesc.AddressV = ConvertAddressMode(desc.addressV);
                samplerDesc.AddressW = ConvertAddressMode(desc.addressW);
                samplerDesc.MipLODBias = desc.mipLodBias;
                samplerDesc.MaxAnisotropy = desc.maxAnisotropy;
                samplerDesc.ComparisonFunc = ConvertCompareOp(desc.compareOp);
                memcpy(samplerDesc.BorderColor, desc.borderColor, sizeof(float) * 4);
                samplerDesc.MinLOD = desc.minLod;
                samplerDesc.MaxLOD = desc.maxLod;

                m_device->CreateSampler(&samplerDesc, alloc.cpuHandle);
                return std::make_unique<D3D12Sampler>(desc, alloc);
            }

            std::unique_ptr<IRHIPipelineState> D3D12Device::CreatePipelineState(const RHIPipelineStateDesc& desc,
                                                                                IRHIShader* vertexShader,
                                                                                IRHIShader* pixelShader)
            {
                auto rootSig = CreateDefaultRootSignature();
                if (!rootSig)
                    return nullptr;

                auto* vs = static_cast<D3D12Shader*>(vertexShader);
                auto* ps = static_cast<D3D12Shader*>(pixelShader);

                // Build input layout
                std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
                for (const auto& elem : desc.inputLayout.elements)
                {
                    D3D12_INPUT_ELEMENT_DESC d3dElem = {};
                    d3dElem.SemanticName = elem.semanticName.c_str();
                    d3dElem.SemanticIndex = elem.semanticIndex;
                    d3dElem.Format = ConvertVertexFormat(elem.format);
                    d3dElem.InputSlot = elem.inputSlot;
                    d3dElem.AlignedByteOffset = elem.byteOffset;
                    d3dElem.InputSlotClass = elem.perInstance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                              : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                    d3dElem.InstanceDataStepRate = elem.instanceStepRate;
                    inputElements.push_back(d3dElem);
                }

                D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
                psoDesc.pRootSignature = rootSig.Get();

                if (vs)
                    psoDesc.VS = vs->GetD3D12Bytecode();
                if (ps)
                    psoDesc.PS = ps->GetD3D12Bytecode();

                psoDesc.InputLayout.pInputElementDescs = inputElements.data();
                psoDesc.InputLayout.NumElements = static_cast<UINT>(inputElements.size());

                // Rasterizer
                psoDesc.RasterizerState.FillMode = desc.rasterizer.fillMode == RHIFillMode::Wireframe
                                                       ? D3D12_FILL_MODE_WIREFRAME
                                                       : D3D12_FILL_MODE_SOLID;
                psoDesc.RasterizerState.CullMode = desc.rasterizer.cullMode == RHICullMode::None ? D3D12_CULL_MODE_NONE
                                                   : desc.rasterizer.cullMode == RHICullMode::Front
                                                       ? D3D12_CULL_MODE_FRONT
                                                       : D3D12_CULL_MODE_BACK;
                psoDesc.RasterizerState.FrontCounterClockwise = desc.rasterizer.frontCounterClockwise;
                psoDesc.RasterizerState.DepthBias = desc.rasterizer.depthBias;
                psoDesc.RasterizerState.DepthBiasClamp = desc.rasterizer.depthBiasClamp;
                psoDesc.RasterizerState.SlopeScaledDepthBias = desc.rasterizer.slopeScaledDepthBias;
                psoDesc.RasterizerState.DepthClipEnable = desc.rasterizer.depthClipEnable;
                psoDesc.RasterizerState.MultisampleEnable = desc.rasterizer.multisampleEnable;
                psoDesc.RasterizerState.AntialiasedLineEnable = desc.rasterizer.antialiasedLineEnable;

                // Depth stencil
                psoDesc.DepthStencilState.DepthEnable = desc.depthStencil.depthEnable;
                psoDesc.DepthStencilState.DepthWriteMask =
                    desc.depthStencil.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
                psoDesc.DepthStencilState.DepthFunc = ConvertCompareOp(desc.depthStencil.depthFunc);
                psoDesc.DepthStencilState.StencilEnable = desc.depthStencil.stencilEnable;
                psoDesc.DepthStencilState.StencilReadMask = desc.depthStencil.stencilReadMask;
                psoDesc.DepthStencilState.StencilWriteMask = desc.depthStencil.stencilWriteMask;

                // Front face stencil
                psoDesc.DepthStencilState.FrontFace.StencilFailOp =
                    ConvertStencilOp(desc.depthStencil.frontFace.stencilFail);
                psoDesc.DepthStencilState.FrontFace.StencilDepthFailOp =
                    ConvertStencilOp(desc.depthStencil.frontFace.stencilDepthFail);
                psoDesc.DepthStencilState.FrontFace.StencilPassOp =
                    ConvertStencilOp(desc.depthStencil.frontFace.stencilPass);
                psoDesc.DepthStencilState.FrontFace.StencilFunc =
                    ConvertCompareOp(desc.depthStencil.frontFace.stencilFunc);

                // Back face stencil
                psoDesc.DepthStencilState.BackFace.StencilFailOp =
                    ConvertStencilOp(desc.depthStencil.backFace.stencilFail);
                psoDesc.DepthStencilState.BackFace.StencilDepthFailOp =
                    ConvertStencilOp(desc.depthStencil.backFace.stencilDepthFail);
                psoDesc.DepthStencilState.BackFace.StencilPassOp =
                    ConvertStencilOp(desc.depthStencil.backFace.stencilPass);
                psoDesc.DepthStencilState.BackFace.StencilFunc =
                    ConvertCompareOp(desc.depthStencil.backFace.stencilFunc);

                // Blend state
                psoDesc.BlendState.AlphaToCoverageEnable = desc.blend.alphaToCoverageEnable;
                psoDesc.BlendState.IndependentBlendEnable = desc.blend.independentBlendEnable;
                for (int i = 0; i < 8; i++)
                {
                    auto& src = desc.blend.renderTargets[i];
                    auto& dst = psoDesc.BlendState.RenderTarget[i];
                    dst.BlendEnable = src.blendEnable;
                    dst.SrcBlend = ConvertBlendFactor(src.srcBlend);
                    dst.DestBlend = ConvertBlendFactor(src.dstBlend);
                    dst.BlendOp = ConvertBlendOp(src.blendOp);
                    dst.SrcBlendAlpha = ConvertBlendFactor(src.srcBlendAlpha);
                    dst.DestBlendAlpha = ConvertBlendFactor(src.dstBlendAlpha);
                    dst.BlendOpAlpha = ConvertBlendOp(src.blendOpAlpha);
                    dst.RenderTargetWriteMask = src.writeMask;
                }

                psoDesc.SampleMask = UINT_MAX;
                psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                psoDesc.NumRenderTargets = desc.numRenderTargets;
                for (uint32_t i = 0; i < desc.numRenderTargets; i++)
                    psoDesc.RTVFormats[i] = ConvertFormat(desc.renderTargetFormats[i]);
                psoDesc.DSVFormat = ConvertFormat(desc.depthStencilFormat);
                psoDesc.SampleDesc.Count = desc.sampleCount;

                ComPtr<ID3D12PipelineState> pso;
                HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
                if (FAILED(hr))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Failed to create PSO");
                    return nullptr;
                }

                return std::make_unique<D3D12PipelineState>(desc, std::move(pso), std::move(rootSig));
            }

            // ============================================================================
            // D3D12 DEVICE — DEFERRED GPU RESOURCE RELEASE
            // ============================================================================
            // D3D12 resources may still be referenced by in-flight command lists.
            // These helpers enqueue the underlying ID3D12Resource into the deferred
            // release queue so the COM ref is held until the GPU passes the fence.
            // The wrapper object (D3D12Buffer/D3D12Texture) is owned by the caller's
            // unique_ptr and destroyed immediately; only the GPU resource is deferred.

            void D3D12Device::DeferredReleaseBuffer(D3D12Buffer* buffer)
            {
                if (!buffer)
                    return;
                std::lock_guard<std::mutex> lock(m_deferredReleaseMutex);
                m_deferredReleaseQueue.push({buffer->GetD3D12Resource(), m_frameFence.GetCurrentValue()});
            }

            void D3D12Device::DeferredReleaseTexture(D3D12Texture* texture)
            {
                if (!texture)
                    return;
                m_cbvSrvUavHeap.Free(texture->GetSRVDescriptor());
                m_rtvHeap.Free(texture->GetRTVDescriptor());
                m_dsvHeap.Free(texture->GetDSVDescriptor());
                m_cbvSrvUavHeap.Free(texture->GetUAVDescriptor());
                std::lock_guard<std::mutex> lock(m_deferredReleaseMutex);
                m_deferredReleaseQueue.push({texture->GetD3D12Resource(), m_frameFence.GetCurrentValue()});
            }

            // ============================================================================
            // D3D12 DEVICE — RESOURCE UPDATES
            // ============================================================================

            void* D3D12Device::MapBuffer(IRHIBuffer* buffer)
            {
                auto* b = static_cast<D3D12Buffer*>(buffer);
                if (!b)
                    return nullptr;
                if (b->GetMappedPointer())
                    return b->GetMappedPointer();
                void* mapped = nullptr;
                HRESULT hr = b->GetD3D12Resource()->Map(0, nullptr, &mapped);
                if (FAILED(hr))
                    return nullptr;
                b->SetMappedPointer(mapped);
                return mapped;
            }

            void D3D12Device::UnmapBuffer(IRHIBuffer* buffer)
            {
                auto* b = static_cast<D3D12Buffer*>(buffer);
                if (b && b->GetMappedPointer())
                {
                    b->GetD3D12Resource()->Unmap(0, nullptr);
                    b->SetMappedPointer(nullptr);
                }
            }

            void D3D12Device::UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset)
            {
                auto* b = static_cast<D3D12Buffer*>(buffer);
                if (!b || !data)
                    return;
                void* mapped = MapBuffer(buffer);
                if (mapped)
                {
                    memcpy(static_cast<uint8_t*>(mapped) + offset, data, size);
                    if (b->GetDesc().access == RHIBufferAccess::Static)
                        UnmapBuffer(buffer);
                }
            }

            void D3D12Device::UpdateTexture(IRHITexture* texture, const void* data, uint32_t mipLevel,
                                            uint32_t /*arraySlice*/)
            {
                auto* t = static_cast<D3D12Texture*>(texture);
                if (!t || !data)
                    return;

                uint32_t mipWidth = std::max(1u, t->GetWidth() >> mipLevel);
                uint32_t mipHeight = std::max(1u, t->GetHeight() >> mipLevel);
                uint32_t pixelSize = GetFormatSize(t->GetFormat());
                uint32_t rowPitch = (mipWidth * pixelSize + 255) & ~255u; // D3D12 alignment
                uint64_t uploadSize = static_cast<uint64_t>(rowPitch) * mipHeight;

                // Create upload buffer
                D3D12_HEAP_PROPERTIES uploadHeap = {};
                uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

                D3D12_RESOURCE_DESC bufDesc = {};
                bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bufDesc.Width = uploadSize;
                bufDesc.Height = 1;
                bufDesc.DepthOrArraySize = 1;
                bufDesc.MipLevels = 1;
                bufDesc.SampleDesc.Count = 1;
                bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                ComPtr<ID3D12Resource> uploadBuffer;
                HRESULT hr = m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                               IID_PPV_ARGS(&uploadBuffer));
                if (FAILED(hr))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D12: Failed to create texture upload buffer");
                    return;
                }

                // Copy source data into upload buffer row-by-row (respecting alignment)
                uint8_t* mapped = nullptr;
                hr = uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
                if (FAILED(hr))
                    return;
                uint32_t srcRowPitch = mipWidth * pixelSize;
                for (uint32_t row = 0; row < mipHeight; row++)
                {
                    memcpy(mapped + row * rowPitch, static_cast<const uint8_t*>(data) + row * srcRowPitch, srcRowPitch);
                }
                uploadBuffer->Unmap(0, nullptr);

                // Record copy command
                m_immediateCommandList->Begin();

                // Transition to copy dest
                m_immediateCommandList->TransitionBarrier(t, t->GetCurrentState(), D3D12_RESOURCE_STATE_COPY_DEST);
                m_immediateCommandList->FlushBarriers();

                D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
                srcLoc.pResource = uploadBuffer.Get();
                srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                srcLoc.PlacedFootprint.Offset = 0;
                srcLoc.PlacedFootprint.Footprint.Format = ConvertFormat(t->GetFormat());
                srcLoc.PlacedFootprint.Footprint.Width = mipWidth;
                srcLoc.PlacedFootprint.Footprint.Height = mipHeight;
                srcLoc.PlacedFootprint.Footprint.Depth = 1;
                srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

                D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
                dstLoc.pResource = t->GetD3D12Resource();
                dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dstLoc.SubresourceIndex = mipLevel;

                m_immediateCommandList->GetCommandList()->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

                // Transition back to shader resource
                m_immediateCommandList->TransitionBarrier(t, D3D12_RESOURCE_STATE_COPY_DEST,
                                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                m_immediateCommandList->End();

                // Execute with fence — only waits for this upload, not all GPU work
                ID3D12CommandList* lists[] = {m_immediateCommandList->GetCommandList()};
                m_directQueue->ExecuteCommandLists(1, lists);
                uint64_t uploadFenceVal = m_frameFence.Signal(m_directQueue.Get());
                m_frameFence.WaitForValue(uploadFenceVal);

                // Queue upload buffer for deferred release at this fence value
                {
                    std::lock_guard<std::mutex> lock(m_deferredReleaseMutex);
                    m_deferredReleaseQueue.push({uploadBuffer, uploadFenceVal});
                }
            }

            // ============================================================================
            // D3D12 DEVICE — COMMAND LISTS
            // ============================================================================

            IRHICommandList* D3D12Device::GetImmediateCommandList()
            {
                return m_immediateCommandList.get();
            }

            std::unique_ptr<IRHICommandList> D3D12Device::CreateDeferredCommandList()
            {
                return std::make_unique<D3D12CommandList>(m_device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
            }

            void D3D12Device::ExecuteCommandList(IRHICommandList* commandList)
            {
                auto* cmdList = static_cast<D3D12CommandList*>(commandList);
                if (!cmdList)
                    return;
                std::lock_guard<std::mutex> lock(m_submitMutex);
                ID3D12CommandList* lists[] = {cmdList->GetCommandList()};
                m_directQueue->ExecuteCommandLists(1, lists);
                m_statistics.drawCalls++;
            }

            // ============================================================================
            // D3D12 DEVICE — FRAME MANAGEMENT
            // ============================================================================

            void D3D12Device::BeginFrame()
            {
                auto& frame = m_frameResources[m_currentFrameIndex];
                m_frameFence.WaitForValue(frame.fenceValue);
                frame.commandAllocator->Reset();
                ProcessDeferredReleases();
                m_statistics = {};
            }

            void D3D12Device::EndFrame()
            {
                MoveToNextFrame();
            }

            void D3D12Device::WaitForIdle()
            {
                uint64_t val = m_frameFence.Signal(m_directQueue.Get());
                m_frameFence.WaitForValue(val);
            }

            void D3D12Device::MoveToNextFrame()
            {
                auto& frame = m_frameResources[m_currentFrameIndex];
                frame.fenceValue = m_frameFence.Signal(m_directQueue.Get());
                m_currentFrameIndex = (m_currentFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
            }

            void D3D12Device::ProcessDeferredReleases()
            {
                std::lock_guard<std::mutex> lock(m_deferredReleaseMutex);
                uint64_t completed = m_frameFence.GetCompletedValue();
                while (!m_deferredReleaseQueue.empty())
                {
                    auto& front = m_deferredReleaseQueue.front();
                    if (front.fenceValue > completed)
                        break;
                    m_deferredReleaseQueue.pop();
                }
            }

            void D3D12Device::ResetStatistics()
            {
                m_statistics = {};
            }

            std::string D3D12Device::GetDeviceInfo() const
            {
                std::ostringstream ss;
                ss << "=== D3D12 Device ===\n";
                ss << "GPU: " << m_capabilities.deviceName << "\n";
                ss << "Vendor: " << m_capabilities.vendorName << "\n";
                if (m_isSoftwareDevice)
                    ss << "Type: Software (WARP)\n";
                ss << "VRAM: " << (m_capabilities.dedicatedVideoMemory / (1024 * 1024)) << " MB\n";
                ss << "DXR: " << (m_dxrSupported ? "Yes" : "No") << "\n";
                ss << "Mesh Shaders: " << (m_capabilities.meshShaderSupport ? "Yes" : "No") << "\n";
                ss << "Bindless: " << (m_capabilities.bindlessResourceSupport ? "Yes" : "No") << "\n";
                ss << "Enhanced Barriers: " << (m_capabilities.enhancedBarrierSupport ? "Yes" : "No") << "\n";
                ss << "GPU Upload Heaps: " << (m_capabilities.hostImageCopySupport ? "Yes" : "No") << "\n";
                return ss.str();
            }

            // ============================================================================
            // D3D12 DEVICE — ROOT SIGNATURES
            // ============================================================================

            ComPtr<ID3D12RootSignature> D3D12Device::CreateDefaultRootSignature() const
            {
                D3D12_DESCRIPTOR_RANGE1 cbvRange = {};
                cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                cbvRange.NumDescriptors = 14;
                cbvRange.BaseShaderRegister = 0;

                D3D12_DESCRIPTOR_RANGE1 srvRange = {};
                srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                srvRange.NumDescriptors = 32;
                srvRange.BaseShaderRegister = 0;

                D3D12_DESCRIPTOR_RANGE1 samplerRange = {};
                samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                samplerRange.NumDescriptors = 16;
                samplerRange.BaseShaderRegister = 0;

                D3D12_DESCRIPTOR_RANGE1 uavRange = {};
                uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                uavRange.NumDescriptors = 8;
                uavRange.BaseShaderRegister = 0;

                D3D12_ROOT_PARAMETER1 params[4] = {};
                params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[0].DescriptorTable.NumDescriptorRanges = 1;
                params[0].DescriptorTable.pDescriptorRanges = &cbvRange;
                params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

                params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[1].DescriptorTable.NumDescriptorRanges = 1;
                params[1].DescriptorTable.pDescriptorRanges = &srvRange;
                params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[2].DescriptorTable.NumDescriptorRanges = 1;
                params[2].DescriptorTable.pDescriptorRanges = &samplerRange;
                params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

                params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[3].DescriptorTable.NumDescriptorRanges = 1;
                params[3].DescriptorTable.pDescriptorRanges = &uavRange;
                params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

                D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
                desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
                desc.Desc_1_1.NumParameters = 4;
                desc.Desc_1_1.pParameters = params;
                desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

                ComPtr<ID3DBlob> signature;
                ComPtr<ID3DBlob> error;
                HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &signature, &error);
                if (FAILED(hr) || !signature)
                    return nullptr;

                ComPtr<ID3D12RootSignature> rootSig;
                hr = m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                   IID_PPV_ARGS(&rootSig));
                if (FAILED(hr))
                    return nullptr;
                return rootSig;
            }

            ComPtr<ID3D12RootSignature> D3D12Device::CreateRootSignature(const void* data, size_t dataSize) const
            {
                ComPtr<ID3D12RootSignature> rootSig;
                HRESULT hr = m_device->CreateRootSignature(0, data, dataSize, IID_PPV_ARGS(&rootSig));
                if (FAILED(hr))
                    return nullptr;
                return rootSig;
            }

            // ============================================================================
            // D3D12 DEVICE — FORMAT CONVERSION HELPERS
            // ============================================================================

            DXGI_FORMAT D3D12Device::ConvertFormat(PixelFormat format) const
            {
                switch (format)
                {
                case PixelFormat::R8_UNORM:
                    return DXGI_FORMAT_R8_UNORM;
                case PixelFormat::R8_SNORM:
                    return DXGI_FORMAT_R8_SNORM;
                case PixelFormat::R8_UINT:
                    return DXGI_FORMAT_R8_UINT;
                case PixelFormat::R8G8_UNORM:
                    return DXGI_FORMAT_R8G8_UNORM;
                case PixelFormat::R8G8B8A8_UNORM:
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                case PixelFormat::R8G8B8A8_UNORM_SRGB:
                    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                case PixelFormat::R8G8B8A8_SNORM:
                    return DXGI_FORMAT_R8G8B8A8_SNORM;
                case PixelFormat::B8G8R8A8_UNORM:
                    return DXGI_FORMAT_B8G8R8A8_UNORM;
                case PixelFormat::B8G8R8A8_UNORM_SRGB:
                    return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                case PixelFormat::R10G10B10A2_UNORM:
                    return DXGI_FORMAT_R10G10B10A2_UNORM;
                case PixelFormat::R11G11B10_FLOAT:
                    return DXGI_FORMAT_R11G11B10_FLOAT;
                case PixelFormat::R16_FLOAT:
                    return DXGI_FORMAT_R16_FLOAT;
                case PixelFormat::R16_UINT:
                    return DXGI_FORMAT_R16_UINT;
                case PixelFormat::R16G16_FLOAT:
                    return DXGI_FORMAT_R16G16_FLOAT;
                case PixelFormat::R16G16B16A16_FLOAT:
                    return DXGI_FORMAT_R16G16B16A16_FLOAT;
                case PixelFormat::R16G16B16A16_UNORM:
                    return DXGI_FORMAT_R16G16B16A16_UNORM;
                case PixelFormat::R32_FLOAT:
                    return DXGI_FORMAT_R32_FLOAT;
                case PixelFormat::R32_UINT:
                    return DXGI_FORMAT_R32_UINT;
                case PixelFormat::R32G32_FLOAT:
                    return DXGI_FORMAT_R32G32_FLOAT;
                case PixelFormat::R32G32B32_FLOAT:
                    return DXGI_FORMAT_R32G32B32_FLOAT;
                case PixelFormat::R32G32B32A32_FLOAT:
                    return DXGI_FORMAT_R32G32B32A32_FLOAT;
                case PixelFormat::BC1_UNORM:
                    return DXGI_FORMAT_BC1_UNORM;
                case PixelFormat::BC1_UNORM_SRGB:
                    return DXGI_FORMAT_BC1_UNORM_SRGB;
                case PixelFormat::BC2_UNORM:
                    return DXGI_FORMAT_BC2_UNORM;
                case PixelFormat::BC3_UNORM:
                    return DXGI_FORMAT_BC3_UNORM;
                case PixelFormat::BC3_UNORM_SRGB:
                    return DXGI_FORMAT_BC3_UNORM_SRGB;
                case PixelFormat::BC4_UNORM:
                    return DXGI_FORMAT_BC4_UNORM;
                case PixelFormat::BC5_UNORM:
                    return DXGI_FORMAT_BC5_UNORM;
                case PixelFormat::BC6H_UF16:
                    return DXGI_FORMAT_BC6H_UF16;
                case PixelFormat::BC7_UNORM:
                    return DXGI_FORMAT_BC7_UNORM;
                case PixelFormat::BC7_UNORM_SRGB:
                    return DXGI_FORMAT_BC7_UNORM_SRGB;
                case PixelFormat::D16_UNORM:
                    return DXGI_FORMAT_D16_UNORM;
                case PixelFormat::D24_UNORM_S8_UINT:
                    return DXGI_FORMAT_D24_UNORM_S8_UINT;
                case PixelFormat::D32_FLOAT:
                    return DXGI_FORMAT_D32_FLOAT;
                case PixelFormat::D32_FLOAT_S8_UINT:
                    return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
                default:
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                }
            }

            D3D12_FILTER D3D12Device::ConvertFilter(const RHISamplerDesc& desc) const
            {
                if (desc.minFilter == RHIFilterMode::Anisotropic)
                    return D3D12_FILTER_ANISOTROPIC;
                bool minLinear = (desc.minFilter == RHIFilterMode::Linear);
                bool magLinear = (desc.magFilter == RHIFilterMode::Linear);
                bool mipLinear = (desc.mipFilter == RHIFilterMode::Linear);
                if (minLinear && magLinear && mipLinear)
                    return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                if (minLinear && magLinear)
                    return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                if (minLinear)
                    return D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
                return D3D12_FILTER_MIN_MAG_MIP_POINT;
            }

            D3D12_TEXTURE_ADDRESS_MODE D3D12Device::ConvertAddressMode(RHIAddressMode mode) const
            {
                switch (mode)
                {
                case RHIAddressMode::Wrap:
                    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                case RHIAddressMode::Clamp:
                    return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                case RHIAddressMode::Mirror:
                    return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
                case RHIAddressMode::Border:
                    return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
                case RHIAddressMode::MirrorOnce:
                    return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
                default:
                    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                }
            }

            D3D12_COMPARISON_FUNC D3D12Device::ConvertCompareOp(RHICompareOp op) const
            {
                switch (op)
                {
                case RHICompareOp::Never:
                    return D3D12_COMPARISON_FUNC_NEVER;
                case RHICompareOp::Less:
                    return D3D12_COMPARISON_FUNC_LESS;
                case RHICompareOp::Equal:
                    return D3D12_COMPARISON_FUNC_EQUAL;
                case RHICompareOp::LessEqual:
                    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
                case RHICompareOp::Greater:
                    return D3D12_COMPARISON_FUNC_GREATER;
                case RHICompareOp::NotEqual:
                    return D3D12_COMPARISON_FUNC_NOT_EQUAL;
                case RHICompareOp::GreaterEqual:
                    return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
                case RHICompareOp::Always:
                    return D3D12_COMPARISON_FUNC_ALWAYS;
                default:
                    return D3D12_COMPARISON_FUNC_LESS;
                }
            }

            D3D12_STENCIL_OP D3D12Device::ConvertStencilOp(RHIStencilOp op) const
            {
                switch (op)
                {
                case RHIStencilOp::Keep:
                    return D3D12_STENCIL_OP_KEEP;
                case RHIStencilOp::Zero:
                    return D3D12_STENCIL_OP_ZERO;
                case RHIStencilOp::Replace:
                    return D3D12_STENCIL_OP_REPLACE;
                case RHIStencilOp::IncrSat:
                    return D3D12_STENCIL_OP_INCR_SAT;
                case RHIStencilOp::DecrSat:
                    return D3D12_STENCIL_OP_DECR_SAT;
                case RHIStencilOp::Invert:
                    return D3D12_STENCIL_OP_INVERT;
                case RHIStencilOp::IncrWrap:
                    return D3D12_STENCIL_OP_INCR;
                case RHIStencilOp::DecrWrap:
                    return D3D12_STENCIL_OP_DECR;
                default:
                    return D3D12_STENCIL_OP_KEEP;
                }
            }

            D3D12_BLEND D3D12Device::ConvertBlendFactor(RHIBlendFactor factor) const
            {
                switch (factor)
                {
                case RHIBlendFactor::Zero:
                    return D3D12_BLEND_ZERO;
                case RHIBlendFactor::One:
                    return D3D12_BLEND_ONE;
                case RHIBlendFactor::SrcColor:
                    return D3D12_BLEND_SRC_COLOR;
                case RHIBlendFactor::InvSrcColor:
                    return D3D12_BLEND_INV_SRC_COLOR;
                case RHIBlendFactor::SrcAlpha:
                    return D3D12_BLEND_SRC_ALPHA;
                case RHIBlendFactor::InvSrcAlpha:
                    return D3D12_BLEND_INV_SRC_ALPHA;
                case RHIBlendFactor::DstAlpha:
                    return D3D12_BLEND_DEST_ALPHA;
                case RHIBlendFactor::InvDstAlpha:
                    return D3D12_BLEND_INV_DEST_ALPHA;
                case RHIBlendFactor::DstColor:
                    return D3D12_BLEND_DEST_COLOR;
                case RHIBlendFactor::InvDstColor:
                    return D3D12_BLEND_INV_DEST_COLOR;
                default:
                    return D3D12_BLEND_ONE;
                }
            }

            D3D12_BLEND_OP D3D12Device::ConvertBlendOp(RHIBlendOp op) const
            {
                switch (op)
                {
                case RHIBlendOp::Add:
                    return D3D12_BLEND_OP_ADD;
                case RHIBlendOp::Subtract:
                    return D3D12_BLEND_OP_SUBTRACT;
                case RHIBlendOp::RevSubtract:
                    return D3D12_BLEND_OP_REV_SUBTRACT;
                case RHIBlendOp::Min:
                    return D3D12_BLEND_OP_MIN;
                case RHIBlendOp::Max:
                    return D3D12_BLEND_OP_MAX;
                default:
                    return D3D12_BLEND_OP_ADD;
                }
            }

            DXGI_FORMAT D3D12Device::ConvertVertexFormat(RHIVertexFormat format) const
            {
                switch (format)
                {
                case RHIVertexFormat::Float1:
                    return DXGI_FORMAT_R32_FLOAT;
                case RHIVertexFormat::Float2:
                    return DXGI_FORMAT_R32G32_FLOAT;
                case RHIVertexFormat::Float3:
                    return DXGI_FORMAT_R32G32B32_FLOAT;
                case RHIVertexFormat::Float4:
                    return DXGI_FORMAT_R32G32B32A32_FLOAT;
                case RHIVertexFormat::Int1:
                    return DXGI_FORMAT_R32_SINT;
                case RHIVertexFormat::Int2:
                    return DXGI_FORMAT_R32G32_SINT;
                case RHIVertexFormat::Int3:
                    return DXGI_FORMAT_R32G32B32_SINT;
                case RHIVertexFormat::Int4:
                    return DXGI_FORMAT_R32G32B32A32_SINT;
                case RHIVertexFormat::UInt1:
                    return DXGI_FORMAT_R32_UINT;
                case RHIVertexFormat::UInt2:
                    return DXGI_FORMAT_R32G32_UINT;
                case RHIVertexFormat::UInt3:
                    return DXGI_FORMAT_R32G32B32_UINT;
                case RHIVertexFormat::UInt4:
                    return DXGI_FORMAT_R32G32B32A32_UINT;
                case RHIVertexFormat::UNorm8x4:
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                case RHIVertexFormat::SNorm8x4:
                    return DXGI_FORMAT_R8G8B8A8_SNORM;
                default:
                    return DXGI_FORMAT_R32G32B32_FLOAT;
                }
            }

            D3D12_RESOURCE_STATES D3D12Device::GetInitialResourceState(RHIBufferAccess access) const
            {
                switch (access)
                {
                case RHIBufferAccess::Static:
                    return D3D12_RESOURCE_STATE_COMMON;
                case RHIBufferAccess::Dynamic:
                    return D3D12_RESOURCE_STATE_GENERIC_READ;
                case RHIBufferAccess::Staging:
                    return D3D12_RESOURCE_STATE_GENERIC_READ;
                case RHIBufferAccess::ReadBack:
                    return D3D12_RESOURCE_STATE_COPY_DEST;
                default:
                    return D3D12_RESOURCE_STATE_COMMON;
                }
            }

        } // namespace D3D12
    } // namespace RHI
} // namespace Spark

// Clean up local compatibility macros
#undef LOG_ERROR
#undef LOG_INFO

#endif // _WIN32
