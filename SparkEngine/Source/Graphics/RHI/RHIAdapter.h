/**
 * @file RHIAdapter.h
 * @brief Adapter layer that routes legacy GraphicsEngine calls through the RHI
 * @author Spark Engine Team
 * @date 2026
 *
 * Maps the concrete DX11 operations historically called by GraphicsEngine
 * (CreateBuffer, SetRenderTarget, Draw, etc.) onto the abstract IRHIDevice /
 * IRHICommandList interfaces.  This is the key bridge for recommendation R3.2:
 * all rendering work issued by GraphicsEngine or RenderPipeline flows through
 * the RHI, making it possible to swap the backend from D3D11 to Vulkan, Metal,
 * or OpenGL without touching higher-level code.
 *
 * RHIAdapter does **not** own the IRHIDevice.  It takes a non-owning pointer
 * (typically from RenderDevice or RHIBridge) and exposes convenience methods
 * that mirror what GraphicsEngine used to call directly on the D3D11 context.
 *
 * Usage:
 * @code
 *   RHIAdapter adapter;
 *   adapter.Initialize(renderDevice.GetRHIDevice());
 *
 *   auto* vb = adapter.CreateVertexBuffer(data, size, stride);
 *   adapter.BeginPass("GBuffer");
 *   adapter.SetRenderTargets({rt0, rt1}, depth);
 *   adapter.SetViewport(0, 0, width, height);
 *   adapter.SetPipelineState(pso);
 *   adapter.BindVertexBuffer(vb);
 *   adapter.BindIndexBuffer(ib);
 *   adapter.DrawIndexed(indexCount);
 *   adapter.EndPass();
 * @endcode
 */

#pragma once

#include "RHIDevice.h"
#include "RHIResources.h"
#include "RHITypes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::RHI
{

    // ========================================================================
    // Viewport / scissor helpers
    // ========================================================================

    /**
     * @brief Convenience viewport description matching GraphicsEngine semantics.
     */
    struct AdapterViewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float minDepth = 0.0f;
        float maxDepth = 1.0f;
    };

    /**
     * @brief Convenience scissor rect matching GraphicsEngine semantics.
     */
    struct AdapterScissorRect
    {
        int32_t left = 0;
        int32_t top = 0;
        int32_t right = 0;
        int32_t bottom = 0;
    };

    // ========================================================================
    // RHIAdapter
    // ========================================================================

    /**
     * @brief Adapter that translates legacy GraphicsEngine API calls into
     *        backend-agnostic RHI operations.
     *
     * The adapter holds a command list for the current frame.  Callers issue
     * draw/state commands through the adapter; it forwards them to the
     * underlying IRHICommandList obtained from the device.
     *
     * ### Resource ownership
     * Resources created through the adapter (buffers, textures, samplers,
     * pipeline states) are tracked internally and destroyed on Shutdown().
     * Callers receive non-owning raw pointers.
     *
     * ### Thread safety
     * Main thread only -- mirrors the GraphicsEngine contract.
     */
    class RHIAdapter
    {
      public:
        RHIAdapter() = default;
        ~RHIAdapter();

        // -- Lifecycle --------------------------------------------------------

        /**
         * @brief Attach the adapter to an RHI device.
         * @param device Non-owning pointer -- must outlive the adapter.
         * @return true on success.
         */
        bool Initialize(IRHIDevice* device);

        /**
         * @brief Destroy all tracked resources and detach from the device.
         */
        void Shutdown();

        /**
         * @brief Check whether the adapter is ready to issue commands.
         */
        bool IsInitialized() const { return m_device != nullptr; }

        // -- Frame bookkeeping ------------------------------------------------

        /**
         * @brief Begin a new frame.  Must be called before any draw/state cmds.
         */
        void BeginFrame();

        /**
         * @brief End the current frame.
         */
        void EndFrame();

        // -- Pass markers (debug annotations) ---------------------------------

        /**
         * @brief Begin a named pass (GPU debug marker).
         */
        void BeginPass(const char* name);

        /**
         * @brief End the current pass.
         */
        void EndPass();

        // -- Render target binding --------------------------------------------

        /**
         * @brief Set one or more render targets with an optional depth buffer.
         * @param renderTargets Span of render-target texture pointers.
         * @param depthStencil  Optional depth-stencil texture (may be nullptr).
         */
        void SetRenderTargets(std::span<IRHITexture* const> renderTargets, IRHITexture* depthStencil = nullptr);

        /**
         * @brief Clear a render target to a solid colour.
         */
        void ClearRenderTarget(IRHITexture* target, const float color[4]);

        /**
         * @brief Clear a depth-stencil target.
         */
        void ClearDepthStencil(IRHITexture* target, float depth = 1.0f, uint8_t stencil = 0);

        // -- Viewport / scissor -----------------------------------------------

        /**
         * @brief Set the viewport from explicit dimensions.
         */
        void SetViewport(float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f);

        /**
         * @brief Set the viewport from a descriptor.
         */
        void SetViewport(const AdapterViewport& vp);

        /**
         * @brief Set the scissor rectangle.
         */
        void SetScissorRect(const AdapterScissorRect& rect);

        /**
         * @brief Set a full-screen scissor rect matching a given viewport size.
         */
        void SetScissorRect(int32_t width, int32_t height);

        // -- Pipeline state ---------------------------------------------------

        /**
         * @brief Bind a pipeline state object.
         */
        void SetPipelineState(IRHIPipelineState* pso);

        /**
         * @brief Set the primitive topology.
         */
        void SetPrimitiveTopology(RHIPrimitiveTopology topology);

        // -- Resource binding -------------------------------------------------

        /**
         * @brief Bind a vertex buffer to a slot.
         */
        void BindVertexBuffer(IRHIBuffer* buffer, uint32_t slot = 0, uint32_t offset = 0);

        /**
         * @brief Bind an index buffer.
         */
        void BindIndexBuffer(IRHIBuffer* buffer, uint32_t offset = 0);

        /**
         * @brief Bind a constant buffer.
         */
        void BindConstantBuffer(RHIShaderStage stage, uint32_t slot, IRHIBuffer* buffer);

        /**
         * @brief Bind a texture as shader resource.
         */
        void BindTexture(RHIShaderStage stage, uint32_t slot, IRHITexture* texture);

        /**
         * @brief Bind a sampler.
         */
        void BindSampler(RHIShaderStage stage, uint32_t slot, IRHISampler* sampler);

        // -- Draw commands ----------------------------------------------------

        /**
         * @brief Non-indexed draw.
         */
        void Draw(uint32_t vertexCount, uint32_t startVertex = 0);

        /**
         * @brief Indexed draw.
         */
        void DrawIndexed(uint32_t indexCount, uint32_t startIndex = 0, int32_t baseVertex = 0);

        /**
         * @brief Instanced draw (non-indexed).
         */
        void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex = 0,
                           uint32_t startInstance = 0);

        /**
         * @brief Indexed + instanced draw.
         */
        void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex = 0,
                                  int32_t baseVertex = 0, uint32_t startInstance = 0);

        // -- Compute dispatch -------------------------------------------------

        /**
         * @brief Dispatch a compute shader.
         */
        void Dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ);

        /** @brief Issue an indirect draw call from GPU-filled argument buffer. */
        void DrawInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset);

        /** @brief Issue an indirect indexed draw call from GPU-filled argument buffer. */
        void DrawIndexedInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset);

        /** @brief Issue an indirect compute dispatch from GPU-filled argument buffer. */
        void DispatchIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset);

        // -- Resource creation (convenience wrappers) -------------------------
        // DEPRECATED: Resource creation should go through RHIBridge, which owns
        // the IRHIDevice and returns std::unique_ptr with proper lifetime
        // semantics.  RHIAdapter should only be used for command-issuing
        // operations (SetRenderTargets, Draw, Dispatch, etc.).  These wrappers
        // remain for backward compatibility but will be removed in a future
        // release.

        /**
         * @brief Create a vertex buffer.
         * @param data     Pointer to initial vertex data (may be nullptr).
         * @param size     Total buffer size in bytes.
         * @param stride   Per-vertex stride in bytes.
         * @return Non-owning pointer to the created buffer.
         */
        [[deprecated("Use RHIBridge for resource creation instead")]]
        IRHIBuffer* CreateVertexBuffer(const void* data, uint64_t size, uint32_t stride);

        /**
         * @brief Create an index buffer.
         */
        [[deprecated("Use RHIBridge for resource creation instead")]]
        IRHIBuffer* CreateIndexBuffer(const void* data, uint64_t size, uint32_t stride = sizeof(uint32_t));

        /**
         * @brief Create a constant/uniform buffer.
         */
        [[deprecated("Use RHIBridge for resource creation instead")]]
        IRHIBuffer* CreateConstantBuffer(uint64_t size);

        /**
         * @brief Create a structured buffer.
         */
        [[deprecated("Use RHIBridge for resource creation instead")]]
        IRHIBuffer* CreateStructuredBuffer(const void* data, uint64_t size, uint32_t stride);

        /**
         * @brief Create a 2D texture.
         */
        [[deprecated("Use RHIBridge for resource creation instead")]]
        IRHITexture* CreateTexture2D(uint32_t width, uint32_t height, PixelFormat format, RHITextureUsage usage,
                                     const void* initialData = nullptr);

        /**
         * @brief Create a depth-stencil texture.
         */
        [[deprecated("Use RHIBridge for resource creation instead")]]
        IRHITexture* CreateDepthStencil(uint32_t width, uint32_t height,
                                        PixelFormat format = PixelFormat::D24_UNORM_S8_UINT);

        /**
         * @brief Create a render target texture.
         */
        [[deprecated("Use RHIBridge for resource creation instead")]]
        IRHITexture* CreateRenderTarget(uint32_t width, uint32_t height,
                                        PixelFormat format = PixelFormat::R8G8B8A8_UNORM);

        /**
         * @brief Create a sampler state.
         */
        [[deprecated("Use RHIBridge for resource creation instead")]]
        IRHISampler* CreateSampler(const RHISamplerDesc& desc);

        /**
         * @brief Create a graphics pipeline state.
         */
        [[deprecated("Use RHIBridge for resource creation instead")]]
        IRHIPipelineState* CreateGraphicsPipeline(const RHIPipelineStateDesc& desc, IRHIShader* vertexShader,
                                                  IRHIShader* pixelShader);

        /**
         * @brief Compile and create a shader from source.
         */
        [[deprecated("Use RHIBridge for resource creation instead")]]
        IRHIShader* CreateShader(const RHIShaderDesc& desc);

        // -- Resource updates -------------------------------------------------

        /**
         * @brief Update buffer contents.
         */
        void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset = 0);

        /**
         * @brief Map a buffer for CPU write access.
         * @return Mapped pointer, or nullptr on failure.
         */
        void* MapBuffer(IRHIBuffer* buffer);

        /**
         * @brief Unmap a previously mapped buffer.
         */
        void UnmapBuffer(IRHIBuffer* buffer);

        // -- Resource destruction ---------------------------------------------

        /**
         * @brief Destroy a buffer and remove it from internal tracking.
         */
        void DestroyBuffer(IRHIBuffer* buffer);

        /**
         * @brief Destroy a texture and remove it from internal tracking.
         */
        void DestroyTexture(IRHITexture* texture);

        /**
         * @brief Destroy a shader and remove it from internal tracking.
         */
        void DestroyShader(IRHIShader* shader);

        /**
         * @brief Destroy a sampler and remove it from internal tracking.
         */
        void DestroySampler(IRHISampler* sampler);

        /**
         * @brief Destroy a pipeline state and remove it from internal tracking.
         */
        void DestroyPipelineState(IRHIPipelineState* pso);

        // -- Queries ----------------------------------------------------------

        /**
         * @brief Get the underlying RHI device.
         */
        IRHIDevice* GetDevice() const { return m_device; }

        /**
         * @brief Get the active command list.
         */
        IRHICommandList* GetCommandList() const { return m_commandList; }

        /**
         * @brief Get device capabilities.
         */
        const RHIDeviceCapabilities& GetCapabilities() const;

        /**
         * @brief Get the active graphics backend.
         */
        GraphicsBackend GetBackend() const;

        /**
         * @brief Get per-frame RHI statistics.
         */
        const RHIStatistics& GetStatistics() const;

      private:
        IRHIDevice* m_device = nullptr;
        IRHICommandList* m_commandList = nullptr;
        bool m_frameActive = false;

        // Tracked resources for bulk cleanup (adapter owns these via unique_ptr)
        std::vector<std::unique_ptr<IRHIBuffer>> m_ownedBuffers;
        std::vector<std::unique_ptr<IRHITexture>> m_ownedTextures;
        std::vector<std::unique_ptr<IRHIShader>> m_ownedShaders;
        std::vector<std::unique_ptr<IRHISampler>> m_ownedSamplers;
        std::vector<std::unique_ptr<IRHIPipelineState>> m_ownedPipelineStates;
    };

} // namespace Spark::RHI
