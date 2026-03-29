/**
 * @file MetalDevice.h
 * @brief Apple Metal implementation of the RHI device interface
 * @author Spark Engine Team
 * @date 2025
 *
 * Wraps the Apple Metal API through the platform-agnostic RHI abstraction
 * layer. Requires Objective-C++ compilation (.mm) for implementation files.
 * Supports Apple Silicon feature detection including mesh shaders, ray tracing,
 * and argument buffers for bindless resource access.
 */

#pragma once

#ifdef __APPLE__

#include "../RHIDevice.h"
#include "../RHIResources.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <unordered_map>
#include <vector>
#include <mutex>

namespace Spark
{
    namespace RHI
    {
        namespace Metal
        {

            // ============================================================================
            // OBJECTIVE-C HANDLE WRAPPER
            // ============================================================================

            /**
             * @brief RAII wrapper for Objective-C Metal objects using ARC-compatible
             *        __bridge casts. Retains on construction, releases on destruction.
             */
            template <typename T>
            class ObjCHandle
            {
            public:
                ObjCHandle() : m_handle(nil) {}

                explicit ObjCHandle(T handle) : m_handle(handle)
                {
                    if (m_handle)
                    {
                        CFRetain((__bridge CFTypeRef)m_handle);
                    }
                }

                ~ObjCHandle()
                {
                    if (m_handle)
                    {
                        CFRelease((__bridge CFTypeRef)m_handle);
                    }
                }

                ObjCHandle(const ObjCHandle& other) : m_handle(other.m_handle)
                {
                    if (m_handle)
                    {
                        CFRetain((__bridge CFTypeRef)m_handle);
                    }
                }

                ObjCHandle& operator=(const ObjCHandle& other)
                {
                    if (this != &other)
                    {
                        if (m_handle)
                        {
                            CFRelease((__bridge CFTypeRef)m_handle);
                        }
                        m_handle = other.m_handle;
                        if (m_handle)
                        {
                            CFRetain((__bridge CFTypeRef)m_handle);
                        }
                    }
                    return *this;
                }

                ObjCHandle(ObjCHandle&& other) noexcept : m_handle(other.m_handle)
                {
                    other.m_handle = nil;
                }

                ObjCHandle& operator=(ObjCHandle&& other) noexcept
                {
                    if (this != &other)
                    {
                        if (m_handle)
                        {
                            CFRelease((__bridge CFTypeRef)m_handle);
                        }
                        m_handle = other.m_handle;
                        other.m_handle = nil;
                    }
                    return *this;
                }

                T Get() const { return m_handle; }
                explicit operator bool() const { return m_handle != nil; }

                void Reset(T handle = nil)
                {
                    if (m_handle)
                    {
                        CFRelease((__bridge CFTypeRef)m_handle);
                    }
                    m_handle = handle;
                    if (m_handle)
                    {
                        CFRetain((__bridge CFTypeRef)m_handle);
                    }
                }

            private:
                T m_handle;
            };

            // ============================================================================
            // METAL RESOURCE IMPLEMENTATIONS
            // ============================================================================

            class MetalBuffer : public IRHIBuffer
            {
            public:
                MetalBuffer(const RHIBufferDesc& desc, id<MTLBuffer> buffer);
                ~MetalBuffer() override = default;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return static_cast<bool>(m_buffer); }

                const RHIBufferDesc& GetDesc() const override { return m_desc; }
                uint64_t GetSize() const override { return m_desc.size; }
                uint32_t GetStride() const override { return m_desc.stride; }
                void* GetNativeHandle() const override { return (__bridge void*)m_buffer.Get(); }

                id<MTLBuffer> GetMTLBuffer() const { return m_buffer.Get(); }

            private:
                RHIBufferDesc m_desc;
                ObjCHandle<id<MTLBuffer>> m_buffer;
            };

            class MetalTexture : public IRHITexture
            {
            public:
                MetalTexture(const RHITextureDesc& desc, id<MTLTexture> texture);
                ~MetalTexture() override = default;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return static_cast<bool>(m_texture); }

                const RHITextureDesc& GetDesc() const override { return m_desc; }
                uint32_t GetWidth() const override { return m_desc.width; }
                uint32_t GetHeight() const override { return m_desc.height; }
                uint32_t GetDepth() const override { return m_desc.depth; }
                uint32_t GetMipLevels() const override { return m_desc.mipLevels; }
                PixelFormat GetFormat() const override { return m_desc.format; }
                void* GetNativeHandle() const override { return (__bridge void*)m_texture.Get(); }

                // Metal uses the same texture object for all views; return the texture handle
                void* GetShaderResourceView() const override { return (__bridge void*)m_texture.Get(); }
                void* GetRenderTargetView() const override { return (__bridge void*)m_texture.Get(); }
                void* GetDepthStencilView() const override { return (__bridge void*)m_texture.Get(); }

                id<MTLTexture> GetMTLTexture() const { return m_texture.Get(); }

                void SetMTLTexture(id<MTLTexture> texture) { m_texture.Reset(texture); }

            private:
                RHITextureDesc m_desc;
                ObjCHandle<id<MTLTexture>> m_texture;
            };

            class MetalShader : public IRHIShader
            {
            public:
                MetalShader(const RHIShaderDesc& desc, id<MTLLibrary> library, id<MTLFunction> function);
                ~MetalShader() override = default;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return static_cast<bool>(m_function); }

                RHIShaderStage GetStage() const override { return m_desc.stage; }
                const std::string& GetEntryPoint() const override { return m_desc.entryPoint; }
                void* GetNativeHandle() const override { return (__bridge void*)m_function.Get(); }
                const void* GetBytecode() const override { return m_desc.bytecode; }
                size_t GetBytecodeSize() const override { return m_desc.bytecodeSize; }

                id<MTLLibrary> GetMTLLibrary() const { return m_library.Get(); }
                id<MTLFunction> GetMTLFunction() const { return m_function.Get(); }

            private:
                RHIShaderDesc m_desc;
                ObjCHandle<id<MTLLibrary>> m_library;
                ObjCHandle<id<MTLFunction>> m_function;
            };

            class MetalSampler : public IRHISampler
            {
            public:
                MetalSampler(const RHISamplerDesc& desc, id<MTLSamplerState> sampler);
                ~MetalSampler() override = default;

                const std::string& GetDebugName() const override { return m_debugName; }
                void SetDebugName(const std::string& name) override { m_debugName = name; }
                bool IsValid() const override { return static_cast<bool>(m_sampler); }

                const RHISamplerDesc& GetDesc() const override { return m_desc; }
                void* GetNativeHandle() const override { return (__bridge void*)m_sampler.Get(); }

                id<MTLSamplerState> GetMTLSampler() const { return m_sampler.Get(); }

            private:
                RHISamplerDesc m_desc;
                ObjCHandle<id<MTLSamplerState>> m_sampler;
                std::string m_debugName;
            };

            class MetalPipelineState : public IRHIPipelineState
            {
            public:
                MetalPipelineState(const RHIPipelineStateDesc& desc,
                                   id<MTLRenderPipelineState> renderPipeline,
                                   id<MTLDepthStencilState> depthStencilState,
                                   MetalShader* vertexShader,
                                   MetalShader* fragmentShader);
                ~MetalPipelineState() override = default;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return static_cast<bool>(m_renderPipeline); }

                const RHIPipelineStateDesc& GetDesc() const override { return m_desc; }
                void* GetNativeHandle() const override { return (__bridge void*)m_renderPipeline.Get(); }

                id<MTLRenderPipelineState> GetMTLRenderPipelineState() const { return m_renderPipeline.Get(); }
                id<MTLDepthStencilState> GetMTLDepthStencilState() const { return m_depthStencilState.Get(); }
                MetalShader* GetVertexShader() const { return m_vertexShader; }
                MetalShader* GetFragmentShader() const { return m_fragmentShader; }

            private:
                RHIPipelineStateDesc m_desc;
                ObjCHandle<id<MTLRenderPipelineState>> m_renderPipeline;
                ObjCHandle<id<MTLDepthStencilState>> m_depthStencilState;
                MetalShader* m_vertexShader;
                MetalShader* m_fragmentShader;
            };

            /**
             * @brief Metal compute pipeline state, separate from the graphics pipeline
             */
            class MetalComputePipelineState
            {
            public:
                MetalComputePipelineState(id<MTLComputePipelineState> pipeline,
                                          const std::string& debugName);
                ~MetalComputePipelineState() = default;

                bool IsValid() const { return static_cast<bool>(m_pipeline); }
                id<MTLComputePipelineState> GetMTLComputePipelineState() const { return m_pipeline.Get(); }

                const std::string& GetDebugName() const { return m_debugName; }
                NSUInteger GetThreadExecutionWidth() const;
                NSUInteger GetMaxTotalThreadsPerThreadgroup() const;

            private:
                ObjCHandle<id<MTLComputePipelineState>> m_pipeline;
                std::string m_debugName;
            };

            // ============================================================================
            // METAL ARGUMENT BUFFER (BINDLESS RESOURCES)
            // ============================================================================

            /**
             * @brief Wraps a Metal argument buffer for bindless resource binding.
             *        Argument buffers encode resource references into a GPU-accessible
             *        buffer, enabling bindless descriptor indexing on Apple Silicon.
             */
            class MetalArgumentBuffer
            {
            public:
                MetalArgumentBuffer(id<MTLBuffer> argumentBuffer,
                                    id<MTLArgumentEncoder> encoder,
                                    uint32_t maxEntries);
                ~MetalArgumentBuffer() = default;

                void SetBuffer(uint32_t index, id<MTLBuffer> buffer, uint64_t offset);
                void SetTexture(uint32_t index, id<MTLTexture> texture);
                void SetSampler(uint32_t index, id<MTLSamplerState> sampler);

                id<MTLBuffer> GetMTLBuffer() const { return m_argumentBuffer.Get(); }
                id<MTLArgumentEncoder> GetEncoder() const { return m_encoder.Get(); }
                uint32_t GetMaxEntries() const { return m_maxEntries; }

            private:
                ObjCHandle<id<MTLBuffer>> m_argumentBuffer;
                ObjCHandle<id<MTLArgumentEncoder>> m_encoder;
                uint32_t m_maxEntries;
            };

            // ============================================================================
            // METAL SWAP CHAIN
            // ============================================================================

            class MetalSwapChain : public IRHISwapChain
            {
            public:
                MetalSwapChain(id<MTLDevice> device, const RHISwapChainDesc& desc);
                ~MetalSwapChain() override;

                bool Present(bool vsync) override;
                bool Resize(uint32_t width, uint32_t height) override;
                IRHITexture* GetBackBuffer() override;
                PixelFormat GetFormat() const override { return m_desc.format; }
                uint32_t GetWidth() const override { return m_desc.width; }
                uint32_t GetHeight() const override { return m_desc.height; }
                uint32_t GetCurrentBufferIndex() const override { return m_currentBufferIndex; }

                CAMetalLayer* GetMetalLayer() const { return m_metalLayer; }
                id<CAMetalDrawable> GetCurrentDrawable() const { return m_currentDrawable; }
                void AcquireNextDrawable();

            private:
                bool ConfigureMetalLayer();

                RHISwapChainDesc m_desc;
                id<MTLDevice> m_device;
                CAMetalLayer* m_metalLayer;
                id<CAMetalDrawable> m_currentDrawable;
                std::unique_ptr<MetalTexture> m_backBuffer;
                uint32_t m_currentBufferIndex = 0;
            };

            // ============================================================================
            // METAL COMMAND LIST
            // ============================================================================

            class MetalCommandList : public IRHICommandList
            {
            public:
                MetalCommandList(id<MTLCommandQueue> commandQueue, bool isImmediate);
                ~MetalCommandList() override = default;

                void Begin() override;
                void End() override;
                void Reset() override;

                void SetRenderTargets(IRHITexture* const* renderTargets, uint32_t count, IRHITexture* depthStencil) override;
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

                void BeginEvent(const char* name) override;
                void EndEvent() override;
                void SetMarker(const char* name) override;

                // Metal-specific
                id<MTLCommandBuffer> GetMTLCommandBuffer() const { return m_commandBuffer.Get(); }
                id<MTLRenderCommandEncoder> GetRenderEncoder() const { return m_renderEncoder.Get(); }
                id<MTLComputeCommandEncoder> GetComputeEncoder() const { return m_computeEncoder.Get(); }

                void SetComputePipelineState(MetalComputePipelineState* computePipeline);
                void SetArgumentBuffer(RHIShaderStage stage, uint32_t slot, MetalArgumentBuffer* argumentBuffer);

                void EndCurrentEncoder();

            private:
                void EnsureRenderEncoder();
                void EnsureComputeEncoder();
                MTLPrimitiveType ConvertTopology(RHIPrimitiveTopology topology) const;

                ObjCHandle<id<MTLCommandQueue>> m_commandQueue;
                ObjCHandle<id<MTLCommandBuffer>> m_commandBuffer;
                ObjCHandle<id<MTLRenderCommandEncoder>> m_renderEncoder;
                ObjCHandle<id<MTLComputeCommandEncoder>> m_computeEncoder;

                MTLRenderPassDescriptor* m_currentRenderPassDesc = nil;
                MTLPrimitiveType m_currentTopology = MTLPrimitiveTypeTriangle;
                ObjCHandle<id<MTLBuffer>> m_currentIndexBuffer;
                uint32_t m_currentIndexBufferOffset = 0;
                bool m_isImmediate;
            };

            // ============================================================================
            // APPLE SILICON FEATURE CAPABILITIES
            // ============================================================================

            /**
             * @brief Extended capability flags specific to Apple Silicon GPUs.
             *        Detected at device initialization from the MTLDevice GPU family.
             */
            struct MetalFeatureSet
            {
                bool appleGPUFamily7 = false;    // A14+, M1+ base
                bool appleGPUFamily8 = false;    // A15+, M2+
                bool appleGPUFamily9 = false;    // A17 Pro, M3+
                bool meshShaders = false;         // Object/mesh shader pipeline
                bool rayTracing = false;          // Hardware-accelerated ray tracing
                bool sparseTileTextures = false;  // Sparse/virtual textures
                bool dynamicLibraries = false;    // MTLDynamicLibrary support
                bool functionPointers = false;    // Indirect function calls in shaders
                bool barycenticCoords = false;    // Fragment barycentric coordinates
                bool int64Atomics = false;        // 64-bit atomic operations in shaders
                bool simdPermute = false;         // SIMD group permute operations
                bool losslessFramebufferCompression = false;
                uint32_t maxArgumentBufferEntries = 0;
                uint32_t maxBufferLength = 0;     // Maximum single buffer size in bytes
                uint32_t maxThreadgroupMemoryLength = 0;
            };

            // ============================================================================
            // METAL DEVICE
            // ============================================================================

            class MetalDevice : public IRHIDevice
            {
            public:
                MetalDevice();
                ~MetalDevice() override;

                // IRHIDevice interface
                bool Initialize(const RHIDeviceDesc& desc) override;
                void Shutdown() override;

                [[nodiscard]] std::unique_ptr<IRHISwapChain> CreateSwapChain(const RHISwapChainDesc& desc) override;

                [[nodiscard]] std::unique_ptr<IRHIBuffer> CreateBuffer(const RHIBufferDesc& desc) override;
                [[nodiscard]] std::unique_ptr<IRHITexture> CreateTexture(const RHITextureDesc& desc) override;
                [[nodiscard]] std::unique_ptr<IRHITexture> WrapNativeTexture(void* nativeHandle,
                                                                const RHITextureDesc& desc) override;
                [[nodiscard]] std::unique_ptr<IRHIShader> CreateShader(const RHIShaderDesc& desc) override;
                [[nodiscard]] std::unique_ptr<IRHISampler> CreateSampler(const RHISamplerDesc& desc) override;
                [[nodiscard]] std::unique_ptr<IRHIPipelineState> CreatePipelineState(const RHIPipelineStateDesc& desc,
                                                                       IRHIShader* vertexShader,
                                                                       IRHIShader* pixelShader) override;

                void* MapBuffer(IRHIBuffer* buffer) override;
                void UnmapBuffer(IRHIBuffer* buffer) override;
                void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset) override;
                void UpdateTexture(IRHITexture* texture, const void* data, uint32_t mipLevel,
                                   uint32_t arraySlice) override;

                IRHICommandList* GetImmediateCommandList() override;
                [[nodiscard]] std::unique_ptr<IRHICommandList> CreateDeferredCommandList() override;
                void ExecuteCommandList(IRHICommandList* commandList) override;

                void BeginFrame() override;
                void EndFrame() override;
                void WaitForIdle() override;

                GraphicsBackend GetBackendType() const override { return GraphicsBackend::Metal; }
                const RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
                const RHIStatistics& GetStatistics() const override { return m_statistics; }
                void ResetStatistics() override;
                std::string GetDeviceInfo() const override;

                // Metal-specific accessors
                id<MTLDevice> GetMTLDevice() const { return m_device.Get(); }
                id<MTLCommandQueue> GetMTLCommandQueue() const { return m_commandQueue.Get(); }
                const MetalFeatureSet& GetMetalFeatures() const { return m_metalFeatures; }

                // Compute pipeline creation
                MetalComputePipelineState* CreateComputePipeline(MetalShader* computeShader,
                                                                 const std::string& debugName = "");

                // Argument buffer creation for bindless resources
                MetalArgumentBuffer* CreateArgumentBuffer(id<MTLFunction> function,
                                                          uint32_t bufferIndex,
                                                          uint32_t maxEntries);

            private:
                void DetectFeatures();
                void PopulateCapabilities();

                MTLPixelFormat ConvertFormat(PixelFormat format) const;
                MTLSamplerMinMagFilter ConvertMinMagFilter(RHIFilterMode mode) const;
                MTLSamplerMipFilter ConvertMipFilter(RHIFilterMode mode) const;
                MTLSamplerAddressMode ConvertAddressMode(RHIAddressMode mode) const;
                MTLCompareFunction ConvertCompareOp(RHICompareOp op) const;
                MTLStencilOperation ConvertStencilOp(RHIStencilOp op) const;
                MTLBlendFactor ConvertBlendFactor(RHIBlendFactor factor) const;
                MTLBlendOperation ConvertBlendOp(RHIBlendOp op) const;
                MTLVertexFormat ConvertVertexFormat(RHIVertexFormat format) const;
                MTLResourceOptions ConvertBufferAccess(RHIBufferAccess access) const;
                MTLTextureType ConvertTextureType(RHITextureType type) const;

                ObjCHandle<id<MTLDevice>> m_device;
                ObjCHandle<id<MTLCommandQueue>> m_commandQueue;

                std::unique_ptr<MetalCommandList> m_immediateCommandList;

                RHIDeviceCapabilities m_capabilities;
                RHIStatistics m_statistics;
                MetalFeatureSet m_metalFeatures;

                bool m_debugEnabled = false;
            };

        } // namespace Metal
    } // namespace RHI
} // namespace Spark

#endif // __APPLE__
