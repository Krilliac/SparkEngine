#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file D3D11Device.cpp
 * @brief DirectX 11 RHI backend implementation
 * @author Spark Engine Team
 * @date 2025
 *
 * RHI Ownership Model: Create*() methods return raw pointers. The RHI device
 * owns the underlying GPU resource. Callers must call the corresponding
 * Destroy*() method to release. This pattern is intentional — it matches
 * the D3D11/D3D12/Vulkan/OpenGL resource lifecycle and avoids forcing
 * std::unique_ptr across the backend-agnostic RHI boundary.
 */

#include "D3D11Device.h"
#include "../RHIFormatUtils.h"
#include "../../../Utils/Validate.h"
#include <algorithm>
#include <cassert>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

namespace Spark
{
    namespace RHI
    {
        namespace D3D11
        {

            // ============================================================================
            // D3D11 BUFFER
            // ============================================================================

            D3D11Buffer::D3D11Buffer(const RHIBufferDesc& desc, ComPtr<ID3D11Buffer> buffer,
                                     ComPtr<ID3D11ShaderResourceView> srv, ComPtr<ID3D11UnorderedAccessView> uav)
                : m_desc(desc), m_buffer(std::move(buffer)), m_srv(std::move(srv)), m_uav(std::move(uav))
            {
                Spark::Graphics::RHIValidationLayer::GetInstance().TrackResource(
                    reinterpret_cast<uint64_t>(m_buffer.Get()), m_desc.debugName, "Buffer");
            }

            D3D11Buffer::~D3D11Buffer()
            {
                Spark::Graphics::RHIValidationLayer::GetInstance().UntrackResource(
                    reinterpret_cast<uint64_t>(m_buffer.Get()));
            }

            // ============================================================================
            // D3D11 TEXTURE
            // ============================================================================

            D3D11Texture::D3D11Texture(const RHITextureDesc& desc, ComPtr<ID3D11Resource> resource,
                                       ComPtr<ID3D11ShaderResourceView> srv, ComPtr<ID3D11RenderTargetView> rtv,
                                       ComPtr<ID3D11DepthStencilView> dsv)
                : m_desc(desc), m_resource(std::move(resource)), m_srv(std::move(srv)), m_rtv(std::move(rtv)),
                  m_dsv(std::move(dsv))
            {
                Spark::Graphics::RHIValidationLayer::GetInstance().TrackResource(
                    reinterpret_cast<uint64_t>(m_resource.Get()), m_desc.debugName, "Texture2D");
            }

            D3D11Texture::~D3D11Texture()
            {
                Spark::Graphics::RHIValidationLayer::GetInstance().UntrackResource(
                    reinterpret_cast<uint64_t>(m_resource.Get()));
            }

            // ============================================================================
            // D3D11 SHADER
            // ============================================================================

            D3D11Shader::D3D11Shader(const RHIShaderDesc& desc, ComPtr<ID3D11DeviceChild> shader,
                                     ComPtr<ID3DBlob> bytecodeBlob)
                : m_desc(desc), m_shader(std::move(shader)), m_bytecodeBlob(std::move(bytecodeBlob))
            {
            }

            const void* D3D11Shader::GetBytecode() const
            {
                return m_bytecodeBlob ? m_bytecodeBlob->GetBufferPointer() : nullptr;
            }

            size_t D3D11Shader::GetBytecodeSize() const
            {
                return m_bytecodeBlob ? m_bytecodeBlob->GetBufferSize() : 0;
            }

            ID3D11VertexShader* D3D11Shader::GetVertexShader() const
            {
                if (m_desc.stage != RHIShaderStage::Vertex)
                    return nullptr;
                ComPtr<ID3D11VertexShader> vs;
                m_shader.As(&vs);
                return vs.Get();
            }

            ID3D11PixelShader* D3D11Shader::GetPixelShader() const
            {
                if (m_desc.stage != RHIShaderStage::Pixel)
                    return nullptr;
                ComPtr<ID3D11PixelShader> ps;
                m_shader.As(&ps);
                return ps.Get();
            }

            ID3D11GeometryShader* D3D11Shader::GetGeometryShader() const
            {
                if (m_desc.stage != RHIShaderStage::Geometry)
                    return nullptr;
                ComPtr<ID3D11GeometryShader> gs;
                m_shader.As(&gs);
                return gs.Get();
            }

            ID3D11HullShader* D3D11Shader::GetHullShader() const
            {
                if (m_desc.stage != RHIShaderStage::Hull)
                    return nullptr;
                ComPtr<ID3D11HullShader> hs;
                m_shader.As(&hs);
                return hs.Get();
            }

            ID3D11DomainShader* D3D11Shader::GetDomainShader() const
            {
                if (m_desc.stage != RHIShaderStage::Domain)
                    return nullptr;
                ComPtr<ID3D11DomainShader> ds;
                m_shader.As(&ds);
                return ds.Get();
            }

            ID3D11ComputeShader* D3D11Shader::GetComputeShader() const
            {
                if (m_desc.stage != RHIShaderStage::Compute)
                    return nullptr;
                ComPtr<ID3D11ComputeShader> cs;
                m_shader.As(&cs);
                return cs.Get();
            }

            // ============================================================================
            // D3D11 SAMPLER
            // ============================================================================

            D3D11Sampler::D3D11Sampler(const RHISamplerDesc& desc, ComPtr<ID3D11SamplerState> sampler)
                : m_desc(desc), m_sampler(std::move(sampler))
            {
            }

            // ============================================================================
            // D3D11 PIPELINE STATE
            // ============================================================================

            D3D11PipelineState::D3D11PipelineState(const RHIPipelineStateDesc& desc,
                                                   ComPtr<ID3D11InputLayout> inputLayout,
                                                   ComPtr<ID3D11RasterizerState> rasterizerState,
                                                   ComPtr<ID3D11DepthStencilState> depthStencilState,
                                                   ComPtr<ID3D11BlendState> blendState, D3D11Shader* vs,
                                                   D3D11Shader* ps)
                : m_desc(desc), m_inputLayout(std::move(inputLayout)), m_rasterizerState(std::move(rasterizerState)),
                  m_depthStencilState(std::move(depthStencilState)), m_blendState(std::move(blendState)),
                  m_vertexShader(vs), m_pixelShader(ps)
            {
            }

            // ============================================================================
            // D3D11 SWAP CHAIN
            // ============================================================================

            D3D11SwapChain::D3D11SwapChain(ID3D11Device* device, const RHISwapChainDesc& desc)
                : m_desc(desc), m_device(device)
            {
                ComPtr<IDXGIDevice> dxgiDevice;
                HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), &dxgiDevice);
                if (FAILED(hr))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "D3D11SwapChain: QueryInterface(IDXGIDevice) failed (HRESULT 0x%08lX)", hr);
                    return;
                }

                ComPtr<IDXGIAdapter> dxgiAdapter;
                hr = dxgiDevice->GetAdapter(&dxgiAdapter);
                if (FAILED(hr))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D11SwapChain: GetAdapter failed (HRESULT 0x%08lX)",
                                    hr);
                    return;
                }

                ComPtr<IDXGIFactory2> dxgiFactory;
                hr = dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), &dxgiFactory);
                if (FAILED(hr))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "D3D11SwapChain: GetParent(IDXGIFactory2) failed (HRESULT 0x%08lX)", hr);
                    return;
                }

                HWND hwnd = static_cast<HWND>(desc.windowHandle);

                // Try swapchain creation with fallback configurations
                struct SwapChainFallback
                {
                    DXGI_FORMAT format;
                    DXGI_SWAP_EFFECT swapEffect;
                    UINT sampleCount;
                    const char* description;
                };

                const SwapChainFallback fallbacks[] = {
                    {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_EFFECT_FLIP_DISCARD, desc.sampleCount, "preferred"},
                    {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_EFFECT_FLIP_DISCARD, desc.sampleCount, "BGRA format"},
                    {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_EFFECT_DISCARD, 1, "legacy swap effect"},
                    {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_EFFECT_DISCARD, 1, "BGRA + legacy swap"},
                };

                for (const auto& fb : fallbacks)
                {
                    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
                    swapChainDesc.Width = desc.width;
                    swapChainDesc.Height = desc.height;
                    swapChainDesc.Format = fb.format;
                    swapChainDesc.SampleDesc.Count = fb.sampleCount;
                    swapChainDesc.SampleDesc.Quality = 0;
                    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                    swapChainDesc.BufferCount = desc.bufferCount;
                    swapChainDesc.SwapEffect = fb.swapEffect;

                    hr = dxgiFactory->CreateSwapChainForHwnd(device, hwnd, &swapChainDesc, nullptr, nullptr,
                                                             &m_swapChain);
                    if (SUCCEEDED(hr))
                    {
                        if (fb.format != DXGI_FORMAT_R8G8B8A8_UNORM || fb.swapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                            fb.sampleCount != desc.sampleCount)
                        {
                            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                           "D3D11SwapChain: created with fallback config (%s)", fb.description);
                        }
                        break;
                    }

                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "D3D11SwapChain: %s config failed (HRESULT 0x%08lX) — trying next fallback",
                                   fb.description, hr);
                }

                if (!m_swapChain)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "D3D11SwapChain: all swapchain configurations failed (last HRESULT 0x%08lX)", hr);
                    return;
                }

                CreateBackBufferViews();
            }

            D3D11SwapChain::~D3D11SwapChain()
            {
                m_backBuffer.reset();
            }

            bool D3D11SwapChain::Present(bool vsync)
            {
                if (!m_swapChain)
                    return false;
                HRESULT hr = m_swapChain->Present(vsync ? 1 : 0, 0);
                return SUCCEEDED(hr);
            }

            bool D3D11SwapChain::Resize(uint32_t width, uint32_t height)
            {
                if (!m_swapChain)
                    return false;

                m_backBuffer.reset();
                m_desc.width = width;
                m_desc.height = height;

                HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
                if (FAILED(hr))
                    return false;

                return CreateBackBufferViews();
            }

            IRHITexture* D3D11SwapChain::GetBackBuffer()
            {
                return m_backBuffer.get();
            }

            bool D3D11SwapChain::CreateBackBufferViews()
            {
                if (!m_swapChain || !m_device)
                    return false;

                ComPtr<ID3D11Texture2D> backBufferTex;
                HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBufferTex);
                if (FAILED(hr))
                    return false;

                ComPtr<ID3D11RenderTargetView> rtv;
                hr = m_device->CreateRenderTargetView(backBufferTex.Get(), nullptr, &rtv);
                if (FAILED(hr))
                    return false;

                RHITextureDesc desc;
                desc.width = m_desc.width;
                desc.height = m_desc.height;
                desc.format = m_desc.format;
                desc.usage = RHITextureUsage::RenderTarget;
                desc.debugName = "BackBuffer";

                m_backBuffer = std::make_unique<D3D11Texture>(desc, backBufferTex, nullptr, rtv, nullptr);
                return true;
            }

            // ============================================================================
            // D3D11 COMMAND LIST
            // ============================================================================

            D3D11CommandList::D3D11CommandList(ID3D11DeviceContext* context, bool isImmediate)
                : m_context(context), m_isImmediate(isImmediate)
            {
            }

            D3D11CommandList::D3D11CommandList(ComPtr<ID3D11DeviceContext> deferredContext)
                : m_ownedContext(std::move(deferredContext)), m_context(m_ownedContext.Get()), m_isImmediate(false)
            {
            }

            void D3D11CommandList::Begin()
            {
                m_recordedCommands.Reset();
                m_currentPipeline = nullptr;
            }

            void D3D11CommandList::End()
            {
                if (m_isImmediate || !m_context)
                    return;

                // Deferred contexts must be finished into an ID3D11CommandList before
                // D3D11Device::ExecuteCommandList can replay them on the immediate context.
                m_recordedCommands.Reset();
                HRESULT hr = m_context->FinishCommandList(FALSE, &m_recordedCommands);
                if (FAILED(hr))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "D3D11CommandList::End: FinishCommandList failed (HRESULT 0x%08lX)", hr);
                    m_recordedCommands.Reset();
                }
            }

            void D3D11CommandList::Reset()
            {
                m_recordedCommands.Reset();
                m_currentPipeline = nullptr;
            }

            void D3D11CommandList::SetRenderTargets(IRHITexture* const* renderTargets, uint32_t count,
                                                    IRHITexture* depthStencil)
            {
                ID3D11RenderTargetView* rtvs[8] = {};
                for (uint32_t i = 0; i < count && i < 8; ++i)
                {
                    if (renderTargets[i])
                    {
                        auto* d3dTex = static_cast<D3D11Texture*>(renderTargets[i]);
                        rtvs[i] = d3dTex->GetD3D11RTV();
                    }
                }

                ID3D11DepthStencilView* dsv = nullptr;
                if (depthStencil)
                {
                    auto* d3dDS = static_cast<D3D11Texture*>(depthStencil);
                    dsv = d3dDS->GetD3D11DSV();
                }

                m_context->OMSetRenderTargets(count, rtvs, dsv);
            }

            void D3D11CommandList::ClearRenderTarget(IRHITexture* target, const float color[4])
            {
                if (!target)
                    return;
                auto* d3dTex = static_cast<D3D11Texture*>(target);
                if (d3dTex->GetD3D11RTV())
                    m_context->ClearRenderTargetView(d3dTex->GetD3D11RTV(), color);
            }

            void D3D11CommandList::ClearDepthStencil(IRHITexture* target, float depth, uint8_t stencil)
            {
                if (!target)
                    return;
                auto* d3dTex = static_cast<D3D11Texture*>(target);
                if (d3dTex->GetD3D11DSV())
                    m_context->ClearDepthStencilView(d3dTex->GetD3D11DSV(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                                     depth, stencil);
            }

            void D3D11CommandList::SetViewport(const RHIViewport& viewport)
            {
                D3D11_VIEWPORT vp;
                vp.TopLeftX = viewport.x;
                vp.TopLeftY = viewport.y;
                vp.Width = viewport.width;
                vp.Height = viewport.height;
                vp.MinDepth = viewport.minDepth;
                vp.MaxDepth = viewport.maxDepth;
                m_context->RSSetViewports(1, &vp);
            }

            void D3D11CommandList::SetScissorRect(const RHIScissorRect& rect)
            {
                D3D11_RECT r;
                r.left = rect.left;
                r.top = rect.top;
                r.right = rect.right;
                r.bottom = rect.bottom;
                m_context->RSSetScissorRects(1, &r);
            }

            void D3D11CommandList::SetPipelineState(IRHIPipelineState* pipelineState)
            {
                if (!pipelineState || pipelineState == m_currentPipeline)
                    return; // Skip redundant pipeline bind (saves 7 D3D11 state calls)
                m_currentPipeline = pipelineState;
                auto* d3dPSO = static_cast<D3D11PipelineState*>(pipelineState);

                m_context->IASetInputLayout(d3dPSO->GetInputLayout());
                m_context->RSSetState(d3dPSO->GetRasterizerState());
                m_context->OMSetDepthStencilState(d3dPSO->GetDepthStencilState(), 0);

                float blendFactor[4] = {0, 0, 0, 0};
                m_context->OMSetBlendState(d3dPSO->GetBlendState(), blendFactor, 0xFFFFFFFF);

                if (d3dPSO->GetVertexShader())
                {
                    auto* vs = d3dPSO->GetVertexShader()->GetVertexShader();
                    m_context->VSSetShader(vs, nullptr, 0);
                }
                if (d3dPSO->GetPixelShader())
                {
                    auto* ps = d3dPSO->GetPixelShader()->GetPixelShader();
                    m_context->PSSetShader(ps, nullptr, 0);
                }
            }

            void D3D11CommandList::SetPrimitiveTopology(RHIPrimitiveTopology topology)
            {
                static const D3D11_PRIMITIVE_TOPOLOGY d3dTopology[] = {
                    D3D11_PRIMITIVE_TOPOLOGY_POINTLIST,     D3D11_PRIMITIVE_TOPOLOGY_LINELIST,
                    D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,     D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED};
                m_context->IASetPrimitiveTopology(d3dTopology[static_cast<int>(topology)]);
            }

            void D3D11CommandList::SetVertexBuffer(IRHIBuffer* buffer, uint32_t slot, uint32_t offset)
            {
                if (!buffer)
                    return;
                auto* d3dBuf = static_cast<D3D11Buffer*>(buffer);
                ID3D11Buffer* buf = d3dBuf->GetD3D11Buffer();
                UINT stride = d3dBuf->GetStride();
                UINT off = offset;
                m_context->IASetVertexBuffers(slot, 1, &buf, &stride, &off);
            }

            void D3D11CommandList::SetIndexBuffer(IRHIBuffer* buffer, uint32_t offset)
            {
                if (!buffer)
                    return;
                auto* d3dBuf = static_cast<D3D11Buffer*>(buffer);
                DXGI_FORMAT fmt = (d3dBuf->GetStride() == 4) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
                m_context->IASetIndexBuffer(d3dBuf->GetD3D11Buffer(), fmt, offset);
            }

            void D3D11CommandList::SetConstantBuffer(RHIShaderStage stage, uint32_t slot, IRHIBuffer* buffer)
            {
                if (!buffer)
                    return;
                auto* d3dBuf = static_cast<D3D11Buffer*>(buffer);
                ID3D11Buffer* buf = d3dBuf->GetD3D11Buffer();

                switch (stage)
                {
                case RHIShaderStage::Vertex:
                    m_context->VSSetConstantBuffers(slot, 1, &buf);
                    break;
                case RHIShaderStage::Pixel:
                    m_context->PSSetConstantBuffers(slot, 1, &buf);
                    break;
                case RHIShaderStage::Geometry:
                    m_context->GSSetConstantBuffers(slot, 1, &buf);
                    break;
                case RHIShaderStage::Hull:
                    m_context->HSSetConstantBuffers(slot, 1, &buf);
                    break;
                case RHIShaderStage::Domain:
                    m_context->DSSetConstantBuffers(slot, 1, &buf);
                    break;
                case RHIShaderStage::Compute:
                    m_context->CSSetConstantBuffers(slot, 1, &buf);
                    break;
                default:
                    break;
                }
            }

            void D3D11CommandList::SetShaderResource(RHIShaderStage stage, uint32_t slot, IRHITexture* texture)
            {
                ID3D11ShaderResourceView* srv = nullptr;
                if (texture)
                {
                    auto* d3dTex = static_cast<D3D11Texture*>(texture);
                    srv = d3dTex->GetD3D11SRV();
                }

                switch (stage)
                {
                case RHIShaderStage::Vertex:
                    m_context->VSSetShaderResources(slot, 1, &srv);
                    break;
                case RHIShaderStage::Pixel:
                    m_context->PSSetShaderResources(slot, 1, &srv);
                    break;
                case RHIShaderStage::Geometry:
                    m_context->GSSetShaderResources(slot, 1, &srv);
                    break;
                case RHIShaderStage::Hull:
                    m_context->HSSetShaderResources(slot, 1, &srv);
                    break;
                case RHIShaderStage::Domain:
                    m_context->DSSetShaderResources(slot, 1, &srv);
                    break;
                case RHIShaderStage::Compute:
                    m_context->CSSetShaderResources(slot, 1, &srv);
                    break;
                default:
                    break;
                }
            }

            void D3D11CommandList::SetSampler(RHIShaderStage stage, uint32_t slot, IRHISampler* sampler)
            {
                ID3D11SamplerState* ss = nullptr;
                if (sampler)
                {
                    auto* d3dSamp = static_cast<D3D11Sampler*>(sampler);
                    ss = d3dSamp->GetD3D11Sampler();
                }

                switch (stage)
                {
                case RHIShaderStage::Vertex:
                    m_context->VSSetSamplers(slot, 1, &ss);
                    break;
                case RHIShaderStage::Pixel:
                    m_context->PSSetSamplers(slot, 1, &ss);
                    break;
                case RHIShaderStage::Geometry:
                    m_context->GSSetSamplers(slot, 1, &ss);
                    break;
                case RHIShaderStage::Hull:
                    m_context->HSSetSamplers(slot, 1, &ss);
                    break;
                case RHIShaderStage::Domain:
                    m_context->DSSetSamplers(slot, 1, &ss);
                    break;
                case RHIShaderStage::Compute:
                    m_context->CSSetSamplers(slot, 1, &ss);
                    break;
                default:
                    break;
                }
            }

            void D3D11CommandList::Draw(uint32_t vertexCount, uint32_t startVertex)
            {
                // Debug builds only — the release build of RHIValidationLayer is an
                // inline no-op, so this costs nothing in shipping configurations.
                Spark::Graphics::RHIValidationLayer::GetInstance().ValidateDrawCall(vertexCount, 0);
                m_context->Draw(vertexCount, startVertex);
            }

            void D3D11CommandList::DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex)
            {
                m_context->DrawIndexed(indexCount, startIndex, baseVertex);
            }

            void D3D11CommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex,
                                                 uint32_t startInstance)
            {
                Spark::Graphics::RHIValidationLayer::GetInstance().ValidateDrawCall(vertexCount, 0);
                m_context->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance);
            }

            void D3D11CommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount,
                                                        uint32_t startIndex, int32_t baseVertex, uint32_t startInstance)
            {
                m_context->DrawIndexedInstanced(indexCount, instanceCount, startIndex, baseVertex, startInstance);
            }

            void D3D11CommandList::Dispatch(uint32_t x, uint32_t y, uint32_t z)
            {
                m_context->Dispatch(x, y, z);
            }

            namespace
            {
                /// D3D11 rejects indirect draws whose args buffer lacks MISC_DRAWINDIRECT_ARGS.
                /// Fail loudly instead of letting the runtime drop the call silently.
                bool IsUsableIndirectArgsBuffer(const D3D11Buffer* buffer, const char* callSite)
                {
                    if (buffer->GetDesc().usage & RHIBufferUsage::IndirectArgs)
                        return true;

                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "%s: args buffer '%s' was not created with RHIBufferUsage::IndirectArgs — "
                                    "the D3D11 runtime would reject the call",
                                    callSite, buffer->GetDebugName().c_str());
                    return false;
                }
            } // namespace

            void D3D11CommandList::DrawInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
            {
                if (!argsBuffer)
                    return;
                auto* d3dBuf = static_cast<D3D11Buffer*>(argsBuffer);
                if (!IsUsableIndirectArgsBuffer(d3dBuf, "DrawInstancedIndirect"))
                    return;
                m_context->DrawInstancedIndirect(d3dBuf->GetD3D11Buffer(), argsOffset);
            }

            void D3D11CommandList::DrawIndexedInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
            {
                if (!argsBuffer)
                    return;
                auto* d3dBuf = static_cast<D3D11Buffer*>(argsBuffer);
                if (!IsUsableIndirectArgsBuffer(d3dBuf, "DrawIndexedInstancedIndirect"))
                    return;
                m_context->DrawIndexedInstancedIndirect(d3dBuf->GetD3D11Buffer(), argsOffset);
            }

            void D3D11CommandList::DispatchIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
            {
                if (!argsBuffer)
                    return;
                auto* d3dBuf = static_cast<D3D11Buffer*>(argsBuffer);
                if (!IsUsableIndirectArgsBuffer(d3dBuf, "DispatchIndirect"))
                    return;
                m_context->DispatchIndirect(d3dBuf->GetD3D11Buffer(), argsOffset);
            }

            void D3D11CommandList::CopyTexture(IRHITexture* dst, IRHITexture* src)
            {
                if (!dst || !src)
                    return;
                auto* d3dDst = static_cast<D3D11Texture*>(dst);
                auto* d3dSrc = static_cast<D3D11Texture*>(src);
                m_context->CopyResource(d3dDst->GetD3D11Resource(), d3dSrc->GetD3D11Resource());
            }

            void D3D11CommandList::BeginEvent(const char*) {}
            void D3D11CommandList::EndEvent() {}
            void D3D11CommandList::SetMarker(const char*) {}

            // ============================================================================
            // D3D11 DEVICE
            // ============================================================================

            D3D11Device::D3D11Device()
            {
                m_capabilities.backend = GraphicsBackend::D3D11;
            }

            D3D11Device::~D3D11Device()
            {
                Shutdown();
            }

            bool D3D11Device::Initialize(const RHIDeviceDesc& desc)
            {
                SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "D3D11Device::Initialize starting");
                m_debugEnabled = desc.enableDebugLayer;

                // Detect Wine environment. Under Wine+DXVK, D3D11 is translated to
                // Vulkan. WARP is unavailable and repeated D3D11 device creation can
                // crash with Lavapipe (software Vulkan) due to DXVK state corruption.
                static int s_wineDetected = -1;
                static bool s_wineD3D11Failed = false;
                if (s_wineDetected < 0)
                {
                    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
                    s_wineDetected = (ntdll && GetProcAddress(ntdll, "wine_get_version")) ? 1 : 0;
                }

                // If a previous D3D11 init already failed under Wine, don't retry —
                // DXVK state may be corrupted and retrying can cause ACCESS_VIOLATION.
                if (s_wineD3D11Failed)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "D3D11: Skipping init — previous attempt failed under Wine");
                    return false;
                }

                UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
                if (desc.enableDebugLayer)
                    createFlags |= D3D11_CREATE_DEVICE_DEBUG;

                // Feature level 11_0 is the floor: every shader the engine compiles targets
                // Shader Model 5.0, which a FL10.x device cannot create. Accepting 10_x here
                // produced a device that initialized "successfully" and then failed every
                // CreateVertexShader call.
                D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

                ComPtr<ID3D11Device> device;
                ComPtr<ID3D11DeviceContext> context;
                D3D_FEATURE_LEVEL achievedLevel;

                // Try hardware first, fall back to WARP (Microsoft's software rasterizer)
                // if no GPU is available (e.g. CI runners, headless servers)
                HRESULT hr =
                    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags, featureLevels,
                                      _countof(featureLevels), D3D11_SDK_VERSION, &device, &achievedLevel, &context);

                if (FAILED(hr))
                {
                    // Wine/DXVK does not support WARP (D3D_DRIVER_TYPE_WARP) and
                    // will crash with ACCESS_VIOLATION instead of returning a clean
                    // HRESULT failure. Skip WARP entirely when running under Wine.
                    if (s_wineDetected)
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                       "D3D11: Running under Wine — WARP fallback unavailable");
                        s_wineD3D11Failed = true;
                        return false;
                    }

                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "D3D11: Hardware device creation failed — falling back to WARP software rasterizer");
                    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createFlags, featureLevels,
                                           _countof(featureLevels), D3D11_SDK_VERSION, &device, &achievedLevel,
                                           &context);
                    if (FAILED(hr))
                        return false;
                    m_isSoftwareDevice = true;
                }

                hr = device.As(&m_device);
                if (FAILED(hr))
                    return false;

                hr = context.As(&m_immediateContext);
                if (FAILED(hr))
                    return false;

                // Get DXGI factory
                ComPtr<IDXGIDevice> dxgiDevice;
                hr = m_device.As(&dxgiDevice);
                if (FAILED(hr))
                    return false;
                ComPtr<IDXGIAdapter> adapter;
                hr = dxgiDevice->GetAdapter(&adapter);
                if (FAILED(hr))
                    return false;
                hr = adapter->GetParent(__uuidof(IDXGIFactory2), &m_dxgiFactory);
                if (FAILED(hr))
                    return false;

                // Query capabilities
                DXGI_ADAPTER_DESC adapterDesc;
                hr = adapter->GetDesc(&adapterDesc);
                if (FAILED(hr))
                    return false;

                char deviceName[256];
                wcstombs(deviceName, adapterDesc.Description, 256);
                m_capabilities.deviceName = deviceName;
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
                    break;
                default:
                    m_capabilities.vendorName = "Unknown";
                    break;
                }

                m_capabilities.tessellationSupport = (achievedLevel >= D3D_FEATURE_LEVEL_11_0);
                m_capabilities.computeShaderSupport = (achievedLevel >= D3D_FEATURE_LEVEL_11_0);
                m_capabilities.geometryShaderSupport = (achievedLevel >= D3D_FEATURE_LEVEL_10_0);
                m_capabilities.multiDrawIndirectSupport = (achievedLevel >= D3D_FEATURE_LEVEL_11_0);
                m_capabilities.maxTextureSize = (achievedLevel >= D3D_FEATURE_LEVEL_11_0) ? 16384 : 8192;
                m_capabilities.maxRenderTargets = 8;
                m_capabilities.maxAnisotropy = 16.0f;
                m_capabilities.apiVersion = (achievedLevel >= D3D_FEATURE_LEVEL_11_1) ? "DirectX 11.1" : "DirectX 11.0";
                m_capabilities.isSoftwareDevice = m_isSoftwareDevice;

                // Query actual MSAA support
                UINT msaaQuality = 0;
                for (uint32_t samples = 8; samples >= 2; samples /= 2)
                {
                    if (SUCCEEDED(m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, samples,
                                                                          &msaaQuality)) &&
                        msaaQuality > 0)
                    {
                        m_capabilities.maxMSAASamples = samples;
                        break;
                    }
                }

                if (m_isSoftwareDevice)
                {
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "D3D11: Using WARP software device '%s'",
                                   m_capabilities.deviceName.c_str());
                }

                // DX11 supports compute shaders (CS 5.0) for SDFGI software RT fallback
                // No hardware RT on DX11 — requires DX12 for DXR
                m_capabilities.rayTracing.bestBackend = m_capabilities.computeShaderSupport
                                                            ? RayTracingBackend::Software_SDFGI
                                                            : RayTracingBackend::Disabled;
                m_capabilities.rayTracing.supportsHardwareRT = false;
                m_capabilities.rayTracing.supportsInlineRT = false;
                m_capabilities.rayTracing.maxRecursionDepth = 0;
                m_capabilities.rayTracing.supportsVRS = false;
                FinalizeDeviceCapabilities(m_capabilities);

                // Create immediate command list wrapper
                m_immediateCommandList = std::make_unique<D3D11CommandList>(m_immediateContext.Get(), true);

                // Phase Z Theme 3B: wire the transient vertex/index allocator
                // into the lifecycle once the D3D device is ready. CreateBuffer
                // is valid from this point so TransientBufferAllocator::Initialize
                // can materialise its two persistent Dynamic buffers.
                m_transientBuffers.Initialize(this);

                return true;
            }

            void D3D11Device::Shutdown()
            {
                SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "D3D11Device::Shutdown");
                // Phase Z Theme 3B: release the transient allocator's GPU
                // buffers before the device itself goes away.
                m_transientBuffers.Shutdown(this);
                m_immediateCommandList.reset();
                m_immediateContext.Reset();
                m_dxgiFactory.Reset();
                m_device.Reset();
            }

            std::unique_ptr<IRHISwapChain> D3D11Device::CreateSwapChain(const RHISwapChainDesc& desc)
            {
                if (!m_device)
                    return nullptr;

                auto swapChain = std::make_unique<D3D11SwapChain>(m_device.Get(), desc);
                if (!swapChain->IsValid())
                {
                    // The constructor already logged the DXGI failure. Returning a live
                    // object here would let callers (RHIBridge) treat a dead swap chain
                    // as success and crash on the first Present().
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "D3D11Device::CreateSwapChain: swap chain unusable — returning nullptr");
                    return nullptr;
                }
                return swapChain;
            }

            std::unique_ptr<IRHIBuffer> D3D11Device::CreateBuffer(const RHIBufferDesc& desc)
            {
                // A D3D11 buffer UAV only exists on a DEFAULT-usage resource, so honouring a
                // Storage request can require moving the allocation off Dynamic. effectiveDesc
                // is what the buffer is really created with and what D3D11Buffer reports, so
                // MapBuffer/UpdateBuffer afterwards pick the path that matches the resource.
                RHIBufferDesc effectiveDesc = desc;

                D3D11_BUFFER_DESC d3dDesc = {};
                d3dDesc.ByteWidth = static_cast<UINT>(effectiveDesc.size);
                // D3D11 only accepts a structure stride on structured buffers; the RHI
                // stride for vertex/index buffers lives in D3D11Buffer::GetStride().
                if (effectiveDesc.usage & RHIBufferUsage::Structured)
                    d3dDesc.StructureByteStride = effectiveDesc.stride;

                if (effectiveDesc.usage & RHIBufferUsage::Vertex)
                    d3dDesc.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
                if (effectiveDesc.usage & RHIBufferUsage::Index)
                    d3dDesc.BindFlags |= D3D11_BIND_INDEX_BUFFER;
                if (effectiveDesc.usage & RHIBufferUsage::Constant)
                    d3dDesc.BindFlags |= D3D11_BIND_CONSTANT_BUFFER;
                if (effectiveDesc.usage & RHIBufferUsage::Structured)
                {
                    if (effectiveDesc.stride == 0)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "D3D11Device::CreateBuffer: structured buffer '%s' needs a non-zero stride",
                                        effectiveDesc.debugName.c_str());
                        return nullptr;
                    }
                    if (effectiveDesc.size % effectiveDesc.stride != 0)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "D3D11Device::CreateBuffer: structured buffer '%s' size %llu is not a whole "
                                        "multiple of stride %u",
                                        effectiveDesc.debugName.c_str(),
                                        static_cast<unsigned long long>(effectiveDesc.size), effectiveDesc.stride);
                        return nullptr;
                    }
                    d3dDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
                    d3dDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                }
                if (effectiveDesc.usage & RHIBufferUsage::Storage)
                {
                    // Never hand back a live buffer whose GetD3D11UAV() is null: a compute pass
                    // would bind nothing and write nowhere while the caller's null check passes.
                    // Either the UAV is created, or CreateBuffer fails loudly.
                    if (!(effectiveDesc.usage & RHIBufferUsage::Structured))
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "D3D11Device::CreateBuffer: '%s' requested Storage (UAV) without Structured "
                                        "usage — this backend has no UAV for a plain buffer",
                                        effectiveDesc.debugName.c_str());
                        return nullptr;
                    }
                    if (effectiveDesc.access == RHIBufferAccess::Staging ||
                        effectiveDesc.access == RHIBufferAccess::ReadBack)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "D3D11Device::CreateBuffer: '%s' requested Storage (UAV) on a staging/readback "
                                        "buffer — D3D11 forbids bind flags on STAGING resources",
                                        effectiveDesc.debugName.c_str());
                        return nullptr;
                    }
                    if (effectiveDesc.access == RHIBufferAccess::Dynamic)
                    {
                        // A DYNAMIC buffer cannot carry a UAV. The UAV is the load-bearing half
                        // of the request, so promote the allocation to DEFAULT and keep it:
                        // UpdateBuffer falls back to UpdateSubresource for a Static buffer, so
                        // CPU-side writes still land. Only MapBuffer becomes unavailable.
                        SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                       "D3D11Device::CreateBuffer: '%s' requested Storage (UAV) with Dynamic access — "
                                       "promoting to Static (DEFAULT) usage; MapBuffer is unavailable on it",
                                       effectiveDesc.debugName.c_str());
                        effectiveDesc.access = RHIBufferAccess::Static;
                    }
                    d3dDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
                }
                if (effectiveDesc.usage & RHIBufferUsage::IndirectArgs)
                {
                    // D3D11 rejects a buffer carrying both of these misc flags; without the
                    // check the create fails with an opaque HRESULT.
                    if (d3dDesc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "D3D11Device::CreateBuffer: '%s' combines Structured and IndirectArgs usage — "
                                        "D3D11 misc flags are mutually exclusive",
                                        effectiveDesc.debugName.c_str());
                        return nullptr;
                    }
                    d3dDesc.MiscFlags |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
                }

                switch (effectiveDesc.access)
                {
                case RHIBufferAccess::Static:
                    d3dDesc.Usage = D3D11_USAGE_DEFAULT;
                    break;
                case RHIBufferAccess::Dynamic:
                    d3dDesc.Usage = D3D11_USAGE_DYNAMIC;
                    d3dDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    break;
                case RHIBufferAccess::Staging:
                    d3dDesc.Usage = D3D11_USAGE_STAGING;
                    d3dDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
                    d3dDesc.BindFlags = 0;
                    break;
                case RHIBufferAccess::ReadBack:
                    d3dDesc.Usage = D3D11_USAGE_STAGING;
                    d3dDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    d3dDesc.BindFlags = 0;
                    break;
                }

                D3D11_SUBRESOURCE_DATA initData = {};
                initData.pSysMem = effectiveDesc.initialData;

                ComPtr<ID3D11Buffer> buffer;
                HRESULT hr =
                    m_device->CreateBuffer(&d3dDesc, effectiveDesc.initialData ? &initData : nullptr, &buffer);
                if (FAILED(hr))
                {
                    SPARK_LOG_ERROR(
                        Spark::LogCategory::Graphics,
                        "D3D11Device::CreateBuffer: '%s' failed (HRESULT 0x%08lX, bind 0x%04X, misc 0x%04X)",
                        effectiveDesc.debugName.c_str(), hr, d3dDesc.BindFlags, d3dDesc.MiscFlags);
                    return nullptr;
                }

                const UINT elementCount =
                    effectiveDesc.stride > 0 ? static_cast<UINT>(effectiveDesc.size / effectiveDesc.stride) : 0;

                ComPtr<ID3D11ShaderResourceView> srv;
                if ((d3dDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) && elementCount > 0)
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Format = DXGI_FORMAT_UNKNOWN; // Structured buffers use the structure stride
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                    srvDesc.Buffer.FirstElement = 0;
                    srvDesc.Buffer.NumElements = elementCount;
                    hr = m_device->CreateShaderResourceView(buffer.Get(), &srvDesc, &srv);
                    if (FAILED(hr))
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "D3D11Device::CreateBuffer: SRV creation failed for '%s' (HRESULT 0x%08lX)",
                                        effectiveDesc.debugName.c_str(), hr);
                        return nullptr;
                    }
                }

                ComPtr<ID3D11UnorderedAccessView> uav;
                if ((d3dDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) && elementCount > 0)
                {
                    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
                    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
                    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                    uavDesc.Buffer.FirstElement = 0;
                    uavDesc.Buffer.NumElements = elementCount;
                    hr = m_device->CreateUnorderedAccessView(buffer.Get(), &uavDesc, &uav);
                    if (FAILED(hr))
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "D3D11Device::CreateBuffer: UAV creation failed for '%s' (HRESULT 0x%08lX)",
                                        effectiveDesc.debugName.c_str(), hr);
                        return nullptr;
                    }
                }

                // effectiveDesc, not desc: a promoted Storage buffer must report Static so
                // MapBuffer refuses it and UpdateBuffer takes the UpdateSubresource path.
                return std::make_unique<D3D11Buffer>(effectiveDesc, std::move(buffer), std::move(srv),
                                                     std::move(uav));
            }

            namespace
            {
                /// @brief Typeless resource format a depth texture must use to also carry an SRV.
                /// Returns DXGI_FORMAT_UNKNOWN for non-depth formats.
                DXGI_FORMAT DepthTypelessFormat(DXGI_FORMAT depthFormat)
                {
                    switch (depthFormat)
                    {
                    case DXGI_FORMAT_D16_UNORM:
                        return DXGI_FORMAT_R16_TYPELESS;
                    case DXGI_FORMAT_D24_UNORM_S8_UINT:
                        return DXGI_FORMAT_R24G8_TYPELESS;
                    case DXGI_FORMAT_D32_FLOAT:
                        return DXGI_FORMAT_R32_TYPELESS;
                    default:
                        return DXGI_FORMAT_UNKNOWN;
                    }
                }

                /// @brief SRV format that reads the depth channel of a depth texture.
                DXGI_FORMAT DepthShaderResourceFormat(DXGI_FORMAT depthFormat)
                {
                    switch (depthFormat)
                    {
                    case DXGI_FORMAT_D16_UNORM:
                        return DXGI_FORMAT_R16_UNORM;
                    case DXGI_FORMAT_D24_UNORM_S8_UINT:
                        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                    case DXGI_FORMAT_D32_FLOAT:
                        return DXGI_FORMAT_R32_FLOAT;
                    default:
                        return DXGI_FORMAT_UNKNOWN;
                    }
                }
            } // namespace

            std::unique_ptr<IRHITexture> D3D11Device::CreateTexture(const RHITextureDesc& desc)
            {
                if (desc.type != RHITextureType::Texture2D && desc.type != RHITextureType::Texture2DArray)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "D3D11Device::CreateTexture: '%s' requested texture type %d — the D3D11 backend "
                                    "only implements Texture2D/Texture2DArray",
                                    desc.debugName.c_str(), static_cast<int>(desc.type));
                    return nullptr;
                }

                const DXGI_FORMAT format = ConvertFormat(desc.format);
                if (format == DXGI_FORMAT_UNKNOWN)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "D3D11Device::CreateTexture: '%s' has an unsupported pixel format",
                                    desc.debugName.c_str());
                    return nullptr;
                }

                if (desc.sampleCount > 1)
                {
                    UINT qualityLevels = 0;
                    if (FAILED(m_device->CheckMultisampleQualityLevels(format, desc.sampleCount, &qualityLevels)) ||
                        qualityLevels == 0)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "D3D11Device::CreateTexture: '%s' requested %u MSAA samples, unsupported for "
                                        "this format",
                                        desc.debugName.c_str(), desc.sampleCount);
                        return nullptr;
                    }
                }

                // A depth texture that is also sampled must be allocated typeless, with a
                // typed DSV and a typed SRV — D3D11 rejects an SRV on a DXGI_FORMAT_D* resource.
                const DXGI_FORMAT typelessFormat = DepthTypelessFormat(format);
                const bool depthReadback = typelessFormat != DXGI_FORMAT_UNKNOWN &&
                                           (desc.usage & RHITextureUsage::ShaderResource) &&
                                           (desc.usage & RHITextureUsage::DepthStencil);

                D3D11_TEXTURE2D_DESC texDesc = {};
                texDesc.Width = desc.width;
                texDesc.Height = desc.height;
                texDesc.MipLevels = desc.mipLevels;
                texDesc.ArraySize = desc.arraySize;
                texDesc.Format = depthReadback ? typelessFormat : format;
                texDesc.SampleDesc.Count = desc.sampleCount;
                texDesc.SampleDesc.Quality = 0;
                texDesc.Usage = D3D11_USAGE_DEFAULT;

                if (desc.usage & RHITextureUsage::ShaderResource)
                    texDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
                if (desc.usage & RHITextureUsage::RenderTarget)
                    texDesc.BindFlags |= D3D11_BIND_RENDER_TARGET;
                if (desc.usage & RHITextureUsage::DepthStencil)
                    texDesc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
                if (desc.usage & RHITextureUsage::UnorderedAccess)
                    texDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

                ComPtr<ID3D11Texture2D> texture;
                HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &texture);
                if (FAILED(hr))
                    return nullptr;

                ComPtr<ID3D11ShaderResourceView> srv;
                if (desc.usage & RHITextureUsage::ShaderResource)
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Format = depthReadback ? DepthShaderResourceFormat(format) : format;
                    if (desc.sampleCount > 1)
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
                    }
                    else
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srvDesc.Texture2D.MipLevels = desc.mipLevels;
                    }
                    hr = m_device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv);
                    if (FAILED(hr))
                        return nullptr;
                }

                ComPtr<ID3D11RenderTargetView> rtv;
                if (desc.usage & RHITextureUsage::RenderTarget)
                {
                    hr = m_device->CreateRenderTargetView(texture.Get(), nullptr, &rtv);
                    if (FAILED(hr))
                        return nullptr;
                }

                ComPtr<ID3D11DepthStencilView> dsv;
                if (desc.usage & RHITextureUsage::DepthStencil)
                {
                    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
                    dsvDesc.Format = format;
                    dsvDesc.ViewDimension =
                        desc.sampleCount > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
                    hr = m_device->CreateDepthStencilView(texture.Get(), &dsvDesc, &dsv);
                    if (FAILED(hr))
                        return nullptr;
                }

                return std::make_unique<D3D11Texture>(desc, texture, std::move(srv), std::move(rtv), std::move(dsv));
            }

            std::unique_ptr<IRHITexture> D3D11Device::WrapNativeTexture(void* nativeHandle, const RHITextureDesc& desc)
            {
                if (!nativeHandle)
                    return nullptr;

                auto* texture = static_cast<ID3D11Texture2D*>(nativeHandle);
                ComPtr<ID3D11Resource> resource;
                HRESULT hr = texture->QueryInterface(IID_PPV_ARGS(&resource));
                if (FAILED(hr) || !resource)
                    return nullptr;

                ComPtr<ID3D11ShaderResourceView> srv;
                if (desc.usage & RHITextureUsage::ShaderResource)
                {
                    const DXGI_FORMAT format = ConvertFormat(desc.format);
                    // Depth resources are allocated typeless by the renderer; an SRV on
                    // them must use the R-typed view format, never DXGI_FORMAT_D*.
                    const DXGI_FORMAT depthSRVFormat = DepthShaderResourceFormat(format);

                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Format = depthSRVFormat != DXGI_FORMAT_UNKNOWN ? depthSRVFormat : format;
                    if (desc.sampleCount > 1)
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
                    }
                    else
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srvDesc.Texture2D.MipLevels = desc.mipLevels;
                    }
                    HRESULT srvHr = m_device->CreateShaderResourceView(resource.Get(), &srvDesc, &srv);
                    if (FAILED(srvHr))
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "D3D11Device::WrapNativeTexture: SRV creation failed for '%s' "
                                        "(HRESULT 0x%08lX)",
                                        desc.debugName.c_str(), srvHr);
                        return nullptr;
                    }
                }

                return std::make_unique<D3D11Texture>(desc, resource, std::move(srv));
            }

            std::unique_ptr<IRHIShader> D3D11Device::CreateShader(const RHIShaderDesc& desc)
            {
                ComPtr<ID3DBlob> bytecodeBlob;
                ComPtr<ID3DBlob> errorBlob;

                // Compile from source if provided
                if (!desc.sourceCode.empty())
                {
                    const char* target = nullptr;
                    switch (desc.stage)
                    {
                    case RHIShaderStage::Vertex:
                        target = "vs_5_0";
                        break;
                    case RHIShaderStage::Pixel:
                        target = "ps_5_0";
                        break;
                    case RHIShaderStage::Geometry:
                        target = "gs_5_0";
                        break;
                    case RHIShaderStage::Hull:
                        target = "hs_5_0";
                        break;
                    case RHIShaderStage::Domain:
                        target = "ds_5_0";
                        break;
                    case RHIShaderStage::Compute:
                        target = "cs_5_0";
                        break;
                    default:
                        return nullptr; // RT stages not supported in D3D11
                    }

                    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

                    HRESULT hr = D3DCompile(desc.sourceCode.c_str(), desc.sourceCode.size(), desc.debugName.c_str(),
                                            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, desc.entryPoint.c_str(), target,
                                            flags, 0, &bytecodeBlob, &errorBlob);
                    if (FAILED(hr))
                    {
                        if (errorBlob)
                        {
                            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Shader compile failed (%s): %s",
                                            desc.debugName.c_str(),
                                            static_cast<const char*>(errorBlob->GetBufferPointer()));
                        }
                        return nullptr;
                    }
                }
                else if (desc.bytecode && desc.bytecodeSize > 0)
                {
                    HRESULT hr = D3DCreateBlob(desc.bytecodeSize, &bytecodeBlob);
                    if (FAILED(hr))
                        return nullptr;
                    memcpy(bytecodeBlob->GetBufferPointer(), desc.bytecode, desc.bytecodeSize);
                }
                else
                {
                    return nullptr;
                }

                ComPtr<ID3D11DeviceChild> shaderObj;
                HRESULT hr = E_FAIL;

                switch (desc.stage)
                {
                case RHIShaderStage::Vertex:
                {
                    ComPtr<ID3D11VertexShader> vs;
                    hr = m_device->CreateVertexShader(bytecodeBlob->GetBufferPointer(), bytecodeBlob->GetBufferSize(),
                                                      nullptr, &vs);
                    if (SUCCEEDED(hr))
                        vs.As(&shaderObj);
                    break;
                }
                case RHIShaderStage::Pixel:
                {
                    ComPtr<ID3D11PixelShader> ps;
                    hr = m_device->CreatePixelShader(bytecodeBlob->GetBufferPointer(), bytecodeBlob->GetBufferSize(),
                                                     nullptr, &ps);
                    if (SUCCEEDED(hr))
                        ps.As(&shaderObj);
                    break;
                }
                case RHIShaderStage::Geometry:
                {
                    ComPtr<ID3D11GeometryShader> gs;
                    hr = m_device->CreateGeometryShader(bytecodeBlob->GetBufferPointer(), bytecodeBlob->GetBufferSize(),
                                                        nullptr, &gs);
                    if (SUCCEEDED(hr))
                        gs.As(&shaderObj);
                    break;
                }
                case RHIShaderStage::Hull:
                {
                    ComPtr<ID3D11HullShader> hs;
                    hr = m_device->CreateHullShader(bytecodeBlob->GetBufferPointer(), bytecodeBlob->GetBufferSize(),
                                                    nullptr, &hs);
                    if (SUCCEEDED(hr))
                        hs.As(&shaderObj);
                    break;
                }
                case RHIShaderStage::Domain:
                {
                    ComPtr<ID3D11DomainShader> ds;
                    hr = m_device->CreateDomainShader(bytecodeBlob->GetBufferPointer(), bytecodeBlob->GetBufferSize(),
                                                      nullptr, &ds);
                    if (SUCCEEDED(hr))
                        ds.As(&shaderObj);
                    break;
                }
                case RHIShaderStage::Compute:
                {
                    ComPtr<ID3D11ComputeShader> cs;
                    hr = m_device->CreateComputeShader(bytecodeBlob->GetBufferPointer(), bytecodeBlob->GetBufferSize(),
                                                       nullptr, &cs);
                    if (SUCCEEDED(hr))
                        cs.As(&shaderObj);
                    break;
                }
                default:
                    return nullptr; // RT stages not supported in D3D11
                }

                if (FAILED(hr) || !shaderObj)
                    return nullptr;

                return std::make_unique<D3D11Shader>(desc, std::move(shaderObj), std::move(bytecodeBlob));
            }

            std::unique_ptr<IRHISampler> D3D11Device::CreateSampler(const RHISamplerDesc& desc)
            {
                D3D11_SAMPLER_DESC d3dDesc = {};
                d3dDesc.Filter = ConvertFilter(desc);
                d3dDesc.AddressU = ConvertAddressMode(desc.addressU);
                d3dDesc.AddressV = ConvertAddressMode(desc.addressV);
                d3dDesc.AddressW = ConvertAddressMode(desc.addressW);
                d3dDesc.MipLODBias = desc.mipLodBias;
                d3dDesc.MaxAnisotropy = desc.maxAnisotropy;
                d3dDesc.ComparisonFunc = ConvertCompareOp(desc.compareOp);
                memcpy(d3dDesc.BorderColor, desc.borderColor, sizeof(float) * 4);
                d3dDesc.MinLOD = desc.minLod;
                d3dDesc.MaxLOD = desc.maxLod;

                ComPtr<ID3D11SamplerState> sampler;
                HRESULT hr = m_device->CreateSamplerState(&d3dDesc, &sampler);
                if (FAILED(hr))
                    return nullptr;

                return std::make_unique<D3D11Sampler>(desc, std::move(sampler));
            }

            std::unique_ptr<IRHIPipelineState> D3D11Device::CreatePipelineState(const RHIPipelineStateDesc& desc,
                                                                                IRHIShader* vertexShader,
                                                                                IRHIShader* pixelShader)
            {
                auto* d3dVS = static_cast<D3D11Shader*>(vertexShader);
                auto* d3dPS = static_cast<D3D11Shader*>(pixelShader);

                // Create input layout
                std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
                for (const auto& elem : desc.inputLayout.elements)
                {
                    D3D11_INPUT_ELEMENT_DESC d3dElem = {};
                    d3dElem.SemanticName = elem.semanticName.c_str();
                    d3dElem.SemanticIndex = elem.semanticIndex;
                    d3dElem.Format = ConvertVertexFormat(elem.format);
                    d3dElem.InputSlot = elem.inputSlot;
                    d3dElem.AlignedByteOffset = elem.byteOffset;
                    d3dElem.InputSlotClass =
                        elem.perInstance ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
                    d3dElem.InstanceDataStepRate = elem.instanceStepRate;
                    elements.push_back(d3dElem);
                }

                ComPtr<ID3D11InputLayout> inputLayout;
                if (!elements.empty() && d3dVS)
                {
                    HRESULT hr =
                        m_device->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()),
                                                    d3dVS->GetBytecode(), d3dVS->GetBytecodeSize(), &inputLayout);
                    if (FAILED(hr))
                        return nullptr;
                }

                // Create rasterizer state
                D3D11_RASTERIZER_DESC rasterDesc = {};
                rasterDesc.FillMode =
                    (desc.rasterizer.fillMode == RHIFillMode::Wireframe) ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
                rasterDesc.CullMode = (desc.rasterizer.cullMode == RHICullMode::None)    ? D3D11_CULL_NONE
                                      : (desc.rasterizer.cullMode == RHICullMode::Front) ? D3D11_CULL_FRONT
                                                                                         : D3D11_CULL_BACK;
                rasterDesc.FrontCounterClockwise = desc.rasterizer.frontCounterClockwise;
                rasterDesc.DepthBias = desc.rasterizer.depthBias;
                rasterDesc.DepthBiasClamp = desc.rasterizer.depthBiasClamp;
                rasterDesc.SlopeScaledDepthBias = desc.rasterizer.slopeScaledDepthBias;
                rasterDesc.DepthClipEnable = desc.rasterizer.depthClipEnable;
                rasterDesc.ScissorEnable = desc.rasterizer.scissorEnable;
                rasterDesc.MultisampleEnable = desc.rasterizer.multisampleEnable;
                rasterDesc.AntialiasedLineEnable = desc.rasterizer.antialiasedLineEnable;

                ComPtr<ID3D11RasterizerState> rasterizerState;
                HRESULT hr = m_device->CreateRasterizerState(&rasterDesc, &rasterizerState);
                if (FAILED(hr))
                    return nullptr;

                // Create depth stencil state
                D3D11_DEPTH_STENCIL_DESC dsDesc = {};
                dsDesc.DepthEnable = desc.depthStencil.depthEnable;
                dsDesc.DepthWriteMask =
                    desc.depthStencil.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
                dsDesc.DepthFunc = ConvertCompareOp(desc.depthStencil.depthFunc);
                dsDesc.StencilEnable = desc.depthStencil.stencilEnable;
                dsDesc.StencilReadMask = desc.depthStencil.stencilReadMask;
                dsDesc.StencilWriteMask = desc.depthStencil.stencilWriteMask;

                dsDesc.FrontFace.StencilFailOp = ConvertStencilOp(desc.depthStencil.frontFace.stencilFail);
                dsDesc.FrontFace.StencilDepthFailOp = ConvertStencilOp(desc.depthStencil.frontFace.stencilDepthFail);
                dsDesc.FrontFace.StencilPassOp = ConvertStencilOp(desc.depthStencil.frontFace.stencilPass);
                dsDesc.FrontFace.StencilFunc = ConvertCompareOp(desc.depthStencil.frontFace.stencilFunc);

                dsDesc.BackFace.StencilFailOp = ConvertStencilOp(desc.depthStencil.backFace.stencilFail);
                dsDesc.BackFace.StencilDepthFailOp = ConvertStencilOp(desc.depthStencil.backFace.stencilDepthFail);
                dsDesc.BackFace.StencilPassOp = ConvertStencilOp(desc.depthStencil.backFace.stencilPass);
                dsDesc.BackFace.StencilFunc = ConvertCompareOp(desc.depthStencil.backFace.stencilFunc);

                ComPtr<ID3D11DepthStencilState> depthStencilState;
                hr = m_device->CreateDepthStencilState(&dsDesc, &depthStencilState);
                if (FAILED(hr))
                    return nullptr;

                // Create blend state
                D3D11_BLEND_DESC blendDesc = {};
                blendDesc.AlphaToCoverageEnable = desc.blend.alphaToCoverageEnable;
                blendDesc.IndependentBlendEnable = desc.blend.independentBlendEnable;

                for (int i = 0; i < 8; ++i)
                {
                    blendDesc.RenderTarget[i].BlendEnable = desc.blend.renderTargets[i].blendEnable;
                    blendDesc.RenderTarget[i].SrcBlend = ConvertBlendFactor(desc.blend.renderTargets[i].srcBlend);
                    blendDesc.RenderTarget[i].DestBlend = ConvertBlendFactor(desc.blend.renderTargets[i].dstBlend);
                    blendDesc.RenderTarget[i].BlendOp = ConvertBlendOp(desc.blend.renderTargets[i].blendOp);
                    blendDesc.RenderTarget[i].SrcBlendAlpha =
                        ConvertBlendFactor(desc.blend.renderTargets[i].srcBlendAlpha);
                    blendDesc.RenderTarget[i].DestBlendAlpha =
                        ConvertBlendFactor(desc.blend.renderTargets[i].dstBlendAlpha);
                    blendDesc.RenderTarget[i].BlendOpAlpha = ConvertBlendOp(desc.blend.renderTargets[i].blendOpAlpha);
                    blendDesc.RenderTarget[i].RenderTargetWriteMask = desc.blend.renderTargets[i].writeMask;
                }

                ComPtr<ID3D11BlendState> blendState;
                hr = m_device->CreateBlendState(&blendDesc, &blendState);
                if (FAILED(hr))
                    return nullptr;

                return std::make_unique<D3D11PipelineState>(desc, std::move(inputLayout), std::move(rasterizerState),
                                                            std::move(depthStencilState), std::move(blendState), d3dVS,
                                                            d3dPS);
            }

            void* D3D11Device::MapBuffer(IRHIBuffer* buffer)
            {
                auto* d3dBuf = static_cast<D3D11Buffer*>(buffer);

                // The map type must match how the buffer was created. A ReadBack/Staging
                // buffer has CPU read access and D3D11 rejects a WRITE_DISCARD map on it.
                D3D11_MAP mapType;
                switch (d3dBuf->GetDesc().access)
                {
                case RHIBufferAccess::Dynamic:
                    mapType = D3D11_MAP_WRITE_DISCARD;
                    break;
                case RHIBufferAccess::ReadBack:
                    mapType = D3D11_MAP_READ;
                    break;
                case RHIBufferAccess::Staging:
                    mapType = D3D11_MAP_READ_WRITE;
                    break;
                default:
                    // Static/Default buffers are GPU-only and cannot be mapped.
                    return nullptr;
                }

                D3D11_MAPPED_SUBRESOURCE mapped;
                HRESULT hr = m_immediateContext->Map(d3dBuf->GetD3D11Buffer(), 0, mapType, 0, &mapped);
                return SUCCEEDED(hr) ? mapped.pData : nullptr;
            }

            void D3D11Device::UnmapBuffer(IRHIBuffer* buffer)
            {
                auto* d3dBuf = static_cast<D3D11Buffer*>(buffer);
                m_immediateContext->Unmap(d3dBuf->GetD3D11Buffer(), 0);
            }

            void D3D11Device::UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset)
            {
                auto* d3dBuf = static_cast<D3D11Buffer*>(buffer);
                if (d3dBuf->GetDesc().access == RHIBufferAccess::Dynamic)
                {
                    void* mapped = MapBuffer(buffer);
                    if (mapped)
                    {
                        memcpy(static_cast<uint8_t*>(mapped) + offset, data, size);
                        UnmapBuffer(buffer);
                    }
                }
                else
                {
                    // A null D3D11_BOX updates the whole resource and ignores size/offset.
                    // For a partial update, describe the exact byte range to write.
                    const uint64_t bufferSize = d3dBuf->GetSize();
                    if (offset != 0 || size < bufferSize)
                    {
                        D3D11_BOX box = {};
                        box.left = static_cast<UINT>(offset);
                        box.right = static_cast<UINT>(offset + size);
                        box.top = 0;
                        box.bottom = 1;
                        box.front = 0;
                        box.back = 1;
                        m_immediateContext->UpdateSubresource(d3dBuf->GetD3D11Buffer(), 0, &box, data, 0, 0);
                    }
                    else
                    {
                        m_immediateContext->UpdateSubresource(d3dBuf->GetD3D11Buffer(), 0, nullptr, data, 0, 0);
                    }
                }
            }

            void D3D11Device::UpdateTexture(IRHITexture* texture, const void* data, uint32_t mipLevel,
                                            uint32_t arraySlice)
            {
                auto* d3dTex = static_cast<D3D11Texture*>(texture);
                uint32_t subresource = D3D11CalcSubresource(mipLevel, arraySlice, d3dTex->GetMipLevels());

                // Row pitch must be computed for the target mip's dimensions, not the
                // base mip. For block-compressed formats the pitch is measured in rows
                // of 4x4 blocks, so use ceil(width/4) blocks * bytes-per-block.
                const PixelFormat fmt = d3dTex->GetFormat();
                const uint32_t mipWidth = std::max(1u, d3dTex->GetWidth() >> mipLevel);
                uint32_t rowPitch;
                if (Spark::RHI::IsCompressedFormat(fmt))
                    rowPitch = ((mipWidth + 3) / 4) * Spark::RHI::GetFormatSize(fmt);
                else
                    rowPitch = mipWidth * Spark::RHI::GetFormatSize(fmt);

                m_immediateContext->UpdateSubresource(d3dTex->GetD3D11Resource(), subresource, nullptr, data, rowPitch,
                                                      0);
            }

            IRHICommandList* D3D11Device::GetImmediateCommandList()
            {
                return m_immediateCommandList.get();
            }

            std::unique_ptr<IRHICommandList> D3D11Device::CreateDeferredCommandList()
            {
                if (!m_device)
                    return nullptr;

                ComPtr<ID3D11DeviceContext> deferredContext;
                HRESULT hr = m_device->CreateDeferredContext(0, &deferredContext);
                if (FAILED(hr))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "D3D11Device::CreateDeferredCommandList: CreateDeferredContext failed "
                                    "(HRESULT 0x%08lX)",
                                    hr);
                    return nullptr;
                }
                // The command list owns the deferred context — a raw pointer here would
                // dangle as soon as this local ComPtr released the last reference.
                return std::make_unique<D3D11CommandList>(std::move(deferredContext));
            }

            void D3D11Device::ExecuteCommandList(IRHICommandList* commandList)
            {
                if (!commandList || !m_immediateContext)
                    return;

                auto* d3dList = static_cast<D3D11CommandList*>(commandList);
                if (d3dList->IsImmediate())
                    return; // Immediate work has already been submitted.

                ID3D11CommandList* recorded = d3dList->GetRecordedCommandList();
                if (!recorded)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "D3D11Device::ExecuteCommandList: deferred list has no recorded commands — "
                                   "call End() before executing");
                    return;
                }

                // RestoreContextState = TRUE. The Windows renderer draws directly on the
                // immediate context and keeps its shaders, input layout, constant buffers
                // and render targets bound across a frame; replaying a deferred list with
                // FALSE clears all of that, so a mid-frame execute would silently blank
                // the following draws. The save/restore costs a little per execute and
                // buys back the contract every caller assumes.
                m_immediateContext->ExecuteCommandList(recorded, TRUE);
                d3dList->ReleaseRecordedCommandList();
            }

            void D3D11Device::BeginFrame()
            {
                ResetStatistics();
                // Phase Z Theme 3B: pump the transient allocator each frame.
                m_transientBuffers.BeginFrame(this);
            }
            void D3D11Device::EndFrame()
            {
                // Phase Z Theme 3B: release the frame's transient mapping.
                m_transientBuffers.EndFrame(this);
            }
            void D3D11Device::WaitForIdle()
            {
                m_immediateContext->Flush();
            }

            std::string D3D11Device::GetDeviceInfo() const
            {
                std::string info = "=== D3D11 Device Info ===\n";
                info += "Device: " + m_capabilities.deviceName + "\n";
                info += "API: " + m_capabilities.apiVersion + "\n";
                if (m_isSoftwareDevice)
                    info += "Type: Software (WARP)\n";
                info += "VRAM: " + std::to_string(m_capabilities.dedicatedVideoMemory / (1024 * 1024)) + " MB\n";
                info += "Max Texture Size: " + std::to_string(m_capabilities.maxTextureSize) + "\n";
                info += "Compute Shaders: " + std::string(m_capabilities.computeShaderSupport ? "Yes" : "No") + "\n";
                info += "Tessellation: " + std::string(m_capabilities.tessellationSupport ? "Yes" : "No") + "\n";
                return info;
            }

            // ============================================================================
            // FORMAT CONVERSION HELPERS
            // ============================================================================

            DXGI_FORMAT D3D11Device::ConvertFormat(PixelFormat format) const
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
                case PixelFormat::BC5_UNORM:
                    return DXGI_FORMAT_BC5_UNORM;
                case PixelFormat::BC6H_UF16:
                    return DXGI_FORMAT_BC6H_UF16;
                case PixelFormat::BC7_UNORM:
                    return DXGI_FORMAT_BC7_UNORM;
                case PixelFormat::D16_UNORM:
                    return DXGI_FORMAT_D16_UNORM;
                case PixelFormat::D24_UNORM_S8_UINT:
                    return DXGI_FORMAT_D24_UNORM_S8_UINT;
                case PixelFormat::D32_FLOAT:
                    return DXGI_FORMAT_D32_FLOAT;
                default:
                    return DXGI_FORMAT_UNKNOWN;
                }
            }

            D3D11_FILTER D3D11Device::ConvertFilter(const RHISamplerDesc& desc) const
            {
                if (desc.minFilter == RHIFilterMode::Anisotropic || desc.magFilter == RHIFilterMode::Anisotropic)
                    return D3D11_FILTER_ANISOTROPIC;
                if (desc.minFilter == RHIFilterMode::Linear && desc.magFilter == RHIFilterMode::Linear)
                    return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                if (desc.minFilter == RHIFilterMode::Nearest && desc.magFilter == RHIFilterMode::Nearest)
                    return D3D11_FILTER_MIN_MAG_MIP_POINT;
                return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            }

            D3D11_TEXTURE_ADDRESS_MODE D3D11Device::ConvertAddressMode(RHIAddressMode mode) const
            {
                switch (mode)
                {
                case RHIAddressMode::Wrap:
                    return D3D11_TEXTURE_ADDRESS_WRAP;
                case RHIAddressMode::Clamp:
                    return D3D11_TEXTURE_ADDRESS_CLAMP;
                case RHIAddressMode::Mirror:
                    return D3D11_TEXTURE_ADDRESS_MIRROR;
                case RHIAddressMode::Border:
                    return D3D11_TEXTURE_ADDRESS_BORDER;
                case RHIAddressMode::MirrorOnce:
                    return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
                default:
                    return D3D11_TEXTURE_ADDRESS_WRAP;
                }
            }

            D3D11_COMPARISON_FUNC D3D11Device::ConvertCompareOp(RHICompareOp op) const
            {
                switch (op)
                {
                case RHICompareOp::Never:
                    return D3D11_COMPARISON_NEVER;
                case RHICompareOp::Less:
                    return D3D11_COMPARISON_LESS;
                case RHICompareOp::Equal:
                    return D3D11_COMPARISON_EQUAL;
                case RHICompareOp::LessEqual:
                    return D3D11_COMPARISON_LESS_EQUAL;
                case RHICompareOp::Greater:
                    return D3D11_COMPARISON_GREATER;
                case RHICompareOp::NotEqual:
                    return D3D11_COMPARISON_NOT_EQUAL;
                case RHICompareOp::GreaterEqual:
                    return D3D11_COMPARISON_GREATER_EQUAL;
                case RHICompareOp::Always:
                    return D3D11_COMPARISON_ALWAYS;
                default:
                    return D3D11_COMPARISON_LESS;
                }
            }

            D3D11_STENCIL_OP D3D11Device::ConvertStencilOp(RHIStencilOp op) const
            {
                switch (op)
                {
                case RHIStencilOp::Keep:
                    return D3D11_STENCIL_OP_KEEP;
                case RHIStencilOp::Zero:
                    return D3D11_STENCIL_OP_ZERO;
                case RHIStencilOp::Replace:
                    return D3D11_STENCIL_OP_REPLACE;
                case RHIStencilOp::IncrSat:
                    return D3D11_STENCIL_OP_INCR_SAT;
                case RHIStencilOp::DecrSat:
                    return D3D11_STENCIL_OP_DECR_SAT;
                case RHIStencilOp::Invert:
                    return D3D11_STENCIL_OP_INVERT;
                case RHIStencilOp::IncrWrap:
                    return D3D11_STENCIL_OP_INCR;
                case RHIStencilOp::DecrWrap:
                    return D3D11_STENCIL_OP_DECR;
                default:
                    return D3D11_STENCIL_OP_KEEP;
                }
            }

            D3D11_BLEND D3D11Device::ConvertBlendFactor(RHIBlendFactor factor) const
            {
                switch (factor)
                {
                case RHIBlendFactor::Zero:
                    return D3D11_BLEND_ZERO;
                case RHIBlendFactor::One:
                    return D3D11_BLEND_ONE;
                case RHIBlendFactor::SrcColor:
                    return D3D11_BLEND_SRC_COLOR;
                case RHIBlendFactor::InvSrcColor:
                    return D3D11_BLEND_INV_SRC_COLOR;
                case RHIBlendFactor::SrcAlpha:
                    return D3D11_BLEND_SRC_ALPHA;
                case RHIBlendFactor::InvSrcAlpha:
                    return D3D11_BLEND_INV_SRC_ALPHA;
                case RHIBlendFactor::DstAlpha:
                    return D3D11_BLEND_DEST_ALPHA;
                case RHIBlendFactor::InvDstAlpha:
                    return D3D11_BLEND_INV_DEST_ALPHA;
                case RHIBlendFactor::DstColor:
                    return D3D11_BLEND_DEST_COLOR;
                case RHIBlendFactor::InvDstColor:
                    return D3D11_BLEND_INV_DEST_COLOR;
                default:
                    return D3D11_BLEND_ZERO;
                }
            }

            D3D11_BLEND_OP D3D11Device::ConvertBlendOp(RHIBlendOp op) const
            {
                switch (op)
                {
                case RHIBlendOp::Add:
                    return D3D11_BLEND_OP_ADD;
                case RHIBlendOp::Subtract:
                    return D3D11_BLEND_OP_SUBTRACT;
                case RHIBlendOp::RevSubtract:
                    return D3D11_BLEND_OP_REV_SUBTRACT;
                case RHIBlendOp::Min:
                    return D3D11_BLEND_OP_MIN;
                case RHIBlendOp::Max:
                    return D3D11_BLEND_OP_MAX;
                default:
                    return D3D11_BLEND_OP_ADD;
                }
            }

            DXGI_FORMAT D3D11Device::ConvertVertexFormat(RHIVertexFormat format) const
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

        } // namespace D3D11
    } // namespace RHI
} // namespace Spark

#endif // SPARK_PLATFORM_WINDOWS
