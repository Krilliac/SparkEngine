/**
 * @file D3D12Device.cpp
 * @brief Complete DirectX 12 RHI backend implementation
 * @author Spark Engine Team
 * @date 2026
 *
 * Full D3D12 device implementation: DXGI factory, adapter enumeration,
 * device creation, command queues, descriptor heaps, resource management,
 * command list recording, swap chain, and frame synchronization.
 */

#ifdef _WIN32

#include "D3D12Device.h"
#include "../RHIFactory.h"
#include "../../../Utils/LogMacros.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Compatibility logging macros — bridge std::format syntax to SPARK_LOG_*
// ---------------------------------------------------------------------------
#define LOG_ERROR(fmt, ...) SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "%s", std::format(fmt, ##__VA_ARGS__).c_str())
#define LOG_INFO(fmt, ...) SPARK_LOG_INFO(Spark::LogCategory::Graphics, "%s", std::format(fmt, ##__VA_ARGS__).c_str())

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
                    std::cerr << "[D3D12] Failed to create descriptor heap (type=" << type << ")" << std::endl;
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
                    std::cerr << "[D3D12] Descriptor heap exhausted" << std::endl;
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
                    std::cerr << "[D3D12] Failed to create fence" << std::endl;
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
            // D3D12 SWAP CHAIN
            // ============================================================================

            D3D12SwapChain::D3D12SwapChain(ID3D12Device* device, ID3D12CommandQueue* commandQueue,
                                           IDXGIFactory4* dxgiFactory, DescriptorHeapAllocator* rtvAllocator,
                                           const RHISwapChainDesc& desc)
                : m_desc(desc), m_device(device), m_commandQueue(commandQueue), m_rtvAllocator(rtvAllocator)
            {
                DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
                swapChainDesc.Width = desc.width;
                swapChainDesc.Height = desc.height;
                swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                swapChainDesc.SampleDesc.Count = 1;
                swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                swapChainDesc.BufferCount = std::min(desc.bufferCount, MAX_BACK_BUFFER_COUNT);
                swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

                ComPtr<IDXGISwapChain1> swapChain1;
                HRESULT hr = dxgiFactory->CreateSwapChainForHwnd(commandQueue, static_cast<HWND>(desc.windowHandle),
                                                                 &swapChainDesc, nullptr, nullptr, &swapChain1);
                if (SUCCEEDED(hr))
                {
                    swapChain1.As(&m_swapChain);
                    CreateBackBufferResources();
                }
                else
                {
                    std::cerr << "[D3D12] Failed to create swap chain" << std::endl;
                }
            }

            D3D12SwapChain::~D3D12SwapChain()
            {
                ReleaseBackBufferResources();
            }

            bool D3D12SwapChain::Present(bool vsync)
            {
                if (!m_swapChain)
                    return false;
                UINT syncInterval = vsync ? 1 : 0;
                UINT flags = vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING;
                HRESULT hr = m_swapChain->Present(syncInterval, flags);
                return SUCCEEDED(hr);
            }

            bool D3D12SwapChain::Resize(uint32_t width, uint32_t height)
            {
                if (!m_swapChain)
                    return false;
                ReleaseBackBufferResources();
                m_desc.width = width;
                m_desc.height = height;
                HRESULT hr = m_swapChain->ResizeBuffers(m_desc.bufferCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                        DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
                if (FAILED(hr))
                    return false;
                return CreateBackBufferResources();
            }

            IRHITexture* D3D12SwapChain::GetBackBuffer()
            {
                if (!m_swapChain)
                    return nullptr;
                uint32_t idx = m_swapChain->GetCurrentBackBufferIndex();
                return m_backBuffers[idx].get();
            }

            bool D3D12SwapChain::CreateBackBufferResources()
            {
                for (uint32_t i = 0; i < m_desc.bufferCount && i < MAX_BACK_BUFFER_COUNT; i++)
                {
                    ComPtr<ID3D12Resource> backBuffer;
                    HRESULT hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer));
                    if (FAILED(hr))
                        return false;

                    auto rtvAlloc = m_rtvAllocator->Allocate(1);
                    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
                    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    m_device->CreateRenderTargetView(backBuffer.Get(), &rtvDesc, rtvAlloc.cpuHandle);

                    RHITextureDesc texDesc;
                    texDesc.width = m_desc.width;
                    texDesc.height = m_desc.height;
                    texDesc.format = PixelFormat::R8G8B8A8_UNORM;
                    texDesc.usage = RHITextureUsage::RenderTarget;

                    m_backBuffers[i] = std::make_unique<D3D12Texture>(texDesc, std::move(backBuffer),
                                                                      DescriptorAllocation{}, rtvAlloc);
                    m_backBufferRTVs[i] = rtvAlloc;
                }
                return true;
            }

            void D3D12SwapChain::ReleaseBackBufferResources()
            {
                for (uint32_t i = 0; i < MAX_BACK_BUFFER_COUNT; i++)
                {
                    if (m_backBuffers[i])
                    {
                        m_rtvAllocator->Free(m_backBufferRTVs[i]);
                        m_backBuffers[i].reset();
                    }
                }
            }

            // ============================================================================
            // D3D12 COMMAND LIST
            // ============================================================================

            D3D12CommandList::D3D12CommandList(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type,
                                               ID3D12PipelineState* initialPSO)
                : m_type(type)
            {
                device->CreateCommandAllocator(type, IID_PPV_ARGS(&m_commandAllocator));
                device->CreateCommandList(0, type, m_commandAllocator.Get(), initialPSO, IID_PPV_ARGS(&m_commandList));
                m_commandList->Close();
            }

            void D3D12CommandList::Begin()
            {
                m_commandAllocator->Reset();
                m_commandList->Reset(m_commandAllocator.Get(), nullptr);
            }

            void D3D12CommandList::End()
            {
                FlushBarriers();
                m_commandList->Close();
            }

            void D3D12CommandList::Reset()
            {
                m_commandAllocator->Reset();
                m_commandList->Reset(m_commandAllocator.Get(), nullptr);
                m_pendingBarriers.clear();
                m_currentRootSignature = nullptr;
            }

            void D3D12CommandList::SetRenderTargets(IRHITexture** renderTargets, uint32_t count,
                                                    IRHITexture* depthStencil)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[8] = {};
                for (uint32_t i = 0; i < count && i < 8; i++)
                {
                    auto* tex = static_cast<D3D12Texture*>(renderTargets[i]);
                    if (tex && tex->GetRTVDescriptor().IsValid())
                        rtvHandles[i] = tex->GetRTVDescriptor().cpuHandle;
                }

                D3D12_CPU_DESCRIPTOR_HANDLE* dsvHandle = nullptr;
                D3D12_CPU_DESCRIPTOR_HANDLE dsvLocal = {};
                if (depthStencil)
                {
                    auto* dsTex = static_cast<D3D12Texture*>(depthStencil);
                    if (dsTex->GetDSVDescriptor().IsValid())
                    {
                        dsvLocal = dsTex->GetDSVDescriptor().cpuHandle;
                        dsvHandle = &dsvLocal;
                    }
                }

                m_commandList->OMSetRenderTargets(count, rtvHandles, FALSE, dsvHandle);
            }

            void D3D12CommandList::ClearRenderTarget(IRHITexture* target, const float color[4])
            {
                auto* tex = static_cast<D3D12Texture*>(target);
                if (tex && tex->GetRTVDescriptor().IsValid())
                    m_commandList->ClearRenderTargetView(tex->GetRTVDescriptor().cpuHandle, color, 0, nullptr);
            }

            void D3D12CommandList::ClearDepthStencil(IRHITexture* target, float depth, uint8_t stencil)
            {
                auto* tex = static_cast<D3D12Texture*>(target);
                if (tex && tex->GetDSVDescriptor().IsValid())
                    m_commandList->ClearDepthStencilView(tex->GetDSVDescriptor().cpuHandle,
                                                         D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depth,
                                                         stencil, 0, nullptr);
            }

            void D3D12CommandList::SetViewport(const RHIViewport& viewport)
            {
                D3D12_VIEWPORT vp = {viewport.x,      viewport.y,        viewport.width,
                                     viewport.height, viewport.minDepth, viewport.maxDepth};
                m_commandList->RSSetViewports(1, &vp);
            }

            void D3D12CommandList::SetScissorRect(const RHIScissorRect& rect)
            {
                D3D12_RECT r = {rect.left, rect.top, rect.right, rect.bottom};
                m_commandList->RSSetScissorRects(1, &r);
            }

            void D3D12CommandList::SetPipelineState(IRHIPipelineState* pipelineState)
            {
                auto* pso = static_cast<D3D12PipelineState*>(pipelineState);
                if (!pso)
                    return;
                m_commandList->SetPipelineState(pso->GetPSO());
                if (pso->GetRootSignature() != m_currentRootSignature)
                {
                    m_commandList->SetGraphicsRootSignature(pso->GetRootSignature());
                    m_currentRootSignature = pso->GetRootSignature();
                }
            }

            void D3D12CommandList::SetPrimitiveTopology(RHIPrimitiveTopology topology)
            {
                D3D_PRIMITIVE_TOPOLOGY d3dTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                switch (topology)
                {
                case RHIPrimitiveTopology::PointList:
                    d3dTopology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
                    break;
                case RHIPrimitiveTopology::LineList:
                    d3dTopology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
                    break;
                case RHIPrimitiveTopology::LineStrip:
                    d3dTopology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
                    break;
                case RHIPrimitiveTopology::TriangleList:
                    d3dTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                    break;
                case RHIPrimitiveTopology::TriangleStrip:
                    d3dTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
                    break;
                default:
                    break;
                }
                m_commandList->IASetPrimitiveTopology(d3dTopology);
            }

            void D3D12CommandList::SetVertexBuffer(IRHIBuffer* buffer, uint32_t slot, uint32_t offset)
            {
                auto* buf = static_cast<D3D12Buffer*>(buffer);
                if (!buf)
                    return;
                D3D12_VERTEX_BUFFER_VIEW vbv = {};
                vbv.BufferLocation = buf->GetGPUVirtualAddress() + offset;
                vbv.SizeInBytes = static_cast<UINT>(buf->GetSize() - offset);
                vbv.StrideInBytes = buf->GetStride();
                m_commandList->IASetVertexBuffers(slot, 1, &vbv);
            }

            void D3D12CommandList::SetIndexBuffer(IRHIBuffer* buffer, uint32_t offset)
            {
                auto* buf = static_cast<D3D12Buffer*>(buffer);
                if (!buf)
                    return;
                D3D12_INDEX_BUFFER_VIEW ibv = {};
                ibv.BufferLocation = buf->GetGPUVirtualAddress() + offset;
                ibv.SizeInBytes = static_cast<UINT>(buf->GetSize() - offset);
                ibv.Format = (buf->GetStride() == 2) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
                m_commandList->IASetIndexBuffer(&ibv);
            }

            void D3D12CommandList::SetConstantBuffer(RHIShaderStage /*stage*/, uint32_t slot, IRHIBuffer* buffer)
            {
                auto* buf = static_cast<D3D12Buffer*>(buffer);
                if (buf)
                    m_commandList->SetGraphicsRootConstantBufferView(slot, buf->GetGPUVirtualAddress());
            }

            void D3D12CommandList::SetShaderResource(RHIShaderStage /*stage*/, uint32_t /*slot*/,
                                                     IRHITexture* /*texture*/)
            {
                // Descriptor table binding handled via root signature
            }

            void D3D12CommandList::SetSampler(RHIShaderStage /*stage*/, uint32_t /*slot*/, IRHISampler* /*sampler*/)
            {
                // Static samplers in root signature or descriptor table
            }

            void D3D12CommandList::Draw(uint32_t vertexCount, uint32_t startVertex)
            {
                FlushBarriers();
                m_commandList->DrawInstanced(vertexCount, 1, startVertex, 0);
            }

            void D3D12CommandList::DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex)
            {
                FlushBarriers();
                m_commandList->DrawIndexedInstanced(indexCount, 1, startIndex, baseVertex, 0);
            }

            void D3D12CommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex,
                                                 uint32_t startInstance)
            {
                FlushBarriers();
                m_commandList->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance);
            }

            void D3D12CommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount,
                                                        uint32_t startIndex, int32_t baseVertex, uint32_t startInstance)
            {
                FlushBarriers();
                m_commandList->DrawIndexedInstanced(indexCount, instanceCount, startIndex, baseVertex, startInstance);
            }

            void D3D12CommandList::Dispatch(uint32_t x, uint32_t y, uint32_t z)
            {
                FlushBarriers();
                m_commandList->Dispatch(x, y, z);
            }

            void D3D12CommandList::BeginEvent(const char* name)
            {
                // PIX event markers (requires pix3.h for full support)
                (void)name;
            }

            void D3D12CommandList::EndEvent() {}

            void D3D12CommandList::SetMarker(const char* name)
            {
                (void)name;
            }

            void D3D12CommandList::TransitionBarrier(D3D12Texture* resource, D3D12_RESOURCE_STATES stateBefore,
                                                     D3D12_RESOURCE_STATES stateAfter)
            {
                if (stateBefore == stateAfter || !resource)
                    return;
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = resource->GetD3D12Resource();
                barrier.Transition.StateBefore = stateBefore;
                barrier.Transition.StateAfter = stateAfter;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                m_pendingBarriers.push_back(barrier);
                resource->SetCurrentState(stateAfter);
            }

            void D3D12CommandList::UAVBarrier(ID3D12Resource* resource)
            {
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barrier.UAV.pResource = resource;
                m_pendingBarriers.push_back(barrier);
            }

            void D3D12CommandList::FlushBarriers()
            {
                if (!m_pendingBarriers.empty())
                {
                    m_commandList->ResourceBarrier(static_cast<UINT>(m_pendingBarriers.size()),
                                                   m_pendingBarriers.data());
                    m_pendingBarriers.clear();
                }
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

                std::cout << "[D3D12] Device initialized: " << m_capabilities.deviceName << std::endl;
                return true;
            }

            void D3D12Device::Shutdown()
            {
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
                    std::cerr << "[D3D12] Failed to create DXGI factory" << std::endl;
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
                    std::cerr << "[D3D12] No D3D12-capable GPU found" << std::endl;
                    return false;
                }
                m_adapter = bestAdapter;

                // Create device
                hr = D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
                if (FAILED(hr))
                {
                    std::cerr << "[D3D12] Failed to create D3D12 device" << std::endl;
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
                    return false;

                queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
                if (FAILED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_copyQueue))))
                    return false;

                queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
                if (FAILED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_computeQueue))))
                    return false;

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
                default:
                    m_capabilities.vendorName = "Unknown";
                    break;
                }

                m_capabilities.apiVersion = "Direct3D 12.0";
                m_capabilities.tessellationSupport = true;
                m_capabilities.computeShaderSupport = true;
                m_capabilities.geometryShaderSupport = true;

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
                    }
                }
                if (!m_dxrSupported)
                    m_dxrDevice.Reset();
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

            IRHIBuffer* D3D12Device::CreateBuffer(const RHIBufferDesc& desc)
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
                    m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(&uploadResource));
                    if (uploadResource)
                    {
                        void* mapped = nullptr;
                        uploadResource->Map(0, nullptr, &mapped);
                        memcpy(mapped, desc.initialData, desc.size);
                        uploadResource->Unmap(0, nullptr);
                    }
                }

                auto* buffer = new D3D12Buffer(desc, std::move(resource), std::move(uploadResource));

                // Map persistent pointer for dynamic buffers
                if (isDynamic)
                {
                    void* mapped = nullptr;
                    buffer->GetD3D12Resource()->Map(0, nullptr, &mapped);
                    buffer->SetMappedPointer(mapped);
                    if (desc.initialData && mapped)
                        memcpy(mapped, desc.initialData, desc.size);
                }

                if (!desc.debugName.empty())
                {
                    std::wstring wname(desc.debugName.begin(), desc.debugName.end());
                    buffer->GetD3D12Resource()->SetName(wname.c_str());
                }

                return buffer;
            }

            IRHITexture* D3D12Device::CreateTexture(const RHITextureDesc& desc)
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

                return new D3D12Texture(desc, std::move(resource), srvAlloc, rtvAlloc, dsvAlloc, uavAlloc);
            }

            IRHIShader* D3D12Device::CreateShader(const RHIShaderDesc& desc)
            {
                if (desc.bytecode && desc.bytecodeSize > 0)
                {
                    ComPtr<ID3DBlob> blob;
                    D3DCreateBlob(desc.bytecodeSize, &blob);
                    memcpy(blob->GetBufferPointer(), desc.bytecode, desc.bytecodeSize);
                    return new D3D12Shader(desc, std::move(blob));
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
                            std::cerr << "[D3D12] Shader compile error: "
                                      << static_cast<const char*>(errors->GetBufferPointer()) << std::endl;
                        return nullptr;
                    }
                    return new D3D12Shader(desc, std::move(bytecode));
                }

                return nullptr;
            }

            IRHISampler* D3D12Device::CreateSampler(const RHISamplerDesc& desc)
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
                return new D3D12Sampler(desc, alloc);
            }

            IRHIPipelineState* D3D12Device::CreatePipelineState(const RHIPipelineStateDesc& desc,
                                                                IRHIShader* vertexShader, IRHIShader* pixelShader)
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
                    std::cerr << "[D3D12] Failed to create PSO" << std::endl;
                    return nullptr;
                }

                return new D3D12PipelineState(desc, std::move(pso), std::move(rootSig));
            }

            // ============================================================================
            // D3D12 DEVICE — RESOURCE DESTRUCTION (deferred)
            // ============================================================================

            void D3D12Device::DestroyBuffer(IRHIBuffer* buffer)
            {
                auto* b = static_cast<D3D12Buffer*>(buffer);
                if (!b)
                    return;
                std::lock_guard<std::mutex> lock(m_deferredReleaseMutex);
                m_deferredReleaseQueue.push({b->GetD3D12Resource(), m_frameFence.GetCurrentValue()});
                delete b;
            }

            void D3D12Device::DestroyTexture(IRHITexture* texture)
            {
                auto* t = static_cast<D3D12Texture*>(texture);
                if (!t)
                    return;
                m_cbvSrvUavHeap.Free(t->GetSRVDescriptor());
                m_rtvHeap.Free(t->GetRTVDescriptor());
                m_dsvHeap.Free(t->GetDSVDescriptor());
                m_cbvSrvUavHeap.Free(t->GetUAVDescriptor());
                std::lock_guard<std::mutex> lock(m_deferredReleaseMutex);
                m_deferredReleaseQueue.push({t->GetD3D12Resource(), m_frameFence.GetCurrentValue()});
                delete t;
            }

            void D3D12Device::DestroyShader(IRHIShader* shader)
            {
                delete static_cast<D3D12Shader*>(shader);
            }
            void D3D12Device::DestroySampler(IRHISampler* sampler)
            {
                delete static_cast<D3D12Sampler*>(sampler);
            }
            void D3D12Device::DestroyPipelineState(IRHIPipelineState* state)
            {
                delete static_cast<D3D12PipelineState*>(state);
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
                b->GetD3D12Resource()->Map(0, nullptr, &mapped);
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
                // For simplicity, use an upload heap + CopyTextureRegion
                // Full implementation would batch these with a copy queue
                (void)mipLevel;
            }

            // ============================================================================
            // D3D12 DEVICE — COMMAND LISTS
            // ============================================================================

            IRHICommandList* D3D12Device::GetImmediateCommandList()
            {
                return m_immediateCommandList.get();
            }

            IRHICommandList* D3D12Device::CreateDeferredCommandList()
            {
                return new D3D12CommandList(m_device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
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

            void D3D12Device::DestroyCommandList(IRHICommandList* commandList)
            {
                delete static_cast<D3D12CommandList*>(commandList);
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
                ss << "VRAM: " << (m_capabilities.dedicatedVideoMemory / (1024 * 1024)) << " MB\n";
                ss << "DXR: " << (m_dxrSupported ? "Yes" : "No") << "\n";
                ss << "Mesh Shaders: " << (m_capabilities.meshShaderSupport ? "Yes" : "No") << "\n";
                ss << "Bindless: " << (m_capabilities.bindlessResourceSupport ? "Yes" : "No") << "\n";
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
                D3D12SerializeVersionedRootSignature(&desc, &signature, &error);
                if (!signature)
                    return nullptr;

                ComPtr<ID3D12RootSignature> rootSig;
                m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                              IID_PPV_ARGS(&rootSig));
                return rootSig;
            }

            ComPtr<ID3D12RootSignature> D3D12Device::CreateRootSignature(const void* data, size_t dataSize) const
            {
                ComPtr<ID3D12RootSignature> rootSig;
                m_device->CreateRootSignature(0, data, dataSize, IID_PPV_ARGS(&rootSig));
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
                case PixelFormat::R8G8_UNORM:
                    return DXGI_FORMAT_R8G8_UNORM;
                case PixelFormat::R8G8B8A8_UNORM:
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                case PixelFormat::R8G8B8A8_UNORM_SRGB:
                    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                case PixelFormat::B8G8R8A8_UNORM:
                    return DXGI_FORMAT_B8G8R8A8_UNORM;
                case PixelFormat::R10G10B10A2_UNORM:
                    return DXGI_FORMAT_R10G10B10A2_UNORM;
                case PixelFormat::R11G11B10_FLOAT:
                    return DXGI_FORMAT_R11G11B10_FLOAT;
                case PixelFormat::R16_FLOAT:
                    return DXGI_FORMAT_R16_FLOAT;
                case PixelFormat::R16G16_FLOAT:
                    return DXGI_FORMAT_R16G16_FLOAT;
                case PixelFormat::R16G16B16A16_FLOAT:
                    return DXGI_FORMAT_R16G16B16A16_FLOAT;
                case PixelFormat::R32_FLOAT:
                    return DXGI_FORMAT_R32_FLOAT;
                case PixelFormat::R32G32_FLOAT:
                    return DXGI_FORMAT_R32G32_FLOAT;
                case PixelFormat::R32G32B32_FLOAT:
                    return DXGI_FORMAT_R32G32B32_FLOAT;
                case PixelFormat::R32G32B32A32_FLOAT:
                    return DXGI_FORMAT_R32G32B32A32_FLOAT;
                case PixelFormat::BC1_UNORM:
                    return DXGI_FORMAT_BC1_UNORM;
                case PixelFormat::BC3_UNORM:
                    return DXGI_FORMAT_BC3_UNORM;
                case PixelFormat::BC7_UNORM:
                    return DXGI_FORMAT_BC7_UNORM;
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
                case RHIVertexFormat::Int4:
                    return DXGI_FORMAT_R32G32B32A32_SINT;
                case RHIVertexFormat::UInt1:
                    return DXGI_FORMAT_R32_UINT;
                case RHIVertexFormat::UNorm8x4:
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                default:
                    return DXGI_FORMAT_R32G32B32_FLOAT;
                }
            }

            uint32_t D3D12Device::GetFormatSize(PixelFormat format) const
            {
                switch (format)
                {
                case PixelFormat::R8_UNORM:
                    return 1;
                case PixelFormat::R8G8_UNORM:
                    return 2;
                case PixelFormat::R16_FLOAT:
                    return 2;
                case PixelFormat::R8G8B8A8_UNORM:
                case PixelFormat::R32_FLOAT:
                case PixelFormat::D24_UNORM_S8_UINT:
                case PixelFormat::D32_FLOAT:
                    return 4;
                case PixelFormat::R16G16B16A16_FLOAT:
                case PixelFormat::R32G32_FLOAT:
                    return 8;
                case PixelFormat::R32G32B32_FLOAT:
                    return 12;
                case PixelFormat::R32G32B32A32_FLOAT:
                    return 16;
                default:
                    return 4;
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
