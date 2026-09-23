/**
 * @file NullRHIDevice.h
 * @brief Headless no-op RHI backend for testing and CI
 *
 * Implements the full IRHIDevice interface with no-op methods, except
 * that resource-creation methods now return real stub objects from
 * `NullRHIResources.h` instead of `nullptr`. Phase Y Theme 3B wires
 * two previously-orphaned RHI utilities into this backend:
 *
 *   - `Spark::RHI::HandlePool<T, Tag, kPoolCapacity>` for each resource
 *     type — CreateBuffer / CreateTexture / CreateShader / CreateSampler /
 *     CreatePipelineState / WrapNativeTexture register the returned object
 *     in the matching pool, and destroying the object returns its slot, so
 *     each pool's Count() is the exact number of live objects of that type
 *     (HEAD-220 leak/soak signal). The pools are cleared on Shutdown; a
 *     resource that outlives Shutdown or the device releases nothing.
 *
 *   - `Spark::RHI::TransientBufferAllocator` — initialized from
 *     NullRHIDevice::Initialize (now that CreateBuffer returns real
 *     NullBuffer instances with CPU-backed storage) and pumped from
 *     BeginFrame / EndFrame so headless tests exercise the full
 *     per-frame transient-memory lifecycle.
 *
 * Enables render graph construction, draw call submission, and
 * shader parameter binding tests without requiring a GPU.
 */

#pragma once

#include "NullRHIResources.h"
#include "RHIDevice.h"
#include "RHIHandlePool.h"
#include "TransientBufferAllocator.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace Spark
{
    namespace RHI
    {

        /**
         * @brief No-op command list that tracks call counts for validation.
         */
        class NullCommandList : public IRHICommandList
        {
          public:
            void Begin() override
            {
                m_drawCalls = 0;
                m_dispatchCalls = 0;
            }
            void End() override {}
            void Reset() override
            {
                m_drawCalls = 0;
                m_dispatchCalls = 0;
            }

            void SetRenderTargets(IRHITexture* const*, uint32_t, IRHITexture*) override {}
            void ClearRenderTarget(IRHITexture*, const float[4]) override {}
            void ClearDepthStencil(IRHITexture*, float, uint8_t) override {}

            void SetViewport(const RHIViewport&) override {}
            void SetScissorRect(const RHIScissorRect&) override {}

            void SetPipelineState(IRHIPipelineState*) override {}
            void SetPrimitiveTopology(RHIPrimitiveTopology) override {}

            void SetVertexBuffer(IRHIBuffer*, uint32_t, uint32_t) override {}
            void SetIndexBuffer(IRHIBuffer*, uint32_t) override {}
            void SetConstantBuffer(RHIShaderStage, uint32_t, IRHIBuffer*) override {}
            void SetShaderResource(RHIShaderStage stage, uint32_t slot, IRHITexture* texture) override
            {
                m_shaderResourceBindCount++;
                m_lastShaderResourceStage = stage;
                m_lastShaderResourceSlot = slot;
                m_lastShaderResource = texture;
            }
            void SetSampler(RHIShaderStage, uint32_t, IRHISampler*) override {}

            void Draw(uint32_t, uint32_t) override { m_drawCalls++; }
            void DrawIndexed(uint32_t, uint32_t, int32_t) override { m_drawCalls++; }
            void DrawInstanced(uint32_t, uint32_t, uint32_t, uint32_t) override { m_drawCalls++; }
            void DrawIndexedInstanced(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override { m_drawCalls++; }

            void Dispatch(uint32_t, uint32_t, uint32_t) override { m_dispatchCalls++; }

            void DrawInstancedIndirect(IRHIBuffer*, uint32_t) override { m_drawCalls++; }
            void DrawIndexedInstancedIndirect(IRHIBuffer*, uint32_t) override { m_drawCalls++; }
            void DispatchIndirect(IRHIBuffer*, uint32_t) override { m_dispatchCalls++; }

            void CopyTexture(IRHITexture*, IRHITexture*) override {}

            void BeginEvent(const char*) override {}
            void EndEvent() override {}
            void SetMarker(const char*) override {}

            uint32_t GetDrawCallCount() const { return m_drawCalls; }
            uint32_t GetDispatchCount() const { return m_dispatchCalls; }
            uint32_t GetShaderResourceBindCount() const { return m_shaderResourceBindCount; }
            IRHITexture* GetLastShaderResource() const { return m_lastShaderResource; }
            RHIShaderStage GetLastShaderResourceStage() const { return m_lastShaderResourceStage; }
            uint32_t GetLastShaderResourceSlot() const { return m_lastShaderResourceSlot; }

          private:
            uint32_t m_drawCalls = 0;
            uint32_t m_dispatchCalls = 0;
            uint32_t m_shaderResourceBindCount = 0;
            RHIShaderStage m_lastShaderResourceStage = RHIShaderStage::Pixel;
            uint32_t m_lastShaderResourceSlot = 0;
            IRHITexture* m_lastShaderResource = nullptr;
        };

        /** @brief CPU-only swap chain with a stable NullTexture back buffer. */
        class NullSwapChain : public IRHISwapChain
        {
          public:
            explicit NullSwapChain(RHISwapChainDesc desc) : m_desc(desc) { RecreateBackBuffer(); }

            bool Present(bool) override
            {
                m_currentIndex = (m_currentIndex + 1) % std::max(1u, m_desc.bufferCount);
                return true;
            }

            bool Resize(uint32_t width, uint32_t height) override
            {
                if (width == 0 || height == 0)
                    return false;
                m_desc.width = width;
                m_desc.height = height;
                m_currentIndex = 0;
                RecreateBackBuffer();
                return true;
            }

            IRHITexture* GetBackBuffer() override { return m_backBuffer.get(); }
            PixelFormat GetFormat() const override { return m_desc.format; }
            uint32_t GetWidth() const override { return m_desc.width; }
            uint32_t GetHeight() const override { return m_desc.height; }
            uint32_t GetCurrentBufferIndex() const override { return m_currentIndex; }

          private:
            void RecreateBackBuffer()
            {
                RHITextureDesc texture;
                texture.width = m_desc.width;
                texture.height = m_desc.height;
                texture.format = m_desc.format;
                texture.sampleCount = std::max(1u, m_desc.sampleCount);
                texture.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
                texture.debugName = "NullSwapChain.BackBuffer";
                m_backBuffer = std::make_unique<NullTexture>(texture);
            }

            RHISwapChainDesc m_desc;
            std::unique_ptr<NullTexture> m_backBuffer;
            uint32_t m_currentIndex = 0;
        };

        /**
         * @brief Headless RHI device that implements all operations as no-ops.
         *
         * Tracks resource creation/destruction counts and call statistics
         * for validation in tests. Returns real NullBuffer/NullTexture/etc.
         * stubs from Create* so downstream code never sees nullptr. Phase Y
         * Theme 3B also wires HandlePool + TransientBufferAllocator into the
         * lifecycle.
         */
        class NullRHIDevice : public IRHIDevice
        {
          public:
            /** @brief Simultaneously live objects tracked per resource type. */
            static constexpr uint32_t kPoolCapacity = 4096;
            using BufferPool = HandlePool<IRHIBuffer, BufferTag, kPoolCapacity>;
            using TexturePool = HandlePool<IRHITexture, TextureTag, kPoolCapacity>;
            using ShaderPool = HandlePool<IRHIShader, ShaderTag, kPoolCapacity>;
            using SamplerPool = HandlePool<IRHISampler, SamplerTag, kPoolCapacity>;
            using PipelinePool = HandlePool<IRHIPipelineState, PipelineTag, kPoolCapacity>;

            static NullRHIDevice& GetInstance()
            {
                static NullRHIDevice instance;
                return instance;
            }

            bool Initialize(const RHIDeviceDesc&) override
            {
                m_caps.deviceName = "Null Device";
                m_caps.vendorName = "SparkEngine";
                m_caps.apiVersion = "Null 1.0";
                m_caps.dedicatedVideoMemory = 0;
                m_caps.maxTextureSize = 16384;
                m_caps.maxRenderTargets = 8;
                m_caps.tessellationSupport = false;
                m_caps.computeShaderSupport = true;
                m_caps.geometryShaderSupport = false;
                m_caps.backend = GraphicsBackend::None;
                m_caps.maxMSAASamples = 1;
                FinalizeDeviceCapabilities(m_caps);
                m_initialized = true;

                // Phase Y: wire the Spark::RHI::TransientBufferAllocator into
                // the headless lifecycle. Initialize creates one NullBuffer
                // for vertices and one for indices via `this->CreateBuffer`,
                // so the pools below track them as real resources.
                m_transientBuffers.Initialize(this);

                return true;
            }

            void Shutdown() override
            {
                // Tear the transient allocator down first: destroying its two
                // NullBuffers returns their slots through the release hook.
                m_transientBuffers.Shutdown(this);

                // Advance the epoch before clearing so objects created before
                // this Shutdown cannot release a slot reused after the next
                // Initialize (Clear resets slot generations).
                ++m_tracker->epoch;
                m_tracker->buffers.Clear();
                m_tracker->textures.Clear();
                m_tracker->shaders.Clear();
                m_tracker->samplers.Clear();
                m_tracker->pipelines.Clear();
                m_tracker->untracked = 0;

                m_initialized = false;
                m_stats = {};
            }

            std::unique_ptr<IRHISwapChain> CreateSwapChain(const RHISwapChainDesc& desc) override
            {
                if (desc.width == 0 || desc.height == 0 || desc.bufferCount == 0)
                    return nullptr;
                return std::make_unique<NullSwapChain>(desc);
            }

            std::unique_ptr<IRHIBuffer> CreateBuffer(const RHIBufferDesc& desc) override
            {
                m_stats.buffersCreated++;
                auto buffer = std::make_unique<NullBuffer>(desc);
                // The caller owns the unique_ptr; the pool only counts it while
                // it is alive and the release hook returns the slot on destroy.
                Track(static_cast<IRHIBuffer*>(buffer.get()), *buffer);
                return buffer;
            }

            std::unique_ptr<IRHITexture> CreateTexture(const RHITextureDesc& desc) override
            {
                m_stats.texturesCreated++;
                auto texture = std::make_unique<NullTexture>(desc);
                Track(static_cast<IRHITexture*>(texture.get()), *texture);
                return texture;
            }

            std::unique_ptr<IRHIShader> CreateShader(const RHIShaderDesc& desc) override
            {
                m_stats.shadersCreated++;
                auto shader = std::make_unique<NullShader>(desc);
                Track(static_cast<IRHIShader*>(shader.get()), *shader);
                return shader;
            }

            std::unique_ptr<IRHISampler> CreateSampler(const RHISamplerDesc& desc) override
            {
                auto sampler = std::make_unique<NullSampler>(desc);
                Track(static_cast<IRHISampler*>(sampler.get()), *sampler);
                return sampler;
            }

            std::unique_ptr<IRHIPipelineState> CreatePipelineState(const RHIPipelineStateDesc& desc, IRHIShader*,
                                                                   IRHIShader*) override
            {
                m_stats.pipelinesCreated++;
                auto pipeline = std::make_unique<NullPipelineState>(desc);
                Track(static_cast<IRHIPipelineState*>(pipeline.get()), *pipeline);
                return pipeline;
            }

            std::unique_ptr<IRHITexture> WrapNativeTexture(void*, const RHITextureDesc& desc) override
            {
                // Wrappers also emit real NullTexture objects so callers
                // never see nullptr.
                auto texture = std::make_unique<NullTexture>(desc);
                Track(static_cast<IRHITexture*>(texture.get()), *texture);
                return texture;
            }

            void* MapBuffer(IRHIBuffer* buffer) override
            {
                // Phase Y: return the CPU-backed storage from NullBuffer so
                // the TransientBufferAllocator receives a writable pointer.
                // A safe static_cast is fine here because every buffer
                // reaching this path was produced by NullRHIDevice::CreateBuffer.
                if (auto* nb = dynamic_cast<NullBuffer*>(buffer))
                    return nb->GetCpuPointer();
                return nullptr;
            }

            void UnmapBuffer(IRHIBuffer*) override {}
            void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset) override
            {
                if (auto* nullBuffer = dynamic_cast<NullBuffer*>(buffer))
                    nullBuffer->Write(data, size, offset);
            }
            void UpdateTexture(IRHITexture*, const void*, uint32_t, uint32_t) override {}

            IRHICommandList* GetImmediateCommandList() override { return &m_commandList; }
            std::unique_ptr<IRHICommandList> CreateDeferredCommandList() override
            {
                return std::make_unique<NullCommandList>();
            }
            void ExecuteCommandList(IRHICommandList*) override { m_stats.commandListsExecuted++; }

            void BeginFrame() override
            {
                m_stats.framesRendered++;
                // Phase Y: pump the transient allocator per frame.
                m_transientBuffers.BeginFrame(this);
            }

            void EndFrame() override
            {
                // Phase Y: release the transient allocator's frame mapping.
                m_transientBuffers.EndFrame(this);
            }

            void WaitForIdle() override {}

            GraphicsBackend GetBackendType() const override { return GraphicsBackend::None; }
            const RHIDeviceCapabilities& GetCapabilities() const override { return m_caps; }
            const RHIStatistics& GetStatistics() const override { return m_rhiStats; }
            void ResetStatistics() override { m_rhiStats = {}; }
            std::string GetDeviceInfo() const override { return "NullRHIDevice (headless)"; }

            bool IsInitialized() const { return m_initialized; }

            /** @brief Test-facing statistics for resource tracking. */
            struct NullStats
            {
                uint32_t buffersCreated = 0;
                uint32_t texturesCreated = 0;
                uint32_t shadersCreated = 0;
                uint32_t pipelinesCreated = 0;
                uint32_t commandListsExecuted = 0;
                uint32_t framesRendered = 0;
            };

            const NullStats& GetNullStats() const { return m_stats; }
            void ResetNullStats() { m_stats = {}; }

            // Phase Y accessors — test hooks for validating the wired
            // HandlePool and TransientBufferAllocator instances.
            // Each pool's Count() is the number of currently live objects.
            const BufferPool& GetBufferPool() const { return m_tracker->buffers; }
            const TexturePool& GetTexturePool() const { return m_tracker->textures; }
            const ShaderPool& GetShaderPool() const { return m_tracker->shaders; }
            const SamplerPool& GetSamplerPool() const { return m_tracker->samplers; }
            const PipelinePool& GetPipelinePool() const { return m_tracker->pipelines; }
            /**
             * @brief Live objects created while their pool was full (kPoolCapacity reached).
             *
             * Non-zero means the pool counts are a floor, not the live total; a leak
             * check must fail rather than trust them.
             */
            uint32_t GetUntrackedResourceCount() const { return m_tracker->untracked; }
            TransientBufferAllocator& GetTransientBuffers() { return m_transientBuffers; }
            const TransientBufferAllocator& GetTransientBuffers() const { return m_transientBuffers; }

            /** @brief Public constructor for factory-created instances (unique_ptr ownership). */
            NullRHIDevice() = default;

          private:
            /**
             * @brief Live-resource pools, heap-owned so resources can outlive the device.
             *
             * Each tracked resource's release hook holds a weak_ptr to this tracker and
             * the epoch it was created in; the hook frees nothing once the device is
             * destroyed or has been shut down since. Not thread-safe, like the rest of
             * NullRHIDevice: create and destroy Null resources on one thread.
             */
            struct ResourceTracker
            {
                BufferPool buffers;
                TexturePool textures;
                ShaderPool shaders;
                SamplerPool samplers;
                PipelinePool pipelines;
                uint64_t epoch = 0;
                uint32_t untracked = 0;
            };

            // Pool selection by resource interface type.
            static BufferPool& PoolFor(ResourceTracker& tracker, IRHIBuffer*) { return tracker.buffers; }
            static TexturePool& PoolFor(ResourceTracker& tracker, IRHITexture*) { return tracker.textures; }
            static ShaderPool& PoolFor(ResourceTracker& tracker, IRHIShader*) { return tracker.shaders; }
            static SamplerPool& PoolFor(ResourceTracker& tracker, IRHISampler*) { return tracker.samplers; }
            static PipelinePool& PoolFor(ResourceTracker& tracker, IRHIPipelineState*) { return tracker.pipelines; }

            /** @brief Count @p resource as live until its NullReleaseHook runs. */
            template <typename Interface> void Track(Interface* resource, NullReleaseHook& hook)
            {
                ResourceTracker& tracker = *m_tracker;
                const auto handle = PoolFor(tracker, resource).Allocate(resource);
                const uint64_t epoch = tracker.epoch;
                std::weak_ptr<ResourceTracker> weakTracker = m_tracker;
                if (!handle.IsValid())
                {
                    ++tracker.untracked;
                    hook.SetReleaseCallback(
                        [weakTracker, epoch]()
                        {
                            auto owner = weakTracker.lock();
                            if (owner && owner->epoch == epoch && owner->untracked > 0)
                                --owner->untracked;
                        });
                    return;
                }
                hook.SetReleaseCallback(
                    [weakTracker, handle, epoch]()
                    {
                        auto owner = weakTracker.lock();
                        if (owner && owner->epoch == epoch)
                            PoolFor(*owner, static_cast<Interface*>(nullptr)).Free(handle);
                    });
            }

            RHIDeviceCapabilities m_caps;
            RHIStatistics m_rhiStats;
            NullStats m_stats;
            NullCommandList m_commandList;
            bool m_initialized = false;

            // Resource tracking pools. Populated on Create*, released when the
            // object is destroyed, cleared on Shutdown. Declared before the
            // transient allocator so its buffers release into a live tracker.
            std::shared_ptr<ResourceTracker> m_tracker = std::make_shared<ResourceTracker>();

            // Phase Y: per-frame transient vertex/index memory. 64 KB vertex
            // + 32 KB index is generous for headless tests and cheap at
            // process startup.
            TransientBufferAllocator m_transientBuffers{64 * 1024, 32 * 1024};
        };

    } // namespace RHI
} // namespace Spark
