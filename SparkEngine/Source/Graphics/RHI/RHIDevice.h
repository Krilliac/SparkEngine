/**
 * @file RHIDevice.h
 * @brief Abstract Rendering Hardware Interface device
 * @author Spark Engine Team
 * @date 2025
 *
 * The RHI Device is the central abstraction for all graphics API backends.
 * It manages resource creation, command submission, and device lifetime.
 */

#pragma once

#include "RHITypes.h"
#include "RHIResources.h"
#include <memory>
#include <string>

namespace Spark
{
    namespace RHI
    {

        /**
 * @brief Swap chain creation descriptor
 */
        struct RHISwapChainDesc
        {
            void* windowHandle = nullptr;
            uint32_t width = 1280;
            uint32_t height = 720;
            PixelFormat format = PixelFormat::R8G8B8A8_UNORM;
            uint32_t bufferCount = 2;
            bool vsync = true;
            bool fullscreen = false;
            uint32_t sampleCount = 1;
        };

        /**
 * @brief Device creation descriptor
 */
        struct RHIDeviceDesc
        {
            GraphicsBackend preferredBackend = GraphicsBackend::Auto;
            bool enableDebugLayer = false;
            bool enableGPUValidation = false;
            std::string applicationName = "SparkEngine";
            uint32_t applicationVersion = 1;
        };

        /**
 * @brief Abstract RHI swap chain
 */
        class IRHISwapChain
        {
          public:
            virtual ~IRHISwapChain() = default;

            virtual bool Present(bool vsync) = 0;
            virtual bool Resize(uint32_t width, uint32_t height) = 0;
            virtual IRHITexture* GetBackBuffer() = 0;
            virtual PixelFormat GetFormat() const = 0;
            virtual uint32_t GetWidth() const = 0;
            virtual uint32_t GetHeight() const = 0;
            virtual uint32_t GetCurrentBufferIndex() const = 0;
        };

        /**
 * @brief Abstract RHI command list for recording GPU commands
 */
        class IRHICommandList
        {
          public:
            virtual ~IRHICommandList() = default;

            // State
            virtual void Begin() = 0;
            virtual void End() = 0;
            virtual void Reset() = 0;

            // Render targets
            virtual void SetRenderTargets(IRHITexture* const* renderTargets, uint32_t count,
                                          IRHITexture* depthStencil) = 0;
            virtual void ClearRenderTarget(IRHITexture* target, const float color[4]) = 0;
            virtual void ClearDepthStencil(IRHITexture* target, float depth, uint8_t stencil) = 0;

            // Viewport and scissor
            virtual void SetViewport(const RHIViewport& viewport) = 0;
            virtual void SetScissorRect(const RHIScissorRect& rect) = 0;

            // Pipeline
            virtual void SetPipelineState(IRHIPipelineState* pipelineState) = 0;
            virtual void SetPrimitiveTopology(RHIPrimitiveTopology topology) = 0;

            // Resources
            virtual void SetVertexBuffer(IRHIBuffer* buffer, uint32_t slot = 0, uint32_t offset = 0) = 0;
            virtual void SetIndexBuffer(IRHIBuffer* buffer, uint32_t offset = 0) = 0;
            virtual void SetConstantBuffer(RHIShaderStage stage, uint32_t slot, IRHIBuffer* buffer) = 0;
            virtual void SetShaderResource(RHIShaderStage stage, uint32_t slot, IRHITexture* texture) = 0;
            virtual void SetSampler(RHIShaderStage stage, uint32_t slot, IRHISampler* sampler) = 0;

            // Draw commands
            virtual void Draw(uint32_t vertexCount, uint32_t startVertex = 0) = 0;
            virtual void DrawIndexed(uint32_t indexCount, uint32_t startIndex = 0, int32_t baseVertex = 0) = 0;
            virtual void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex = 0,
                                       uint32_t startInstance = 0) = 0;
            virtual void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex = 0,
                                              int32_t baseVertex = 0, uint32_t startInstance = 0) = 0;

            // Compute
            virtual void Dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;

            // Indirect draw/dispatch — GPU-driven rendering (The Forge / bgfx pattern)
            virtual void DrawInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset) = 0;
            virtual void DrawIndexedInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset) = 0;
            virtual void DispatchIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset) = 0;

            // Resource copy
            virtual void CopyTexture(IRHITexture* dst, IRHITexture* src) = 0;

            // Debug
            virtual void BeginEvent(const char* name) = 0;
            virtual void EndEvent() = 0;
            virtual void SetMarker(const char* name) = 0;
        };

        /**
 * @brief Abstract RHI Device - core interface for all graphics operations
 */
        class IRHIDevice
        {
          public:
            virtual ~IRHIDevice() = default;

            // Initialization
            virtual bool Initialize(const RHIDeviceDesc& desc) = 0;
            virtual void Shutdown() = 0;

            // Swap chain
            virtual std::unique_ptr<IRHISwapChain> CreateSwapChain(const RHISwapChainDesc& desc) = 0;

            // Resource creation — caller owns the returned unique_ptr
            virtual std::unique_ptr<IRHIBuffer> CreateBuffer(const RHIBufferDesc& desc) = 0;
            virtual std::unique_ptr<IRHITexture> CreateTexture(const RHITextureDesc& desc) = 0;
            virtual std::unique_ptr<IRHIShader> CreateShader(const RHIShaderDesc& desc) = 0;
            virtual std::unique_ptr<IRHISampler> CreateSampler(const RHISamplerDesc& desc) = 0;
            virtual std::unique_ptr<IRHIPipelineState> CreatePipelineState(const RHIPipelineStateDesc& desc,
                                                                           IRHIShader* vertexShader,
                                                                           IRHIShader* pixelShader) = 0;

            /// Wrap a native texture handle (e.g. ID3D11Texture2D*) as an IRHITexture.
            /// The RHI does NOT own the underlying resource — caller must ensure it outlives the wrapper.
            /// Caller owns the returned wrapper object.
            virtual std::unique_ptr<IRHITexture> WrapNativeTexture(void* nativeHandle, const RHITextureDesc& desc) = 0;

            // Resource updates
            virtual void* MapBuffer(IRHIBuffer* buffer) = 0;
            virtual void UnmapBuffer(IRHIBuffer* buffer) = 0;
            virtual void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset = 0) = 0;
            virtual void UpdateTexture(IRHITexture* texture, const void* data, uint32_t mipLevel = 0,
                                       uint32_t arraySlice = 0) = 0;

            // Command lists
            virtual IRHICommandList* GetImmediateCommandList() = 0;
            virtual std::unique_ptr<IRHICommandList> CreateDeferredCommandList() = 0;
            /// Submit a command list recorded with CreateDeferredCommandList().
            ///
            /// Contract every backend must honour: submitting a list does not disturb
            /// state the caller has bound elsewhere. On a backend with an immediate
            /// context (D3D11) that means the immediate context's shaders, input layout,
            /// constant buffers, render targets and viewport survive the call, so a
            /// deferred list may be executed mid-frame without re-binding; queue-based
            /// backends (D3D12, Vulkan) satisfy it by construction. A list that was
            /// never recorded (End() not called) is rejected, not partially replayed.
            virtual void ExecuteCommandList(IRHICommandList* commandList) = 0;

            // Frame management
            virtual void BeginFrame() = 0;
            virtual void EndFrame() = 0;
            virtual void WaitForIdle() = 0;

            // Device info
            virtual GraphicsBackend GetBackendType() const = 0;
            virtual const RHIDeviceCapabilities& GetCapabilities() const = 0;
            virtual const RHIStatistics& GetStatistics() const = 0;
            virtual void ResetStatistics() = 0;
            virtual std::string GetDeviceInfo() const = 0;
        };

        // Note: Factory functions (CreateRHIDevice, GetAvailableBackends, GetBackendName)
        // are declared in RHIFactory.h to avoid duplicate declarations.

    } // namespace RHI
} // namespace Spark
