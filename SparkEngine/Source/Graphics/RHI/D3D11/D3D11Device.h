/**
 * @file D3D11Device.h
 * @brief DirectX 11 implementation of the RHI device interface
 * @author Spark Engine Team
 * @date 2025
 *
 * Wraps the existing DirectX 11 rendering functionality through the
 * platform-agnostic RHI abstraction layer, enabling multi-API support.
 */

#pragma once
#include "../../../Core/Platform.h"

#ifdef _WIN32

#include "../RHIDevice.h"
#include "../RHIResources.h"
#include "../DeferredDeletionQueue.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace Spark
{
    namespace RHI
    {
        namespace D3D11
        {

            // ============================================================================
            // D3D11 RESOURCE IMPLEMENTATIONS
            // ============================================================================

            class D3D11Buffer : public IRHIBuffer
            {
              public:
                D3D11Buffer(const RHIBufferDesc& desc, ComPtr<ID3D11Buffer> buffer);
                ~D3D11Buffer() override = default;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return m_buffer != nullptr; }

                const RHIBufferDesc& GetDesc() const override { return m_desc; }
                uint64_t GetSize() const override { return m_desc.size; }
                uint32_t GetStride() const override { return m_desc.stride; }
                void* GetNativeHandle() const override { return m_buffer.Get(); }

                ID3D11Buffer* GetD3D11Buffer() const { return m_buffer.Get(); }

              private:
                RHIBufferDesc m_desc;
                ComPtr<ID3D11Buffer> m_buffer;
            };

            class D3D11Texture : public IRHITexture
            {
              public:
                D3D11Texture(const RHITextureDesc& desc, ComPtr<ID3D11Resource> resource,
                             ComPtr<ID3D11ShaderResourceView> srv = nullptr,
                             ComPtr<ID3D11RenderTargetView> rtv = nullptr,
                             ComPtr<ID3D11DepthStencilView> dsv = nullptr);
                ~D3D11Texture() override = default;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return m_resource != nullptr; }

                const RHITextureDesc& GetDesc() const override { return m_desc; }
                uint32_t GetWidth() const override { return m_desc.width; }
                uint32_t GetHeight() const override { return m_desc.height; }
                uint32_t GetDepth() const override { return m_desc.depth; }
                uint32_t GetMipLevels() const override { return m_desc.mipLevels; }
                PixelFormat GetFormat() const override { return m_desc.format; }
                void* GetNativeHandle() const override { return m_resource.Get(); }
                void* GetShaderResourceView() const override { return m_srv.Get(); }
                void* GetRenderTargetView() const override { return m_rtv.Get(); }
                void* GetDepthStencilView() const override { return m_dsv.Get(); }

                ID3D11Resource* GetD3D11Resource() const { return m_resource.Get(); }
                ID3D11ShaderResourceView* GetD3D11SRV() const { return m_srv.Get(); }
                ID3D11RenderTargetView* GetD3D11RTV() const { return m_rtv.Get(); }
                ID3D11DepthStencilView* GetD3D11DSV() const { return m_dsv.Get(); }

                void SetSRV(ComPtr<ID3D11ShaderResourceView> srv) { m_srv = srv; }
                void SetRTV(ComPtr<ID3D11RenderTargetView> rtv) { m_rtv = rtv; }
                void SetDSV(ComPtr<ID3D11DepthStencilView> dsv) { m_dsv = dsv; }

              private:
                RHITextureDesc m_desc;
                ComPtr<ID3D11Resource> m_resource;
                ComPtr<ID3D11ShaderResourceView> m_srv;
                ComPtr<ID3D11RenderTargetView> m_rtv;
                ComPtr<ID3D11DepthStencilView> m_dsv;
            };

            class D3D11Shader : public IRHIShader
            {
              public:
                D3D11Shader(const RHIShaderDesc& desc, ComPtr<ID3D11DeviceChild> shader, ComPtr<ID3DBlob> bytecodeBlob);
                ~D3D11Shader() override = default;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return m_shader != nullptr; }

                RHIShaderStage GetStage() const override { return m_desc.stage; }
                const std::string& GetEntryPoint() const override { return m_desc.entryPoint; }
                void* GetNativeHandle() const override { return m_shader.Get(); }
                const void* GetBytecode() const override;
                size_t GetBytecodeSize() const override;

                ID3D11VertexShader* GetVertexShader() const;
                ID3D11PixelShader* GetPixelShader() const;
                ID3D11GeometryShader* GetGeometryShader() const;
                ID3D11HullShader* GetHullShader() const;
                ID3D11DomainShader* GetDomainShader() const;
                ID3D11ComputeShader* GetComputeShader() const;
                ID3DBlob* GetBlob() const { return m_bytecodeBlob.Get(); }

              private:
                RHIShaderDesc m_desc;
                ComPtr<ID3D11DeviceChild> m_shader;
                ComPtr<ID3DBlob> m_bytecodeBlob;
            };

            class D3D11Sampler : public IRHISampler
            {
              public:
                D3D11Sampler(const RHISamplerDesc& desc, ComPtr<ID3D11SamplerState> sampler);
                ~D3D11Sampler() override = default;

                const std::string& GetDebugName() const override { return m_debugName; }
                void SetDebugName(const std::string& name) override { m_debugName = name; }
                bool IsValid() const override { return m_sampler != nullptr; }

                const RHISamplerDesc& GetDesc() const override { return m_desc; }
                void* GetNativeHandle() const override { return m_sampler.Get(); }

                ID3D11SamplerState* GetD3D11Sampler() const { return m_sampler.Get(); }

              private:
                RHISamplerDesc m_desc;
                ComPtr<ID3D11SamplerState> m_sampler;
                std::string m_debugName;
            };

            class D3D11PipelineState : public IRHIPipelineState
            {
              public:
                D3D11PipelineState(const RHIPipelineStateDesc& desc, ComPtr<ID3D11InputLayout> inputLayout,
                                   ComPtr<ID3D11RasterizerState> rasterizerState,
                                   ComPtr<ID3D11DepthStencilState> depthStencilState,
                                   ComPtr<ID3D11BlendState> blendState, D3D11Shader* vs, D3D11Shader* ps);
                ~D3D11PipelineState() override = default;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return m_inputLayout != nullptr; }

                const RHIPipelineStateDesc& GetDesc() const override { return m_desc; }
                void* GetNativeHandle() const override { return nullptr; }

                ID3D11InputLayout* GetInputLayout() const { return m_inputLayout.Get(); }
                ID3D11RasterizerState* GetRasterizerState() const { return m_rasterizerState.Get(); }
                ID3D11DepthStencilState* GetDepthStencilState() const { return m_depthStencilState.Get(); }
                ID3D11BlendState* GetBlendState() const { return m_blendState.Get(); }
                D3D11Shader* GetVertexShader() const { return m_vertexShader; }
                D3D11Shader* GetPixelShader() const { return m_pixelShader; }

              private:
                RHIPipelineStateDesc m_desc;
                ComPtr<ID3D11InputLayout> m_inputLayout;
                ComPtr<ID3D11RasterizerState> m_rasterizerState;
                ComPtr<ID3D11DepthStencilState> m_depthStencilState;
                ComPtr<ID3D11BlendState> m_blendState;
                D3D11Shader* m_vertexShader = nullptr; ///< Non-owning; shader lifetime managed by caller
                D3D11Shader* m_pixelShader = nullptr;  ///< Non-owning; shader lifetime managed by caller
            };

            // ============================================================================
            // D3D11 SWAP CHAIN
            // ============================================================================

            class D3D11SwapChain : public IRHISwapChain
            {
              public:
                D3D11SwapChain(ID3D11Device* device, const RHISwapChainDesc& desc);
                ~D3D11SwapChain() override;

                bool Present(bool vsync) override;
                bool Resize(uint32_t width, uint32_t height) override;
                IRHITexture* GetBackBuffer() override;
                PixelFormat GetFormat() const override { return m_desc.format; }
                uint32_t GetWidth() const override { return m_desc.width; }
                uint32_t GetHeight() const override { return m_desc.height; }
                uint32_t GetCurrentBufferIndex() const override { return 0; }

              private:
                bool CreateBackBufferViews();

                RHISwapChainDesc m_desc;
                ID3D11Device* m_device = nullptr; ///< Non-owning; lifetime tied to parent D3D11Device
                ComPtr<IDXGISwapChain1> m_swapChain;
                std::unique_ptr<D3D11Texture> m_backBuffer;
            };

            // ============================================================================
            // D3D11 COMMAND LIST
            // ============================================================================

            class D3D11CommandList : public IRHICommandList
            {
              public:
                D3D11CommandList(ID3D11DeviceContext* context, bool isImmediate);
                ~D3D11CommandList() override = default;

                void Begin() override;
                void End() override;
                void Reset() override;

                void SetRenderTargets(IRHITexture* const* renderTargets, uint32_t count,
                                      IRHITexture* depthStencil) override;
                void ClearRenderTarget(IRHITexture* target, const float color[4]) override;
                void ClearDepthStencil(IRHITexture* target, float depth, uint8_t stencil) override;

                void SetViewport(const RHIViewport& viewport) override;
                void SetScissorRect(const RHIScissorRect& rect) override;

                void SetPipelineState(IRHIPipelineState* pipelineState) override;
                void SetPrimitiveTopology(RHIPrimitiveTopology topology) override;

                void SetVertexBuffer(IRHIBuffer* buffer, uint32_t slot, uint32_t offset) override;
                void SetIndexBuffer(IRHIBuffer* buffer, uint32_t offset) override;
                void SetConstantBuffer(RHIShaderStage stage, uint32_t slot, IRHIBuffer* buffer) override;
                void SetShaderResource(RHIShaderStage stage, uint32_t slot, IRHITexture* texture) override;
                void SetSampler(RHIShaderStage stage, uint32_t slot, IRHISampler* sampler) override;

                void Draw(uint32_t vertexCount, uint32_t startVertex) override;
                void DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex) override;
                void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex,
                                   uint32_t startInstance) override;
                void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex,
                                          int32_t baseVertex, uint32_t startInstance) override;

                void Dispatch(uint32_t x, uint32_t y, uint32_t z) override;

                void DrawInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset) override;
                void DrawIndexedInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset) override;
                void DispatchIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset) override;

                void CopyTexture(IRHITexture* dst, IRHITexture* src) override;

                void BeginEvent(const char* name) override;
                void EndEvent() override;
                void SetMarker(const char* name) override;

              private:
                ID3D11DeviceContext* m_context = nullptr; ///< Non-owning; lifetime tied to parent D3D11Device
                bool m_isImmediate;
            };

            // ============================================================================
            // D3D11 DEVICE
            // ============================================================================

            class D3D11Device : public IRHIDevice
            {
              public:
                D3D11Device();
                ~D3D11Device() override;

                bool Initialize(const RHIDeviceDesc& desc) override;
                void Shutdown() override;

                std::unique_ptr<IRHISwapChain> CreateSwapChain(const RHISwapChainDesc& desc) override;

                std::unique_ptr<IRHIBuffer> CreateBuffer(const RHIBufferDesc& desc) override;
                std::unique_ptr<IRHITexture> CreateTexture(const RHITextureDesc& desc) override;
                std::unique_ptr<IRHITexture> WrapNativeTexture(void* nativeHandle, const RHITextureDesc& desc) override;
                std::unique_ptr<IRHIShader> CreateShader(const RHIShaderDesc& desc) override;
                std::unique_ptr<IRHISampler> CreateSampler(const RHISamplerDesc& desc) override;
                std::unique_ptr<IRHIPipelineState> CreatePipelineState(const RHIPipelineStateDesc& desc,
                                                                       IRHIShader* vertexShader,
                                                                       IRHIShader* pixelShader) override;

                void* MapBuffer(IRHIBuffer* buffer) override;
                void UnmapBuffer(IRHIBuffer* buffer) override;
                void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset) override;
                void UpdateTexture(IRHITexture* texture, const void* data, uint32_t mipLevel,
                                   uint32_t arraySlice) override;

                IRHICommandList* GetImmediateCommandList() override;
                std::unique_ptr<IRHICommandList> CreateDeferredCommandList() override;
                void ExecuteCommandList(IRHICommandList* commandList) override;

                void BeginFrame() override;
                void EndFrame() override;
                void WaitForIdle() override;

                GraphicsBackend GetBackendType() const override { return GraphicsBackend::D3D11; }
                const RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
                const RHIStatistics& GetStatistics() const override { return m_statistics; }
                void ResetStatistics() override;
                std::string GetDeviceInfo() const override;

                // D3D11-specific accessors
                ID3D11Device1* GetD3D11Device() const { return m_device.Get(); }
                ID3D11DeviceContext1* GetD3D11Context() const { return m_immediateContext.Get(); }
                bool IsSoftwareDevice() const { return m_isSoftwareDevice; }

              private:
                DXGI_FORMAT ConvertFormat(PixelFormat format) const;
                D3D11_FILTER ConvertFilter(const RHISamplerDesc& desc) const;
                D3D11_TEXTURE_ADDRESS_MODE ConvertAddressMode(RHIAddressMode mode) const;
                D3D11_COMPARISON_FUNC ConvertCompareOp(RHICompareOp op) const;
                D3D11_STENCIL_OP ConvertStencilOp(RHIStencilOp op) const;
                D3D11_BLEND ConvertBlendFactor(RHIBlendFactor factor) const;
                D3D11_BLEND_OP ConvertBlendOp(RHIBlendOp op) const;
                DXGI_FORMAT ConvertVertexFormat(RHIVertexFormat format) const;

                ComPtr<ID3D11Device1> m_device;
                ComPtr<ID3D11DeviceContext1> m_immediateContext;
                ComPtr<IDXGIFactory2> m_dxgiFactory;

                std::unique_ptr<D3D11CommandList> m_immediateCommandList;

                RHIDeviceCapabilities m_capabilities;
                RHIStatistics m_statistics;
                bool m_debugEnabled = false;
                bool m_isSoftwareDevice = false;

                DeferredDeletionQueue m_deletionQueue;
            };

        } // namespace D3D11
    } // namespace RHI
} // namespace Spark

#endif // _WIN32
