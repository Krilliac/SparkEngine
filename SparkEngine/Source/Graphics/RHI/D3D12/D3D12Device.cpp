/**
 * @file D3D12Device.cpp
 * @brief DirectX 12 implementation of the RHI device interface
 * @author Spark Engine Team
 * @date 2025
 *
 * Full Direct3D 12 backend implementing device lifetime, resource creation,
 * command list management, descriptor heap allocation, fence-based
 * synchronization, and DXGI swap chain presentation.
 */

#ifdef _WIN32

#include "D3D12Device.h"
#include "../RHIFactory.h"
#include "../../../Utils/Logger.h"

#include <algorithm>
#include <cassert>
#include <format>

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d12sdklayers.h>
#endif

// Link against required libraries
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace Spark
{
    namespace RHI
    {
        namespace D3D12
        {

            // ================================================================
            // Helper: Check HRESULT and log on failure
            // ================================================================

            static bool CheckHR(HRESULT hr, const char* operation)
            {
                if (FAILED(hr))
                {
                    LOG_ERROR("D3D12: {} failed with HRESULT 0x{:08X}", operation, static_cast<unsigned>(hr));
                    return false;
                }
                return true;
            }

            // ================================================================
            // DescriptorHeapAllocator
            // ================================================================

            bool DescriptorHeapAllocator::Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                     uint32_t descriptorCount, bool shaderVisible)
            {
                D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
                heapDesc.Type = type;
                heapDesc.NumDescriptors = descriptorCount;
                heapDesc.Flags =
                    shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                heapDesc.NodeMask = 0;

                HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap));
                if (!CheckHR(hr, "CreateDescriptorHeap"))
                {
                    return false;
                }

                m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);
                m_capacity = descriptorCount;
                m_nextFreeIndex = 0;
                m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();

                if (shaderVisible)
                {
                    m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
                }
                else
                {
                    m_gpuStart = {};
                }

                m_freeList.clear();
                return true;
            }

            DescriptorAllocation DescriptorHeapAllocator::Allocate(uint32_t count)
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                DescriptorAllocation allocation = {};

                // For single-descriptor allocations, try the free list first
                if (count == 1 && !m_freeList.empty())
                {
                    uint32_t index = m_freeList.back();
                    m_freeList.pop_back();

                    allocation.index = index;
                    allocation.count = 1;
                    allocation.cpuHandle.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_descriptorSize;
                    if (m_gpuStart.ptr != 0)
                    {
                        allocation.gpuHandle.ptr = m_gpuStart.ptr + static_cast<UINT64>(index) * m_descriptorSize;
                    }
                    return allocation;
                }

                // Allocate from the end of the linear region
                if (m_nextFreeIndex + count > m_capacity)
                {
                    LOG_ERROR("D3D12: DescriptorHeapAllocator out of descriptors (capacity={})", m_capacity);
                    return allocation;
                }

                allocation.index = m_nextFreeIndex;
                allocation.count = count;
                allocation.cpuHandle.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(m_nextFreeIndex) * m_descriptorSize;
                if (m_gpuStart.ptr != 0)
                {
                    allocation.gpuHandle.ptr = m_gpuStart.ptr + static_cast<UINT64>(m_nextFreeIndex) * m_descriptorSize;
                }

                m_nextFreeIndex += count;
                return allocation;
            }

            void DescriptorHeapAllocator::Free(const DescriptorAllocation& allocation)
            {
                if (!allocation.IsValid())
                {
                    return;
                }

                std::lock_guard<std::mutex> lock(m_mutex);

                // Return individual indices to the free list
                for (uint32_t i = 0; i < allocation.count; ++i)
                {
                    m_freeList.push_back(allocation.index + i);
                }
            }

            // ================================================================
            // D3D12Fence
            // ================================================================

            D3D12Fence::~D3D12Fence()
            {
                if (m_fenceEvent)
                {
                    CloseHandle(m_fenceEvent);
                    m_fenceEvent = nullptr;
                }
            }

            bool D3D12Fence::Initialize(ID3D12Device* device, uint64_t initialValue)
            {
                m_currentValue = initialValue;

                HRESULT hr = device->CreateFence(initialValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
                if (!CheckHR(hr, "CreateFence"))
                {
                    return false;
                }

                m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (!m_fenceEvent)
                {
                    LOG_ERROR("D3D12: Failed to create fence event");
                    return false;
                }

                return true;
            }

            uint64_t D3D12Fence::Signal(ID3D12CommandQueue* queue)
            {
                ++m_currentValue;
                HRESULT hr = queue->Signal(m_fence.Get(), m_currentValue);
                CheckHR(hr, "ID3D12CommandQueue::Signal");
                return m_currentValue;
            }

            void D3D12Fence::WaitForValue(uint64_t value) const
            {
                if (m_fence->GetCompletedValue() >= value)
                {
                    return;
                }

                HRESULT hr = m_fence->SetEventOnCompletion(value, m_fenceEvent);
                if (SUCCEEDED(hr))
                {
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

            // ================================================================
            // D3D12Buffer
            // ================================================================

            D3D12Buffer::D3D12Buffer(const RHIBufferDesc& desc, ComPtr<ID3D12Resource> resource,
                                     ComPtr<ID3D12Resource> uploadResource)
                : m_desc(desc), m_resource(std::move(resource)), m_uploadResource(std::move(uploadResource))
            {
            }

            // ================================================================
            // D3D12Texture
            // ================================================================

            D3D12Texture::D3D12Texture(const RHITextureDesc& desc, ComPtr<ID3D12Resource> resource,
                                       const DescriptorAllocation& srvDescriptor,
                                       const DescriptorAllocation& rtvDescriptor,
                                       const DescriptorAllocation& dsvDescriptor,
                                       const DescriptorAllocation& uavDescriptor)
                : m_desc(desc), m_resource(std::move(resource)), m_srvDescriptor(srvDescriptor),
                  m_rtvDescriptor(rtvDescriptor), m_dsvDescriptor(dsvDescriptor), m_uavDescriptor(uavDescriptor)
            {
            }

            // ================================================================
            // D3D12Shader
            // ================================================================

            D3D12Shader::D3D12Shader(const RHIShaderDesc& desc, ComPtr<ID3DBlob> bytecodeBlob)
                : m_desc(desc), m_bytecodeBlob(std::move(bytecodeBlob))
            {
            }

            // ================================================================
            // D3D12Sampler
            // ================================================================

            D3D12Sampler::D3D12Sampler(const RHISamplerDesc& desc, const DescriptorAllocation& descriptor)
                : m_desc(desc), m_descriptor(descriptor)
            {
            }

            // ================================================================
            // D3D12PipelineState
            // ================================================================

            D3D12PipelineState::D3D12PipelineState(const RHIPipelineStateDesc& desc, ComPtr<ID3D12PipelineState> pso,
                                                   ComPtr<ID3D12RootSignature> rootSignature)
                : m_desc(desc), m_pso(std::move(pso)), m_rootSignature(std::move(rootSignature))
            {
            }

            // ================================================================
            // D3D12SwapChain
            // ================================================================

            D3D12SwapChain::D3D12SwapChain(ID3D12Device* device, ID3D12CommandQueue* commandQueue,
                                           IDXGIFactory4* dxgiFactory, DescriptorHeapAllocator* rtvAllocator,
                                           const RHISwapChainDesc& desc)
                : m_desc(desc), m_device(device), m_commandQueue(commandQueue), m_rtvAllocator(rtvAllocator)
            {
                assert(device && commandQueue && dxgiFactory && rtvAllocator);

                DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
                swapChainDesc.Width = desc.width;
                swapChainDesc.Height = desc.height;
                swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // Default; overridden below if needed
                swapChainDesc.SampleDesc.Count = 1;
                swapChainDesc.SampleDesc.Quality = 0;
                swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                swapChainDesc.BufferCount = std::clamp(desc.bufferCount, 2u, MAX_BACK_BUFFER_COUNT);
                swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

                // Map PixelFormat to DXGI_FORMAT for the swap chain
                switch (desc.format)
                {
                case PixelFormat::R8G8B8A8_UNORM:
                    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    break;
                case PixelFormat::R8G8B8A8_UNORM_SRGB:
                    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    break;
                case PixelFormat::B8G8R8A8_UNORM:
                    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    break;
                case PixelFormat::B8G8R8A8_UNORM_SRGB:
                    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    break;
                case PixelFormat::R10G10B10A2_UNORM:
                    swapChainDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                    break;
                case PixelFormat::R16G16B16A16_FLOAT:
                    swapChainDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    break;
                default:
                    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    break;
                }

                HWND hwnd = static_cast<HWND>(desc.windowHandle);
                ComPtr<IDXGISwapChain1> swapChain1;
                HRESULT hr = dxgiFactory->CreateSwapChainForHwnd(commandQueue, hwnd, &swapChainDesc, nullptr, nullptr,
                                                                 &swapChain1);
                if (!CheckHR(hr, "CreateSwapChainForHwnd"))
                {
                    return;
                }

                // Disable Alt+Enter fullscreen toggle managed by DXGI
                dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

                hr = swapChain1.As(&m_swapChain);
                if (!CheckHR(hr, "QueryInterface IDXGISwapChain4"))
                {
                    return;
                }

                CreateBackBufferResources();
            }

            D3D12SwapChain::~D3D12SwapChain()
            {
                ReleaseBackBufferResources();
            }

            bool D3D12SwapChain::Present(bool vsync)
            {
                if (!m_swapChain)
                {
                    return false;
                }

                UINT syncInterval = vsync ? 1 : 0;
                UINT presentFlags = vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING;

                HRESULT hr = m_swapChain->Present(syncInterval, presentFlags);
                if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
                {
                    LOG_ERROR("D3D12: Device lost during Present");
                    return false;
                }
                return SUCCEEDED(hr);
            }

            bool D3D12SwapChain::Resize(uint32_t width, uint32_t height)
            {
                if (!m_swapChain || (width == 0 && height == 0))
                {
                    return false;
                }

                ReleaseBackBufferResources();

                HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                                        DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
                if (!CheckHR(hr, "ResizeBuffers"))
                {
                    return false;
                }

                m_desc.width = width;
                m_desc.height = height;

                return CreateBackBufferResources();
            }

            IRHITexture* D3D12SwapChain::GetBackBuffer()
            {
                if (!m_swapChain)
                {
                    return nullptr;
                }

                uint32_t index = m_swapChain->GetCurrentBackBufferIndex();
                return m_backBuffers[index].get();
            }

            bool D3D12SwapChain::CreateBackBufferResources()
            {
                DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
                m_swapChain->GetDesc1(&swapDesc);

                for (uint32_t i = 0; i < swapDesc.BufferCount; ++i)
                {
                    ComPtr<ID3D12Resource> backBufferResource;
                    HRESULT hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&backBufferResource));
                    if (!CheckHR(hr, "IDXGISwapChain::GetBuffer"))
                    {
                        return false;
                    }

                    // Allocate an RTV descriptor for this back buffer
                    DescriptorAllocation rtvAlloc = m_rtvAllocator->Allocate(1);
                    if (!rtvAlloc.IsValid())
                    {
                        LOG_ERROR("D3D12: Failed to allocate RTV for back buffer {}", i);
                        return false;
                    }

                    m_device->CreateRenderTargetView(backBufferResource.Get(), nullptr, rtvAlloc.cpuHandle);

                    m_backBufferRTVs[i] = rtvAlloc;

                    // Build an RHITextureDesc to wrap the back buffer
                    RHITextureDesc texDesc = {};
                    texDesc.width = m_desc.width;
                    texDesc.height = m_desc.height;
                    texDesc.depth = 1;
                    texDesc.mipLevels = 1;
                    texDesc.format = m_desc.format;
                    texDesc.type = RHITextureType::Texture2D;
                    texDesc.usage = RHITextureUsage::RenderTarget;
                    texDesc.debugName = std::format("BackBuffer_{}", i);

                    m_backBuffers[i] = std::make_unique<D3D12Texture>(texDesc, std::move(backBufferResource),
                                                                      DescriptorAllocation{}, // no SRV
                                                                      rtvAlloc,               // RTV
                                                                      DescriptorAllocation{}, // no DSV
                                                                      DescriptorAllocation{}  // no UAV
                    );
                    m_backBuffers[i]->SetCurrentState(D3D12_RESOURCE_STATE_PRESENT);
                }

                return true;
            }

            void D3D12SwapChain::ReleaseBackBufferResources()
            {
                for (uint32_t i = 0; i < MAX_BACK_BUFFER_COUNT; ++i)
                {
                    m_backBuffers[i].reset();
                    if (m_backBufferRTVs[i].IsValid())
                    {
                        m_rtvAllocator->Free(m_backBufferRTVs[i]);
                        m_backBufferRTVs[i] = {};
                    }
                }
            }

            // ================================================================
            // D3D12CommandList
            // ================================================================

            D3D12CommandList::D3D12CommandList(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type,
                                               ID3D12PipelineState* initialPSO)
                : m_type(type)
            {
                HRESULT hr = device->CreateCommandAllocator(type, IID_PPV_ARGS(&m_commandAllocator));
                if (!CheckHR(hr, "CreateCommandAllocator"))
                {
                    return;
                }

                hr = device->CreateCommandList(0, type, m_commandAllocator.Get(), initialPSO,
                                               IID_PPV_ARGS(&m_commandList));
                if (!CheckHR(hr, "CreateCommandList"))
                {
                    return;
                }

                // Command lists are created in the recording state; close it so
                // the caller must explicitly call Begin() before recording.
                m_commandList->Close();
            }

            void D3D12CommandList::Begin()
            {
                m_commandAllocator->Reset();
                m_commandList->Reset(m_commandAllocator.Get(), nullptr);
                m_pendingBarriers.clear();
                m_currentRootSignature = nullptr;
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

            // -- Render targets ------------------------------------------------

            void D3D12CommandList::SetRenderTargets(IRHITexture** renderTargets, uint32_t count,
                                                    IRHITexture* depthStencil)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[8] = {};

                for (uint32_t i = 0; i < count; ++i)
                {
                    auto* tex = static_cast<D3D12Texture*>(renderTargets[i]);
                    if (tex && tex->GetRTVDescriptor().IsValid())
                    {
                        rtvHandles[i] = tex->GetRTVDescriptor().cpuHandle;
                    }
                }

                D3D12_CPU_DESCRIPTOR_HANDLE* dsvHandle = nullptr;
                D3D12_CPU_DESCRIPTOR_HANDLE dsvTemp = {};
                if (depthStencil)
                {
                    auto* dsTex = static_cast<D3D12Texture*>(depthStencil);
                    if (dsTex->GetDSVDescriptor().IsValid())
                    {
                        dsvTemp = dsTex->GetDSVDescriptor().cpuHandle;
                        dsvHandle = &dsvTemp;
                    }
                }

                m_commandList->OMSetRenderTargets(count, count > 0 ? rtvHandles : nullptr, FALSE, dsvHandle);
            }

            void D3D12CommandList::ClearRenderTarget(IRHITexture* target, const float color[4])
            {
                if (!target)
                {
                    return;
                }

                auto* tex = static_cast<D3D12Texture*>(target);
                if (tex->GetRTVDescriptor().IsValid())
                {
                    m_commandList->ClearRenderTargetView(tex->GetRTVDescriptor().cpuHandle, color, 0, nullptr);
                }
            }

            void D3D12CommandList::ClearDepthStencil(IRHITexture* target, float depth, uint8_t stencil)
            {
                if (!target)
                {
                    return;
                }

                auto* tex = static_cast<D3D12Texture*>(target);
                if (tex->GetDSVDescriptor().IsValid())
                {
                    m_commandList->ClearDepthStencilView(tex->GetDSVDescriptor().cpuHandle,
                                                         D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depth,
                                                         stencil, 0, nullptr);
                }
            }

            // -- Viewport & scissor -------------------------------------------

            void D3D12CommandList::SetViewport(const RHIViewport& viewport)
            {
                D3D12_VIEWPORT vp = {};
                vp.TopLeftX = viewport.x;
                vp.TopLeftY = viewport.y;
                vp.Width = viewport.width;
                vp.Height = viewport.height;
                vp.MinDepth = viewport.minDepth;
                vp.MaxDepth = viewport.maxDepth;
                m_commandList->RSSetViewports(1, &vp);
            }

            void D3D12CommandList::SetScissorRect(const RHIScissorRect& rect)
            {
                D3D12_RECT d3dRect = {};
                d3dRect.left = static_cast<LONG>(rect.left);
                d3dRect.top = static_cast<LONG>(rect.top);
                d3dRect.right = static_cast<LONG>(rect.right);
                d3dRect.bottom = static_cast<LONG>(rect.bottom);
                m_commandList->RSSetScissorRects(1, &d3dRect);
            }

            // -- Pipeline state -----------------------------------------------

            void D3D12CommandList::SetPipelineState(IRHIPipelineState* pipelineState)
            {
                if (!pipelineState)
                {
                    return;
                }

                auto* d3d12PSO = static_cast<D3D12PipelineState*>(pipelineState);
                m_commandList->SetPipelineState(d3d12PSO->GetPSO());

                // Bind root signature if it changed
                ID3D12RootSignature* rootSig = d3d12PSO->GetRootSignature();
                if (rootSig && rootSig != m_currentRootSignature)
                {
                    m_commandList->SetGraphicsRootSignature(rootSig);
                    m_currentRootSignature = rootSig;
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
                case RHIPrimitiveTopology::PatchList:
                    d3dTopology = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
                    break;
                }
                m_commandList->IASetPrimitiveTopology(d3dTopology);
            }

            // -- Resource binding ---------------------------------------------

            void D3D12CommandList::SetVertexBuffer(IRHIBuffer* buffer, uint32_t slot, uint32_t offset)
            {
                if (!buffer)
                {
                    return;
                }

                auto* d3d12Buffer = static_cast<D3D12Buffer*>(buffer);
                D3D12_VERTEX_BUFFER_VIEW vbView = {};
                vbView.BufferLocation = d3d12Buffer->GetGPUVirtualAddress() + offset;
                vbView.SizeInBytes = static_cast<UINT>(d3d12Buffer->GetSize() - offset);
                vbView.StrideInBytes = d3d12Buffer->GetStride();
                m_commandList->IASetVertexBuffers(slot, 1, &vbView);
            }

            void D3D12CommandList::SetIndexBuffer(IRHIBuffer* buffer, uint32_t offset)
            {
                if (!buffer)
                {
                    return;
                }

                auto* d3d12Buffer = static_cast<D3D12Buffer*>(buffer);
                D3D12_INDEX_BUFFER_VIEW ibView = {};
                ibView.BufferLocation = d3d12Buffer->GetGPUVirtualAddress() + offset;
                ibView.SizeInBytes = static_cast<UINT>(d3d12Buffer->GetSize() - offset);
                ibView.Format = (d3d12Buffer->GetStride() == 4) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
                m_commandList->IASetIndexBuffer(&ibView);
            }

            void D3D12CommandList::SetConstantBuffer(RHIShaderStage /*stage*/, uint32_t slot, IRHIBuffer* buffer)
            {
                if (!buffer)
                {
                    return;
                }

                auto* d3d12Buffer = static_cast<D3D12Buffer*>(buffer);
                // Root parameter 0 is the CBV descriptor table. For simplicity,
                // bind directly via root CBV at the given slot.
                m_commandList->SetGraphicsRootConstantBufferView(slot, d3d12Buffer->GetGPUVirtualAddress());
            }

            void D3D12CommandList::SetShaderResource(RHIShaderStage /*stage*/, uint32_t slot, IRHITexture* texture)
            {
                if (!texture)
                {
                    return;
                }

                auto* d3d12Texture = static_cast<D3D12Texture*>(texture);
                const auto& srvDesc = d3d12Texture->GetSRVDescriptor();
                if (srvDesc.IsValid())
                {
                    // Root parameter 1 is the SRV descriptor table.
                    // For a single descriptor table binding, use the GPU handle.
                    m_commandList->SetGraphicsRootDescriptorTable(1, srvDesc.gpuHandle);
                }
            }

            void D3D12CommandList::SetSampler(RHIShaderStage /*stage*/, uint32_t /*slot*/, IRHISampler* sampler)
            {
                if (!sampler)
                {
                    return;
                }

                auto* d3d12Sampler = static_cast<D3D12Sampler*>(sampler);
                const auto& samplerDesc = d3d12Sampler->GetDescriptor();
                if (samplerDesc.IsValid())
                {
                    // Root parameter 2 is the sampler descriptor table.
                    m_commandList->SetGraphicsRootDescriptorTable(2, samplerDesc.gpuHandle);
                }
            }

            // -- Draw & dispatch commands -------------------------------------

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

            // -- Debug markers ------------------------------------------------

            void D3D12CommandList::BeginEvent(const char* name)
            {
                if (!name)
                {
                    return;
                }

                // Use PIX3 BeginEvent encoding via the command list (type 1 = ANSI string)
                m_commandList->BeginEvent(1, name, static_cast<UINT>(strlen(name) + 1));
            }

            void D3D12CommandList::EndEvent()
            {
                m_commandList->EndEvent();
            }

            void D3D12CommandList::SetMarker(const char* name)
            {
                if (!name)
                {
                    return;
                }
                m_commandList->SetMarker(1, name, static_cast<UINT>(strlen(name) + 1));
            }

            // -- Barrier helpers ----------------------------------------------

            void D3D12CommandList::TransitionBarrier(D3D12Texture* resource, D3D12_RESOURCE_STATES stateBefore,
                                                     D3D12_RESOURCE_STATES stateAfter)
            {
                if (!resource || stateBefore == stateAfter)
                {
                    return;
                }

                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = resource->GetD3D12Resource();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = stateBefore;
                barrier.Transition.StateAfter = stateAfter;

                m_pendingBarriers.push_back(barrier);
                resource->SetCurrentState(stateAfter);
            }

            void D3D12CommandList::UAVBarrier(ID3D12Resource* resource)
            {
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
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

            // ================================================================
            // D3D12Device — constructor / destructor
            // ================================================================

            D3D12Device::D3D12Device()
            {
                m_capabilities = {};
                m_statistics = {};
            }

            D3D12Device::~D3D12Device()
            {
                Shutdown();
            }

            // ================================================================
            // D3D12Device — Initialize / Shutdown
            // ================================================================

            bool D3D12Device::Initialize(const RHIDeviceDesc& desc)
            {
                m_debugEnabled = desc.enableDebugLayer;

                // Enable the debug layer before creating any D3D12 objects
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
                            {
                                debugController1->SetEnableGPUBasedValidation(TRUE);
                            }
                        }
                    }
                }

                if (!CreateDevice(desc))
                {
                    return false;
                }

                if (!CreateCommandQueues())
                {
                    return false;
                }

                if (!CreateDescriptorHeaps())
                {
                    return false;
                }

                if (!m_frameFence.Initialize(m_device.Get(), 0))
                {
                    return false;
                }

                if (!CreateFrameResources())
                {
                    return false;
                }

                DetectCapabilities();
                DetectDXRSupport();

                // Create the immediate command list
                m_immediateCommandList =
                    std::make_unique<D3D12CommandList>(m_device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);

                LOG_INFO("D3D12: Device initialized — {}", m_capabilities.deviceName);
                return true;
            }

            void D3D12Device::Shutdown()
            {
                WaitForIdle();

                m_immediateCommandList.reset();

                // Flush deferred releases
                ProcessDeferredReleases();

                m_frameFence = {};

                for (auto& frame : m_frameResources)
                {
                    frame.commandAllocator.Reset();
                }

                m_computeQueue.Reset();
                m_copyQueue.Reset();
                m_directQueue.Reset();
                m_infoQueue.Reset();
                m_dxrDevice.Reset();
                m_device.Reset();
                m_adapter.Reset();
                m_dxgiFactory.Reset();
            }

            // ================================================================
            // D3D12Device — Swap chain
            // ================================================================

            std::unique_ptr<IRHISwapChain> D3D12Device::CreateSwapChain(const RHISwapChainDesc& desc)
            {
                return std::make_unique<D3D12SwapChain>(m_device.Get(), m_directQueue.Get(), m_dxgiFactory.Get(),
                                                        &m_rtvHeap, desc);
            }

            // ================================================================
            // D3D12Device — Resource creation
            // ================================================================

            IRHIBuffer* D3D12Device::CreateBuffer(const RHIBufferDesc& desc)
            {
                D3D12_HEAP_PROPERTIES heapProps = {};
                D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
                ComPtr<ID3D12Resource> uploadResource;

                bool isUploadHeap =
                    (desc.access == RHIBufferAccess::Dynamic || desc.access == RHIBufferAccess::Staging);
                bool isReadbackHeap = (desc.access == RHIBufferAccess::ReadBack);

                if (isUploadHeap)
                {
                    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
                    initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
                }
                else if (isReadbackHeap)
                {
                    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
                    initialState = D3D12_RESOURCE_STATE_COPY_DEST;
                }
                else
                {
                    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
                    initialState = GetInitialResourceState(desc.access);
                }

                // Constant buffers must be 256-byte aligned
                uint64_t alignedSize = desc.size;
                if (desc.usage & RHIBufferUsage::Constant)
                {
                    alignedSize = (alignedSize + 255) & ~255ULL;
                }

                D3D12_RESOURCE_DESC resourceDesc = {};
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                resourceDesc.Alignment = 0;
                resourceDesc.Width = alignedSize;
                resourceDesc.Height = 1;
                resourceDesc.DepthOrArraySize = 1;
                resourceDesc.MipLevels = 1;
                resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
                resourceDesc.SampleDesc.Count = 1;
                resourceDesc.SampleDesc.Quality = 0;
                resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

                if (desc.usage & RHIBufferUsage::Storage)
                {
                    resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                }

                ComPtr<ID3D12Resource> resource;
                HRESULT hr = m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
                                                               initialState, nullptr, IID_PPV_ARGS(&resource));
                if (!CheckHR(hr, "CreateCommittedResource (buffer)"))
                {
                    return nullptr;
                }

                // For static buffers with initial data, create an upload buffer and copy
                if (desc.initialData && desc.access == RHIBufferAccess::Static)
                {
                    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
                    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

                    hr = m_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(&uploadResource));
                    if (SUCCEEDED(hr))
                    {
                        void* mapped = nullptr;
                        uploadResource->Map(0, nullptr, &mapped);
                        memcpy(mapped, desc.initialData, desc.size);
                        uploadResource->Unmap(0, nullptr);
                    }
                }

                // For upload-heap buffers with initial data, copy directly
                if (desc.initialData && isUploadHeap)
                {
                    void* mapped = nullptr;
                    resource->Map(0, nullptr, &mapped);
                    memcpy(mapped, desc.initialData, desc.size);
                    resource->Unmap(0, nullptr);
                }

                auto* buffer = new D3D12Buffer(desc, std::move(resource), std::move(uploadResource));

                // Set debug name
                if (!desc.debugName.empty())
                {
                    std::wstring wideName(desc.debugName.begin(), desc.debugName.end());
                    buffer->GetD3D12Resource()->SetName(wideName.c_str());
                }

                // Persistently map dynamic buffers
                if (isUploadHeap)
                {
                    void* mapped = nullptr;
                    buffer->GetD3D12Resource()->Map(0, nullptr, &mapped);
                    buffer->SetMappedPointer(mapped);
                }

                return buffer;
            }

            IRHITexture* D3D12Device::CreateTexture(const RHITextureDesc& desc)
            {
                D3D12_RESOURCE_DESC resourceDesc = {};
                resourceDesc.Format = ConvertFormat(desc.format);
                resourceDesc.Width = desc.width;
                resourceDesc.Height = desc.height;
                resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
                resourceDesc.SampleDesc.Count = desc.sampleCount;
                resourceDesc.SampleDesc.Quality = 0;

                switch (desc.type)
                {
                case RHITextureType::Texture1D:
                    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
                    resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.arraySize);
                    break;
                case RHITextureType::Texture3D:
                    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
                    resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.depth);
                    break;
                case RHITextureType::TextureCube:
                    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    resourceDesc.DepthOrArraySize = static_cast<UINT16>(6 * desc.arraySize);
                    break;
                case RHITextureType::TextureCubeArray:
                    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    resourceDesc.DepthOrArraySize = static_cast<UINT16>(6 * desc.arraySize);
                    break;
                case RHITextureType::Texture2DArray:
                    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.arraySize);
                    break;
                default: // Texture2D
                    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.arraySize);
                    break;
                }

                resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
                if (desc.usage & RHITextureUsage::RenderTarget)
                {
                    resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                }
                if (desc.usage & RHITextureUsage::DepthStencil)
                {
                    resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                }
                if (desc.usage & RHITextureUsage::UnorderedAccess)
                {
                    resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                }

                D3D12_HEAP_PROPERTIES heapProps = {};
                heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

                D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
                D3D12_CLEAR_VALUE clearValue = {};
                D3D12_CLEAR_VALUE* pClearValue = nullptr;

                if (desc.usage & RHITextureUsage::RenderTarget)
                {
                    initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    clearValue.Format = resourceDesc.Format;
                    clearValue.Color[0] = desc.clearColor[0];
                    clearValue.Color[1] = desc.clearColor[1];
                    clearValue.Color[2] = desc.clearColor[2];
                    clearValue.Color[3] = desc.clearColor[3];
                    pClearValue = &clearValue;
                }
                else if (desc.usage & RHITextureUsage::DepthStencil)
                {
                    initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                    clearValue.Format = resourceDesc.Format;
                    clearValue.DepthStencil.Depth = desc.clearDepth;
                    clearValue.DepthStencil.Stencil = desc.clearStencil;
                    pClearValue = &clearValue;
                }

                ComPtr<ID3D12Resource> resource;
                HRESULT hr = m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
                                                               initialState, pClearValue, IID_PPV_ARGS(&resource));
                if (!CheckHR(hr, "CreateCommittedResource (texture)"))
                {
                    return nullptr;
                }

                // Create descriptors based on usage
                DescriptorAllocation srvAlloc = {};
                DescriptorAllocation rtvAlloc = {};
                DescriptorAllocation dsvAlloc = {};
                DescriptorAllocation uavAlloc = {};

                if (desc.usage & RHITextureUsage::ShaderResource)
                {
                    srvAlloc = m_cbvSrvUavHeap.Allocate(1);
                    if (srvAlloc.IsValid())
                    {
                        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                        srvDesc.Format = ConvertFormat(desc.format);
                        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

                        switch (desc.type)
                        {
                        case RHITextureType::Texture1D:
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                            srvDesc.Texture1D.MipLevels = desc.mipLevels;
                            break;
                        case RHITextureType::Texture3D:
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                            srvDesc.Texture3D.MipLevels = desc.mipLevels;
                            break;
                        case RHITextureType::TextureCube:
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                            srvDesc.TextureCube.MipLevels = desc.mipLevels;
                            break;
                        case RHITextureType::Texture2DArray:
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                            srvDesc.Texture2DArray.MipLevels = desc.mipLevels;
                            srvDesc.Texture2DArray.ArraySize = desc.arraySize;
                            break;
                        default:
                            if (desc.sampleCount > 1)
                            {
                                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                            }
                            else
                            {
                                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                                srvDesc.Texture2D.MipLevels = desc.mipLevels;
                            }
                            break;
                        }

                        // Use a typeless-compatible SRV format for depth textures
                        if (desc.usage & RHITextureUsage::DepthStencil)
                        {
                            switch (desc.format)
                            {
                            case PixelFormat::D16_UNORM:
                                srvDesc.Format = DXGI_FORMAT_R16_UNORM;
                                break;
                            case PixelFormat::D24_UNORM_S8_UINT:
                                srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                                break;
                            case PixelFormat::D32_FLOAT:
                                srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
                                break;
                            case PixelFormat::D32_FLOAT_S8_UINT:
                                srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
                                break;
                            default:
                                break;
                            }
                        }

                        m_device->CreateShaderResourceView(resource.Get(), &srvDesc, srvAlloc.cpuHandle);
                    }
                }

                if (desc.usage & RHITextureUsage::RenderTarget)
                {
                    rtvAlloc = m_rtvHeap.Allocate(1);
                    if (rtvAlloc.IsValid())
                    {
                        m_device->CreateRenderTargetView(resource.Get(), nullptr, rtvAlloc.cpuHandle);
                    }
                }

                if (desc.usage & RHITextureUsage::DepthStencil)
                {
                    dsvAlloc = m_dsvHeap.Allocate(1);
                    if (dsvAlloc.IsValid())
                    {
                        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
                        dsvDesc.Format = ConvertFormat(desc.format);
                        dsvDesc.ViewDimension =
                            (desc.sampleCount > 1) ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
                        m_device->CreateDepthStencilView(resource.Get(), &dsvDesc, dsvAlloc.cpuHandle);
                    }
                }

                if (desc.usage & RHITextureUsage::UnorderedAccess)
                {
                    uavAlloc = m_cbvSrvUavHeap.Allocate(1);
                    if (uavAlloc.IsValid())
                    {
                        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
                        uavDesc.Format = ConvertFormat(desc.format);
                        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                        uavDesc.Texture2D.MipSlice = 0;
                        m_device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, uavAlloc.cpuHandle);
                    }
                }

                auto* texture = new D3D12Texture(desc, std::move(resource), srvAlloc, rtvAlloc, dsvAlloc, uavAlloc);
                texture->SetCurrentState(initialState);

                if (!desc.debugName.empty())
                {
                    std::wstring wideName(desc.debugName.begin(), desc.debugName.end());
                    texture->GetD3D12Resource()->SetName(wideName.c_str());
                }

                return texture;
            }

            IRHIShader* D3D12Device::CreateShader(const RHIShaderDesc& desc)
            {
                ComPtr<ID3DBlob> bytecodeBlob;

                // If pre-compiled bytecode is provided, wrap it in a blob
                if (desc.bytecode && desc.bytecodeSize > 0)
                {
                    HRESULT hr = D3DCreateBlob(desc.bytecodeSize, &bytecodeBlob);
                    if (!CheckHR(hr, "D3DCreateBlob"))
                    {
                        return nullptr;
                    }
                    memcpy(bytecodeBlob->GetBufferPointer(), desc.bytecode, desc.bytecodeSize);
                }
                else if (!desc.sourceCode.empty())
                {
                    // Compile HLSL source code at runtime
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

                    UINT compileFlags = 0;
                    if (m_debugEnabled)
                    {
                        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
                    }
                    else
                    {
                        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
                    }

                    // Build defines
                    std::vector<D3D_SHADER_MACRO> macros;
                    for (const auto& define : desc.defines)
                    {
                        auto eqPos = define.find('=');
                        if (eqPos != std::string::npos)
                        {
                            macros.push_back({define.c_str(), define.c_str() + eqPos + 1});
                        }
                        else
                        {
                            macros.push_back({define.c_str(), "1"});
                        }
                    }
                    macros.push_back({nullptr, nullptr}); // Sentinel

                    ComPtr<ID3DBlob> errorBlob;
                    HRESULT hr = D3DCompile(desc.sourceCode.c_str(), desc.sourceCode.size(),
                                            desc.filePath.empty() ? nullptr : desc.filePath.c_str(), macros.data(),
                                            D3D_COMPILE_STANDARD_FILE_INCLUDE, desc.entryPoint.c_str(), target,
                                            compileFlags, 0, &bytecodeBlob, &errorBlob);
                    if (!CheckHR(hr, "D3DCompile"))
                    {
                        if (errorBlob)
                        {
                            LOG_ERROR("D3D12: Shader compile error: {}",
                                      static_cast<const char*>(errorBlob->GetBufferPointer()));
                        }
                        return nullptr;
                    }
                }
                else
                {
                    LOG_ERROR("D3D12: CreateShader — no bytecode or source provided");
                    return nullptr;
                }

                return new D3D12Shader(desc, std::move(bytecodeBlob));
            }

            IRHISampler* D3D12Device::CreateSampler(const RHISamplerDesc& desc)
            {
                DescriptorAllocation samplerAlloc = m_samplerHeap.Allocate(1);
                if (!samplerAlloc.IsValid())
                {
                    LOG_ERROR("D3D12: Failed to allocate sampler descriptor");
                    return nullptr;
                }

                D3D12_SAMPLER_DESC d3dDesc = {};
                d3dDesc.Filter = ConvertFilter(desc);
                d3dDesc.AddressU = ConvertAddressMode(desc.addressU);
                d3dDesc.AddressV = ConvertAddressMode(desc.addressV);
                d3dDesc.AddressW = ConvertAddressMode(desc.addressW);
                d3dDesc.MipLODBias = desc.mipLodBias;
                d3dDesc.MaxAnisotropy = desc.maxAnisotropy;
                d3dDesc.ComparisonFunc = ConvertCompareOp(desc.compareOp);
                d3dDesc.BorderColor[0] = desc.borderColor[0];
                d3dDesc.BorderColor[1] = desc.borderColor[1];
                d3dDesc.BorderColor[2] = desc.borderColor[2];
                d3dDesc.BorderColor[3] = desc.borderColor[3];
                d3dDesc.MinLOD = desc.minLod;
                d3dDesc.MaxLOD = desc.maxLod;

                m_device->CreateSampler(&d3dDesc, samplerAlloc.cpuHandle);

                return new D3D12Sampler(desc, samplerAlloc);
            }

            IRHIPipelineState* D3D12Device::CreatePipelineState(const RHIPipelineStateDesc& desc,
                                                                IRHIShader* vertexShader, IRHIShader* pixelShader)
            {
                auto rootSignature = CreateDefaultRootSignature();
                if (!rootSignature)
                {
                    LOG_ERROR("D3D12: Failed to create root signature for PSO");
                    return nullptr;
                }

                D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
                psoDesc.pRootSignature = rootSignature.Get();

                // Shaders
                if (vertexShader)
                {
                    auto* vs = static_cast<D3D12Shader*>(vertexShader);
                    psoDesc.VS = vs->GetD3D12Bytecode();
                }
                if (pixelShader)
                {
                    auto* ps = static_cast<D3D12Shader*>(pixelShader);
                    psoDesc.PS = ps->GetD3D12Bytecode();
                }

                // Input layout
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
                psoDesc.InputLayout.pInputElementDescs = inputElements.data();
                psoDesc.InputLayout.NumElements = static_cast<UINT>(inputElements.size());

                // Rasterizer state
                psoDesc.RasterizerState.FillMode = (desc.rasterizer.fillMode == RHIFillMode::Wireframe)
                                                       ? D3D12_FILL_MODE_WIREFRAME
                                                       : D3D12_FILL_MODE_SOLID;
                switch (desc.rasterizer.cullMode)
                {
                case RHICullMode::None:
                    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                    break;
                case RHICullMode::Front:
                    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
                    break;
                case RHICullMode::Back:
                    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
                    break;
                }
                psoDesc.RasterizerState.FrontCounterClockwise = desc.rasterizer.frontCounterClockwise;
                psoDesc.RasterizerState.DepthBias = desc.rasterizer.depthBias;
                psoDesc.RasterizerState.DepthBiasClamp = desc.rasterizer.depthBiasClamp;
                psoDesc.RasterizerState.SlopeScaledDepthBias = desc.rasterizer.slopeScaledDepthBias;
                psoDesc.RasterizerState.DepthClipEnable = desc.rasterizer.depthClipEnable;
                psoDesc.RasterizerState.MultisampleEnable = desc.rasterizer.multisampleEnable;
                psoDesc.RasterizerState.AntialiasedLineEnable = desc.rasterizer.antialiasedLineEnable;
                psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

                // Blend state
                psoDesc.BlendState.AlphaToCoverageEnable = desc.blend.alphaToCoverageEnable;
                psoDesc.BlendState.IndependentBlendEnable = desc.blend.independentBlendEnable;
                for (uint32_t i = 0; i < 8; ++i)
                {
                    const auto& rt = desc.blend.renderTargets[i];
                    auto& d3dRT = psoDesc.BlendState.RenderTarget[i];
                    d3dRT.BlendEnable = rt.blendEnable;
                    d3dRT.SrcBlend = ConvertBlendFactor(rt.srcBlend);
                    d3dRT.DestBlend = ConvertBlendFactor(rt.dstBlend);
                    d3dRT.BlendOp = ConvertBlendOp(rt.blendOp);
                    d3dRT.SrcBlendAlpha = ConvertBlendFactor(rt.srcBlendAlpha);
                    d3dRT.DestBlendAlpha = ConvertBlendFactor(rt.dstBlendAlpha);
                    d3dRT.BlendOpAlpha = ConvertBlendOp(rt.blendOpAlpha);
                    d3dRT.RenderTargetWriteMask = rt.writeMask;
                }

                // Depth stencil state
                psoDesc.DepthStencilState.DepthEnable = desc.depthStencil.depthEnable;
                psoDesc.DepthStencilState.DepthWriteMask =
                    desc.depthStencil.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
                psoDesc.DepthStencilState.DepthFunc = ConvertCompareOp(desc.depthStencil.depthFunc);
                psoDesc.DepthStencilState.StencilEnable = desc.depthStencil.stencilEnable;
                psoDesc.DepthStencilState.StencilReadMask = desc.depthStencil.stencilReadMask;
                psoDesc.DepthStencilState.StencilWriteMask = desc.depthStencil.stencilWriteMask;

                psoDesc.DepthStencilState.FrontFace.StencilFailOp =
                    ConvertStencilOp(desc.depthStencil.frontFace.stencilFail);
                psoDesc.DepthStencilState.FrontFace.StencilDepthFailOp =
                    ConvertStencilOp(desc.depthStencil.frontFace.stencilDepthFail);
                psoDesc.DepthStencilState.FrontFace.StencilPassOp =
                    ConvertStencilOp(desc.depthStencil.frontFace.stencilPass);
                psoDesc.DepthStencilState.FrontFace.StencilFunc =
                    ConvertCompareOp(desc.depthStencil.frontFace.stencilFunc);

                psoDesc.DepthStencilState.BackFace.StencilFailOp =
                    ConvertStencilOp(desc.depthStencil.backFace.stencilFail);
                psoDesc.DepthStencilState.BackFace.StencilDepthFailOp =
                    ConvertStencilOp(desc.depthStencil.backFace.stencilDepthFail);
                psoDesc.DepthStencilState.BackFace.StencilPassOp =
                    ConvertStencilOp(desc.depthStencil.backFace.stencilPass);
                psoDesc.DepthStencilState.BackFace.StencilFunc =
                    ConvertCompareOp(desc.depthStencil.backFace.stencilFunc);

                // Primitive topology type
                switch (desc.topology)
                {
                case RHIPrimitiveTopology::PointList:
                    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
                    break;
                case RHIPrimitiveTopology::LineList:
                case RHIPrimitiveTopology::LineStrip:
                    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
                    break;
                case RHIPrimitiveTopology::PatchList:
                    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
                    break;
                default:
                    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    break;
                }

                // Render target formats
                psoDesc.NumRenderTargets = desc.numRenderTargets;
                for (uint32_t i = 0; i < desc.numRenderTargets; ++i)
                {
                    psoDesc.RTVFormats[i] = ConvertFormat(desc.renderTargetFormats[i]);
                }
                psoDesc.DSVFormat = ConvertFormat(desc.depthStencilFormat);
                psoDesc.SampleDesc.Count = desc.sampleCount;
                psoDesc.SampleDesc.Quality = 0;
                psoDesc.SampleMask = UINT_MAX;

                ComPtr<ID3D12PipelineState> pso;
                HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
                if (!CheckHR(hr, "CreateGraphicsPipelineState"))
                {
                    return nullptr;
                }

                if (!desc.debugName.empty())
                {
                    std::wstring wideName(desc.debugName.begin(), desc.debugName.end());
                    pso->SetName(wideName.c_str());
                }

                return new D3D12PipelineState(desc, std::move(pso), std::move(rootSignature));
            }

            // ================================================================
            // D3D12Device — Resource destruction
            // ================================================================

            void D3D12Device::DestroyBuffer(IRHIBuffer* buffer)
            {
                if (!buffer)
                {
                    return;
                }

                auto* d3d12Buffer = static_cast<D3D12Buffer*>(buffer);

                // Unmap if persistently mapped
                if (d3d12Buffer->GetMappedPointer())
                {
                    d3d12Buffer->GetD3D12Resource()->Unmap(0, nullptr);
                    d3d12Buffer->SetMappedPointer(nullptr);
                }

                // Free descriptor
                if (d3d12Buffer->GetDescriptor().IsValid())
                {
                    m_cbvSrvUavHeap.Free(d3d12Buffer->GetDescriptor());
                }

                // Defer the actual COM release until the GPU is done
                {
                    std::lock_guard<std::mutex> lock(m_deferredReleaseMutex);
                    if (d3d12Buffer->GetD3D12Resource())
                    {
                        m_deferredReleaseQueue.push({d3d12Buffer->GetD3D12Resource(), m_frameFence.GetCurrentValue()});
                    }
                    if (d3d12Buffer->GetUploadResource())
                    {
                        m_deferredReleaseQueue.push({d3d12Buffer->GetUploadResource(), m_frameFence.GetCurrentValue()});
                    }
                }

                delete d3d12Buffer;
            }

            void D3D12Device::DestroyTexture(IRHITexture* texture)
            {
                if (!texture)
                {
                    return;
                }

                auto* d3d12Texture = static_cast<D3D12Texture*>(texture);

                // Free descriptors
                if (d3d12Texture->GetSRVDescriptor().IsValid())
                {
                    m_cbvSrvUavHeap.Free(d3d12Texture->GetSRVDescriptor());
                }
                if (d3d12Texture->GetRTVDescriptor().IsValid())
                {
                    m_rtvHeap.Free(d3d12Texture->GetRTVDescriptor());
                }
                if (d3d12Texture->GetDSVDescriptor().IsValid())
                {
                    m_dsvHeap.Free(d3d12Texture->GetDSVDescriptor());
                }
                if (d3d12Texture->GetUAVDescriptor().IsValid())
                {
                    m_cbvSrvUavHeap.Free(d3d12Texture->GetUAVDescriptor());
                }

                {
                    std::lock_guard<std::mutex> lock(m_deferredReleaseMutex);
                    if (d3d12Texture->GetD3D12Resource())
                    {
                        m_deferredReleaseQueue.push({d3d12Texture->GetD3D12Resource(), m_frameFence.GetCurrentValue()});
                    }
                }

                delete d3d12Texture;
            }

            void D3D12Device::DestroyShader(IRHIShader* shader)
            {
                delete static_cast<D3D12Shader*>(shader);
            }

            void D3D12Device::DestroySampler(IRHISampler* sampler)
            {
                if (!sampler)
                {
                    return;
                }

                auto* d3d12Sampler = static_cast<D3D12Sampler*>(sampler);
                if (d3d12Sampler->GetDescriptor().IsValid())
                {
                    m_samplerHeap.Free(d3d12Sampler->GetDescriptor());
                }
                delete d3d12Sampler;
            }

            void D3D12Device::DestroyPipelineState(IRHIPipelineState* state)
            {
                if (!state)
                {
                    return;
                }

                auto* d3d12PSO = static_cast<D3D12PipelineState*>(state);
                {
                    std::lock_guard<std::mutex> lock(m_deferredReleaseMutex);
                    if (d3d12PSO->GetPSO())
                    {
                        m_deferredReleaseQueue.push({d3d12PSO->GetPSO(), m_frameFence.GetCurrentValue()});
                    }
                    if (d3d12PSO->GetRootSignature())
                    {
                        m_deferredReleaseQueue.push({d3d12PSO->GetRootSignature(), m_frameFence.GetCurrentValue()});
                    }
                }
                delete d3d12PSO;
            }

            // ================================================================
            // D3D12Device — Resource updates (Map / Unmap / Update)
            // ================================================================

            void* D3D12Device::MapBuffer(IRHIBuffer* buffer)
            {
                if (!buffer)
                {
                    return nullptr;
                }

                auto* d3d12Buffer = static_cast<D3D12Buffer*>(buffer);

                // Return the persistently mapped pointer for dynamic buffers
                if (d3d12Buffer->GetMappedPointer())
                {
                    return d3d12Buffer->GetMappedPointer();
                }

                void* mapped = nullptr;
                D3D12_RANGE readRange = {0, 0}; // We do not intend to read from this resource on CPU
                HRESULT hr = d3d12Buffer->GetD3D12Resource()->Map(0, &readRange, &mapped);
                if (!CheckHR(hr, "ID3D12Resource::Map"))
                {
                    return nullptr;
                }
                return mapped;
            }

            void D3D12Device::UnmapBuffer(IRHIBuffer* buffer)
            {
                if (!buffer)
                {
                    return;
                }

                auto* d3d12Buffer = static_cast<D3D12Buffer*>(buffer);

                // Do not unmap persistently mapped buffers
                if (d3d12Buffer->GetMappedPointer())
                {
                    return;
                }

                d3d12Buffer->GetD3D12Resource()->Unmap(0, nullptr);
            }

            void D3D12Device::UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset)
            {
                if (!buffer || !data)
                {
                    return;
                }

                auto* d3d12Buffer = static_cast<D3D12Buffer*>(buffer);

                if (d3d12Buffer->GetDesc().access == RHIBufferAccess::Dynamic ||
                    d3d12Buffer->GetDesc().access == RHIBufferAccess::Staging)
                {
                    // Directly memcpy into the persistently mapped pointer
                    void* mapped = d3d12Buffer->GetMappedPointer();
                    if (mapped)
                    {
                        memcpy(static_cast<uint8_t*>(mapped) + offset, data, size);
                    }
                }
                else
                {
                    // For static buffers, use the upload resource if available
                    ID3D12Resource* uploadRes = d3d12Buffer->GetUploadResource();
                    if (uploadRes)
                    {
                        void* mapped = nullptr;
                        uploadRes->Map(0, nullptr, &mapped);
                        memcpy(static_cast<uint8_t*>(mapped) + offset, data, size);
                        uploadRes->Unmap(0, nullptr);
                    }
                }
            }

            void D3D12Device::UpdateTexture(IRHITexture* texture, const void* data, uint32_t mipLevel,
                                            uint32_t arraySlice)
            {
                if (!texture || !data)
                {
                    return;
                }

                auto* d3d12Texture = static_cast<D3D12Texture*>(texture);
                const auto& desc = d3d12Texture->GetDesc();

                uint32_t subresource = D3D12CalcSubresource(mipLevel, arraySlice, 0, desc.mipLevels, desc.arraySize);

                uint32_t mipWidth = std::max(1u, desc.width >> mipLevel);
                uint32_t mipHeight = std::max(1u, desc.height >> mipLevel);
                uint32_t formatSize = GetFormatSize(desc.format);
                uint64_t rowPitch = static_cast<uint64_t>(mipWidth) * formatSize;
                rowPitch =
                    (rowPitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
                uint64_t totalSize = rowPitch * mipHeight;

                // Create a temporary upload buffer
                D3D12_HEAP_PROPERTIES uploadHeap = {};
                uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

                D3D12_RESOURCE_DESC uploadDesc = {};
                uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                uploadDesc.Width = totalSize;
                uploadDesc.Height = 1;
                uploadDesc.DepthOrArraySize = 1;
                uploadDesc.MipLevels = 1;
                uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
                uploadDesc.SampleDesc.Count = 1;
                uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                ComPtr<ID3D12Resource> uploadBuffer;
                HRESULT hr = m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                               IID_PPV_ARGS(&uploadBuffer));
                if (!CheckHR(hr, "CreateCommittedResource (texture upload)"))
                {
                    return;
                }

                // Copy data into the upload buffer with proper pitch alignment
                void* mapped = nullptr;
                uploadBuffer->Map(0, nullptr, &mapped);

                uint64_t srcRowPitch = static_cast<uint64_t>(mipWidth) * formatSize;
                for (uint32_t row = 0; row < mipHeight; ++row)
                {
                    memcpy(static_cast<uint8_t*>(mapped) + row * rowPitch,
                           static_cast<const uint8_t*>(data) + row * srcRowPitch, srcRowPitch);
                }
                uploadBuffer->Unmap(0, nullptr);

                // Use the immediate command list to copy
                m_immediateCommandList->Begin();

                D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
                srcLocation.pResource = uploadBuffer.Get();
                srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                srcLocation.PlacedFootprint.Footprint.Format = ConvertFormat(desc.format);
                srcLocation.PlacedFootprint.Footprint.Width = mipWidth;
                srcLocation.PlacedFootprint.Footprint.Height = mipHeight;
                srcLocation.PlacedFootprint.Footprint.Depth = 1;
                srcLocation.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);
                srcLocation.PlacedFootprint.Offset = 0;

                D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
                dstLocation.pResource = d3d12Texture->GetD3D12Resource();
                dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dstLocation.SubresourceIndex = subresource;

                m_immediateCommandList->TransitionBarrier(d3d12Texture, d3d12Texture->GetCurrentState(),
                                                          D3D12_RESOURCE_STATE_COPY_DEST);
                m_immediateCommandList->FlushBarriers();
                m_immediateCommandList->GetCommandList()->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation,
                                                                            nullptr);
                m_immediateCommandList->TransitionBarrier(d3d12Texture, D3D12_RESOURCE_STATE_COPY_DEST,
                                                          D3D12_RESOURCE_STATE_COMMON);
                m_immediateCommandList->End();

                ExecuteCommandList(m_immediateCommandList.get());

                // Defer release of upload buffer
                {
                    std::lock_guard<std::mutex> lock(m_deferredReleaseMutex);
                    m_deferredReleaseQueue.push({uploadBuffer.Get(), m_frameFence.GetCurrentValue()});
                }
            }

            // ================================================================
            // D3D12Device — Command list management
            // ================================================================

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
                if (!commandList)
                {
                    return;
                }

                auto* d3d12CmdList = static_cast<D3D12CommandList*>(commandList);
                ID3D12CommandList* lists[] = {d3d12CmdList->GetCommandList()};

                std::lock_guard<std::mutex> lock(m_submitMutex);
                m_directQueue->ExecuteCommandLists(1, lists);
            }

            void D3D12Device::DestroyCommandList(IRHICommandList* commandList)
            {
                if (commandList && commandList != m_immediateCommandList.get())
                {
                    delete static_cast<D3D12CommandList*>(commandList);
                }
            }

            // ================================================================
            // D3D12Device — Frame management
            // ================================================================

            void D3D12Device::BeginFrame()
            {
                // Wait for the frame resources we are about to reuse
                auto& frame = m_frameResources[m_currentFrameIndex];
                m_frameFence.WaitForValue(frame.fenceValue);

                ProcessDeferredReleases();
                ResetStatistics();

                // Reset the per-frame command allocator
                if (frame.commandAllocator)
                {
                    frame.commandAllocator->Reset();
                }
            }

            void D3D12Device::EndFrame()
            {
                // Signal the fence from the direct queue for this frame
                auto& frame = m_frameResources[m_currentFrameIndex];
                frame.fenceValue = m_frameFence.Signal(m_directQueue.Get());

                MoveToNextFrame();
            }

            void D3D12Device::WaitForIdle()
            {
                if (m_directQueue)
                {
                    m_frameFence.Signal(m_directQueue.Get());
                    m_frameFence.WaitForIdle();
                }
            }

            // ================================================================
            // D3D12Device — Statistics
            // ================================================================

            void D3D12Device::ResetStatistics()
            {
                m_statistics = {};
            }

            std::string D3D12Device::GetDeviceInfo() const
            {
                return std::format("D3D12 Device: {} | VRAM: {} MB | DXR: {}", m_capabilities.deviceName,
                                   m_capabilities.dedicatedVideoMemory / (1024 * 1024), m_dxrSupported ? "Yes" : "No");
            }

            // ================================================================
            // D3D12Device — Private helpers: device creation
            // ================================================================

            bool D3D12Device::CreateDevice(const RHIDeviceDesc& desc)
            {
                UINT dxgiFlags = 0;
                if (desc.enableDebugLayer)
                {
                    dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
                }

                HRESULT hr = CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&m_dxgiFactory));
                if (!CheckHR(hr, "CreateDXGIFactory2"))
                {
                    return false;
                }

                // Enumerate adapters and pick the first hardware adapter that supports D3D12
                ComPtr<IDXGIAdapter1> adapter;
                for (UINT i = 0; m_dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
                {
                    DXGI_ADAPTER_DESC1 adapterDesc = {};
                    adapter->GetDesc1(&adapterDesc);

                    // Skip software adapters
                    if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                    {
                        continue;
                    }

                    // Check D3D12 support
                    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr);
                    if (SUCCEEDED(hr))
                    {
                        m_adapter = adapter;
                        break;
                    }
                }

                if (!m_adapter)
                {
                    LOG_ERROR("D3D12: No suitable hardware adapter found");
                    return false;
                }

                hr = D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
                if (!CheckHR(hr, "D3D12CreateDevice"))
                {
                    return false;
                }

                // Set up info queue for debug messages
                if (desc.enableDebugLayer)
                {
                    if (SUCCEEDED(m_device.As(&m_infoQueue)))
                    {
                        m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
                        m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);

                        // Suppress some common non-critical warnings
                        D3D12_MESSAGE_ID suppressedIds[] = {
                            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                            D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
                        };

                        D3D12_INFO_QUEUE_FILTER filter = {};
                        filter.DenyList.NumIDs = _countof(suppressedIds);
                        filter.DenyList.pIDList = suppressedIds;
                        m_infoQueue->AddStorageFilterEntries(&filter);
                    }
                }

                return true;
            }

            bool D3D12Device::CreateCommandQueues()
            {
                // Direct (graphics) queue
                D3D12_COMMAND_QUEUE_DESC directQueueDesc = {};
                directQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                directQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
                directQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
                directQueueDesc.NodeMask = 0;

                HRESULT hr = m_device->CreateCommandQueue(&directQueueDesc, IID_PPV_ARGS(&m_directQueue));
                if (!CheckHR(hr, "CreateCommandQueue (Direct)"))
                {
                    return false;
                }

                // Copy queue
                D3D12_COMMAND_QUEUE_DESC copyQueueDesc = directQueueDesc;
                copyQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
                hr = m_device->CreateCommandQueue(&copyQueueDesc, IID_PPV_ARGS(&m_copyQueue));
                if (!CheckHR(hr, "CreateCommandQueue (Copy)"))
                {
                    return false;
                }

                // Compute queue
                D3D12_COMMAND_QUEUE_DESC computeQueueDesc = directQueueDesc;
                computeQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
                hr = m_device->CreateCommandQueue(&computeQueueDesc, IID_PPV_ARGS(&m_computeQueue));
                if (!CheckHR(hr, "CreateCommandQueue (Compute)"))
                {
                    return false;
                }

                return true;
            }

            bool D3D12Device::CreateDescriptorHeaps()
            {
                if (!m_cbvSrvUavHeap.Initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                CBV_SRV_UAV_HEAP_SIZE, true))
                {
                    return false;
                }
                if (!m_rtvHeap.Initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_HEAP_SIZE, false))
                {
                    return false;
                }
                if (!m_dsvHeap.Initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_HEAP_SIZE, false))
                {
                    return false;
                }
                if (!m_samplerHeap.Initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_HEAP_SIZE,
                                              true))
                {
                    return false;
                }
                return true;
            }

            bool D3D12Device::CreateFrameResources()
            {
                for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
                {
                    HRESULT hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                  IID_PPV_ARGS(&m_frameResources[i].commandAllocator));
                    if (!CheckHR(hr, "CreateCommandAllocator (frame)"))
                    {
                        return false;
                    }
                    m_frameResources[i].fenceValue = 0;
                }
                return true;
            }

            void D3D12Device::DetectCapabilities()
            {
                DXGI_ADAPTER_DESC1 adapterDesc = {};
                m_adapter->GetDesc1(&adapterDesc);

                // Convert wide string device name to narrow string
                char deviceName[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, deviceName, sizeof(deviceName), nullptr,
                                    nullptr);
                m_capabilities.deviceName = deviceName;

                // Vendor name from vendor ID
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

                m_capabilities.backend = GraphicsBackend::D3D12;
                m_capabilities.dedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
                m_capabilities.sharedSystemMemory = adapterDesc.SharedSystemMemory;
                m_capabilities.apiVersion = "12.0";

                // Query feature support
                D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
                if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
                {
                    m_capabilities.conservativeRasterSupport =
                        (options.ConservativeRasterizationTier != D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED);
                }

                D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
                if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
                {
                    m_capabilities.meshShaderSupport =
                        (options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED);
                }

                m_capabilities.tessellationSupport = true;
                m_capabilities.computeShaderSupport = true;
                m_capabilities.geometryShaderSupport = true;
                m_capabilities.multiDrawIndirectSupport = true;
                m_capabilities.bindlessResourceSupport = true;

                m_capabilities.maxTextureSize = 16384;
                m_capabilities.maxRenderTargets = 8;
                m_capabilities.maxSamplers = 16;
                m_capabilities.maxConstantBuffers = 14;
                m_capabilities.maxVertexAttributes = 32;
                m_capabilities.maxMSAASamples = 8;
                m_capabilities.maxAnisotropy = 16.0f;
            }

            void D3D12Device::DetectDXRSupport()
            {
                m_dxrSupported = false;

                D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
                if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
                {
                    if (options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
                    {
                        HRESULT hr = m_device.As(&m_dxrDevice);
                        if (SUCCEEDED(hr))
                        {
                            m_dxrSupported = true;
                            m_capabilities.rayTracingSupport = true;
                            LOG_INFO("D3D12: DXR support detected (Tier {})",
                                     static_cast<int>(options5.RaytracingTier));
                        }
                    }
                }
            }

            // ================================================================
            // D3D12Device — Format converters
            // ================================================================

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
                    return DXGI_FORMAT_UNKNOWN;
                }
            }

            D3D12_FILTER D3D12Device::ConvertFilter(const RHISamplerDesc& desc) const
            {
                bool isAnisotropic =
                    (desc.minFilter == RHIFilterMode::Anisotropic || desc.magFilter == RHIFilterMode::Anisotropic);
                if (isAnisotropic)
                {
                    return (desc.compareOp != RHICompareOp::Never) ? D3D12_FILTER_COMPARISON_ANISOTROPIC
                                                                   : D3D12_FILTER_ANISOTROPIC;
                }

                bool isComparison = (desc.compareOp != RHICompareOp::Never);

                bool minLinear = (desc.minFilter == RHIFilterMode::Linear);
                bool magLinear = (desc.magFilter == RHIFilterMode::Linear);
                bool mipLinear = (desc.mipFilter == RHIFilterMode::Linear);

                // Encode as D3D12 filter bits
                D3D12_FILTER_TYPE minType = minLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
                D3D12_FILTER_TYPE magType = magLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
                D3D12_FILTER_TYPE mipType = mipLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;

                D3D12_FILTER_REDUCTION_TYPE reduction =
                    isComparison ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;

                return D3D12_ENCODE_BASIC_FILTER(minType, magType, mipType, reduction);
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
                    return D3D12_COMPARISON_FUNC_NEVER;
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
                    return D3D12_BLEND_ZERO;
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
                    return DXGI_FORMAT_UNKNOWN;
                }
            }

            uint32_t D3D12Device::GetFormatSize(PixelFormat format) const
            {
                switch (format)
                {
                case PixelFormat::R8_UNORM:
                case PixelFormat::R8_SNORM:
                case PixelFormat::R8_UINT:
                    return 1;
                case PixelFormat::R8G8_UNORM:
                case PixelFormat::R16_FLOAT:
                case PixelFormat::R16_UINT:
                case PixelFormat::D16_UNORM:
                    return 2;
                case PixelFormat::R8G8B8A8_UNORM:
                case PixelFormat::R8G8B8A8_UNORM_SRGB:
                case PixelFormat::R8G8B8A8_SNORM:
                case PixelFormat::B8G8R8A8_UNORM:
                case PixelFormat::B8G8R8A8_UNORM_SRGB:
                case PixelFormat::R10G10B10A2_UNORM:
                case PixelFormat::R11G11B10_FLOAT:
                case PixelFormat::R16G16_FLOAT:
                case PixelFormat::R32_FLOAT:
                case PixelFormat::R32_UINT:
                case PixelFormat::D24_UNORM_S8_UINT:
                case PixelFormat::D32_FLOAT:
                    return 4;
                case PixelFormat::R16G16B16A16_FLOAT:
                case PixelFormat::R16G16B16A16_UNORM:
                case PixelFormat::R32G32_FLOAT:
                case PixelFormat::D32_FLOAT_S8_UINT:
                    return 8;
                case PixelFormat::R32G32B32_FLOAT:
                    return 12;
                case PixelFormat::R32G32B32A32_FLOAT:
                    return 16;
                default:
                    return 4; // Reasonable default
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

            // ================================================================
            // D3D12Device — Frame management helpers
            // ================================================================

            void D3D12Device::MoveToNextFrame()
            {
                m_currentFrameIndex = (m_currentFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
            }

            void D3D12Device::ProcessDeferredReleases()
            {
                std::lock_guard<std::mutex> lock(m_deferredReleaseMutex);
                uint64_t completedValue = m_frameFence.GetCompletedValue();

                while (!m_deferredReleaseQueue.empty())
                {
                    auto& front = m_deferredReleaseQueue.front();
                    if (front.fenceValue <= completedValue)
                    {
                        front.resource.Reset();
                        m_deferredReleaseQueue.pop();
                    }
                    else
                    {
                        break; // Remaining entries have higher fence values
                    }
                }
            }

            // ================================================================
            // D3D12Device — Root signature helpers
            // ================================================================

            ComPtr<ID3D12RootSignature> D3D12Device::CreateDefaultRootSignature() const
            {
                // Root parameter 0: CBV descriptor table (b0-b13, all stages)
                D3D12_DESCRIPTOR_RANGE1 cbvRange = {};
                cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                cbvRange.NumDescriptors = 14;
                cbvRange.BaseShaderRegister = 0;
                cbvRange.RegisterSpace = 0;
                cbvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
                cbvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

                // Root parameter 1: SRV descriptor table (t0-t31, PS)
                D3D12_DESCRIPTOR_RANGE1 srvRange = {};
                srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                srvRange.NumDescriptors = 32;
                srvRange.BaseShaderRegister = 0;
                srvRange.RegisterSpace = 0;
                srvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
                srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

                // Root parameter 2: Sampler descriptor table (s0-s15, PS)
                D3D12_DESCRIPTOR_RANGE1 samplerRange = {};
                samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                samplerRange.NumDescriptors = 16;
                samplerRange.BaseShaderRegister = 0;
                samplerRange.RegisterSpace = 0;
                samplerRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
                samplerRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

                // Root parameter 3: UAV descriptor table (u0-u7, all stages)
                D3D12_DESCRIPTOR_RANGE1 uavRange = {};
                uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                uavRange.NumDescriptors = 8;
                uavRange.BaseShaderRegister = 0;
                uavRange.RegisterSpace = 0;
                uavRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
                uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

                D3D12_ROOT_PARAMETER1 rootParams[4] = {};

                // CBV table — all stages
                rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
                rootParams[0].DescriptorTable.pDescriptorRanges = &cbvRange;
                rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

                // SRV table — pixel shader
                rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
                rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
                rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

                // Sampler table — pixel shader
                rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
                rootParams[2].DescriptorTable.pDescriptorRanges = &samplerRange;
                rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

                // UAV table — all stages
                rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
                rootParams[3].DescriptorTable.pDescriptorRanges = &uavRange;
                rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

                D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
                rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
                rootSigDesc.Desc_1_1.NumParameters = _countof(rootParams);
                rootSigDesc.Desc_1_1.pParameters = rootParams;
                rootSigDesc.Desc_1_1.NumStaticSamplers = 0;
                rootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
                rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

                ComPtr<ID3DBlob> serializedBlob;
                ComPtr<ID3DBlob> errorBlob;
                HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &serializedBlob, &errorBlob);
                if (!CheckHR(hr, "D3D12SerializeVersionedRootSignature"))
                {
                    if (errorBlob)
                    {
                        LOG_ERROR("D3D12: Root signature error: {}",
                                  static_cast<const char*>(errorBlob->GetBufferPointer()));
                    }
                    return nullptr;
                }

                return CreateRootSignature(serializedBlob->GetBufferPointer(), serializedBlob->GetBufferSize());
            }

            ComPtr<ID3D12RootSignature> D3D12Device::CreateRootSignature(const void* data, size_t dataSize) const
            {
                ComPtr<ID3D12RootSignature> rootSignature;
                HRESULT hr = m_device->CreateRootSignature(0, data, dataSize, IID_PPV_ARGS(&rootSignature));
                if (!CheckHR(hr, "CreateRootSignature"))
                {
                    return nullptr;
                }
                return rootSignature;
            }

        } // namespace D3D12
    } // namespace RHI
} // namespace Spark

#endif // _WIN32
