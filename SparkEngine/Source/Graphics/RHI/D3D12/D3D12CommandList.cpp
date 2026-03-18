/**
 * @file D3D12CommandList.cpp
 * @brief D3D12SwapChain and D3D12CommandList implementations
 *
 * Split from D3D12Device.cpp for maintainability.
 */

#ifdef _WIN32

#include "D3D12Device.h"
#include <iostream>

namespace Spark
{
    namespace RHI
    {
        namespace D3D12
        {

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
                HRESULT hr = device->CreateCommandAllocator(type, IID_PPV_ARGS(&m_commandAllocator));
                if (FAILED(hr))
                    return;
                hr = device->CreateCommandList(0, type, m_commandAllocator.Get(), initialPSO,
                                               IID_PPV_ARGS(&m_commandList));
                if (FAILED(hr))
                    return;
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

            void D3D12CommandList::CopyTexture(IRHITexture* dst, IRHITexture* src)
            {
                if (!dst || !src)
                    return;
                FlushBarriers();
                auto* d3dDst = static_cast<D3D12Texture*>(dst);
                auto* d3dSrc = static_cast<D3D12Texture*>(src);
                m_commandList->CopyResource(d3dDst->GetD3D12Resource(), d3dSrc->GetD3D12Resource());
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


        } // namespace D3D12
    } // namespace RHI
} // namespace Spark

#endif // _WIN32
