/**
 * @file NullRHIDevice.h
 * @brief Headless no-op RHI backend for testing and CI
 *
 * Implements the full IRHIDevice interface with no-op methods.
 * Enables render graph construction, draw call submission, and
 * shader parameter binding tests without requiring a GPU.
 */

#pragma once

#include "RHIDevice.h"
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
            void Begin() override { m_drawCalls = 0; }
            void End() override {}
            void Reset() override { m_drawCalls = 0; }

            void SetRenderTargets(IRHITexture**, uint32_t, IRHITexture*) override {}
            void ClearRenderTarget(IRHITexture*, const float[4]) override {}
            void ClearDepthStencil(IRHITexture*, float, uint8_t) override {}

            void SetViewport(const RHIViewport&) override {}
            void SetScissorRect(const RHIScissorRect&) override {}

            void SetPipelineState(IRHIPipelineState*) override {}
            void SetPrimitiveTopology(RHIPrimitiveTopology) override {}

            void SetVertexBuffer(IRHIBuffer*, uint32_t, uint32_t) override {}
            void SetIndexBuffer(IRHIBuffer*, uint32_t) override {}
            void SetConstantBuffer(RHIShaderStage, uint32_t, IRHIBuffer*) override {}
            void SetShaderResource(RHIShaderStage, uint32_t, IRHITexture*) override {}
            void SetSampler(RHIShaderStage, uint32_t, IRHISampler*) override {}

            void Draw(uint32_t, uint32_t) override { m_drawCalls++; }
            void DrawIndexed(uint32_t, uint32_t, int32_t) override { m_drawCalls++; }
            void DrawInstanced(uint32_t, uint32_t, uint32_t, uint32_t) override { m_drawCalls++; }
            void DrawIndexedInstanced(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override { m_drawCalls++; }

            void Dispatch(uint32_t, uint32_t, uint32_t) override { m_dispatchCalls++; }
            void CopyTexture(IRHITexture*, IRHITexture*) override {}

            void BeginEvent(const char*) override {}
            void EndEvent() override {}
            void SetMarker(const char*) override {}

            uint32_t GetDrawCallCount() const { return m_drawCalls; }
            uint32_t GetDispatchCount() const { return m_dispatchCalls; }

          private:
            uint32_t m_drawCalls = 0;
            uint32_t m_dispatchCalls = 0;
        };

        /**
         * @brief Headless RHI device that implements all operations as no-ops.
         *
         * Tracks resource creation/destruction counts and call statistics
         * for validation in tests. No GPU interaction occurs.
         */
        class NullRHIDevice : public IRHIDevice
        {
          public:
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
                m_caps.videoMemoryMB = 0;
                m_caps.maxTextureSize = 16384;
                m_caps.maxRenderTargets = 8;
                m_caps.supportsTessellation = false;
                m_caps.supportsComputeShaders = true;
                m_caps.supportsGeometryShaders = false;
                m_initialized = true;
                return true;
            }

            void Shutdown() override
            {
                m_initialized = false;
                m_stats = {};
            }

            std::unique_ptr<IRHISwapChain> CreateSwapChain(const RHISwapChainDesc&) override { return nullptr; }

            IRHIBuffer* CreateBuffer(const RHIBufferDesc&) override
            {
                m_stats.buffersCreated++;
                return nullptr;
            }

            IRHITexture* CreateTexture(const RHITextureDesc&) override
            {
                m_stats.texturesCreated++;
                return nullptr;
            }

            IRHIShader* CreateShader(const RHIShaderDesc&) override
            {
                m_stats.shadersCreated++;
                return nullptr;
            }

            IRHISampler* CreateSampler(const RHISamplerDesc&) override { return nullptr; }

            IRHIPipelineState* CreatePipelineState(const RHIPipelineStateDesc&, IRHIShader*, IRHIShader*) override
            {
                m_stats.pipelinesCreated++;
                return nullptr;
            }

            IRHITexture* WrapNativeTexture(void*, const RHITextureDesc&) override { return nullptr; }

            void DestroyBuffer(IRHIBuffer*) override { m_stats.buffersDestroyed++; }
            void DestroyTexture(IRHITexture*) override { m_stats.texturesDestroyed++; }
            void DestroyShader(IRHIShader*) override { m_stats.shadersDestroyed++; }
            void DestroySampler(IRHISampler*) override {}
            void DestroyPipelineState(IRHIPipelineState*) override { m_stats.pipelinesDestroyed++; }

            void* MapBuffer(IRHIBuffer*) override { return nullptr; }
            void UnmapBuffer(IRHIBuffer*) override {}
            void UpdateBuffer(IRHIBuffer*, const void*, size_t, size_t) override {}
            void UpdateTexture(IRHITexture*, const void*, uint32_t, uint32_t) override {}

            IRHICommandList* GetImmediateCommandList() override { return &m_commandList; }
            IRHICommandList* CreateDeferredCommandList() override { return &m_commandList; }
            void ExecuteCommandList(IRHICommandList*) override { m_stats.commandListsExecuted++; }
            void DestroyCommandList(IRHICommandList*) override {}

            void BeginFrame() override { m_stats.framesRendered++; }
            void EndFrame() override {}
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
                uint32_t buffersDestroyed = 0;
                uint32_t texturesCreated = 0;
                uint32_t texturesDestroyed = 0;
                uint32_t shadersCreated = 0;
                uint32_t shadersDestroyed = 0;
                uint32_t pipelinesCreated = 0;
                uint32_t pipelinesDestroyed = 0;
                uint32_t commandListsExecuted = 0;
                uint32_t framesRendered = 0;
            };

            const NullStats& GetNullStats() const { return m_stats; }
            void ResetNullStats() { m_stats = {}; }

          private:
            NullRHIDevice() = default;

            RHIDeviceCapabilities m_caps;
            RHIStatistics m_rhiStats;
            NullStats m_stats;
            NullCommandList m_commandList;
            bool m_initialized = false;
        };

    } // namespace RHI
} // namespace Spark
