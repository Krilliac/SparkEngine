/**
 * @file D3D12Device.h
 * @brief DirectX 12 implementation of the RHI device interface
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a full Direct3D 12 backend for the SparkEngine RHI layer. This
 * implementation manages D3D12 device lifetime, command queue submission,
 * descriptor heap allocation, root signature/pipeline state management,
 * fence-based CPU/GPU synchronization, and DXGI swap chain presentation.
 *
 * Key D3D12 concepts mapped to the RHI abstraction:
 *   - IRHICommandList  -> ID3D12GraphicsCommandList + ID3D12CommandAllocator
 *   - IRHISwapChain    -> IDXGISwapChain4 with N back-buffers
 *   - IRHIBuffer       -> ID3D12Resource (committed or placed)
 *   - IRHITexture      -> ID3D12Resource + descriptor heap entries
 *   - IRHIPipelineState-> ID3D12PipelineState + ID3D12RootSignature
 *
 * Optional DXR (DirectX Raytracing) support is detected at runtime via
 * ID3D12Device5 and D3D12_FEATURE_DATA_D3D12_OPTIONS5.
 */

#pragma once
#include "../../../Core/Platform.h"

#ifdef _WIN32

#include "../RHIDevice.h"
#include "../RHIResources.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif // SPARK_PLATFORM_WINDOWS

#include <array>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace Spark
{
    namespace RHI
    {
        namespace D3D12
        {

            // ============================================================================
            // CONSTANTS
            // ============================================================================

            /// Maximum number of back-buffers supported by the swap chain.
            static constexpr uint32_t MAX_BACK_BUFFER_COUNT = 3;

            /// Maximum number of frames that may be queued on the GPU before the
            /// CPU blocks, controlling render latency vs. throughput.
            static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

            /// Default descriptor heap sizes per type.
            static constexpr uint32_t CBV_SRV_UAV_HEAP_SIZE = 1'000'000;
            static constexpr uint32_t RTV_HEAP_SIZE = 256;
            static constexpr uint32_t DSV_HEAP_SIZE = 64;
            static constexpr uint32_t SAMPLER_HEAP_SIZE = 2048;

            // ============================================================================
            // DESCRIPTOR HEAP ALLOCATOR
            // ============================================================================

            /**
             * @brief Manages a contiguous ID3D12DescriptorHeap with free-list allocation.
             *
             * Each DescriptorAllocation represents a range of one or more consecutive
             * descriptors inside the heap. The allocator hands out ranges and accepts
             * them back via Free(), coalescing adjacent free blocks.
             */
            struct DescriptorAllocation
            {
                D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};
                D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {};
                uint32_t index = 0;
                uint32_t count = 0;

                bool IsValid() const { return count > 0; }
            };

            class DescriptorHeapAllocator
            {
              public:
                DescriptorHeapAllocator() = default;
                ~DescriptorHeapAllocator() = default;

                /**
                 * @brief Initialises the allocator, creating the underlying heap.
                 * @param device          The D3D12 device used to create the heap.
                 * @param type            Heap type (CBV_SRV_UAV, RTV, DSV, SAMPLER).
                 * @param descriptorCount Total number of descriptors in the heap.
                 * @param shaderVisible   Whether the heap is shader-visible (GPU-bound).
                 * @return True on success.
                 */
                bool Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t descriptorCount,
                                bool shaderVisible);

                DescriptorAllocation Allocate(uint32_t count = 1);
                void Free(const DescriptorAllocation& allocation);

                ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
                uint32_t GetDescriptorSize() const { return m_descriptorSize; }

              private:
                ComPtr<ID3D12DescriptorHeap> m_heap;
                D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
                D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {};
                uint32_t m_descriptorSize = 0;
                uint32_t m_capacity = 0;
                uint32_t m_nextFreeIndex = 0;
                std::vector<uint32_t> m_freeList;
                std::mutex m_mutex;
            };

            // ============================================================================
            // FENCE WRAPPER
            // ============================================================================

            /**
             * @brief RAII wrapper around an ID3D12Fence for CPU/GPU synchronization.
             *
             * Each call to Signal() returns a monotonically increasing fence value.
             * WaitForValue() blocks the calling CPU thread until the GPU has reached
             * the requested value.
             */
            class D3D12Fence
            {
              public:
                D3D12Fence() = default;
                ~D3D12Fence();

                bool Initialize(ID3D12Device* device, uint64_t initialValue = 0);

                /**
                 * @brief Signals the fence from the given command queue.
                 * @return The fence value that was signalled.
                 */
                uint64_t Signal(ID3D12CommandQueue* queue);

                /**
                 * @brief Blocks the CPU until the fence reaches at least @p value.
                 */
                void WaitForValue(uint64_t value) const;

                /** @brief Blocks until all previously signalled work completes. */
                void WaitForIdle() const;

                uint64_t GetCurrentValue() const { return m_currentValue; }
                uint64_t GetCompletedValue() const;
                ID3D12Fence* GetFence() const { return m_fence.Get(); }

              private:
                ComPtr<ID3D12Fence> m_fence;
                HANDLE m_fenceEvent = nullptr;
                uint64_t m_currentValue = 0;
            };

            // ============================================================================
            // D3D12 RESOURCE IMPLEMENTATIONS
            // ============================================================================

            /**
             * @brief D3D12 buffer backed by a committed ID3D12Resource.
             *
             * For Dynamic and Staging buffers the resource is placed in an upload
             * heap; for Static buffers a default heap is used with an intermediate
             * upload for initial data.
             */
            class D3D12Buffer : public IRHIBuffer
            {
              public:
                D3D12Buffer(const RHIBufferDesc& desc, ComPtr<ID3D12Resource> resource,
                            ComPtr<ID3D12Resource> uploadResource = nullptr);
                ~D3D12Buffer() override = default;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return m_resource != nullptr; }

                const RHIBufferDesc& GetDesc() const override { return m_desc; }
                uint64_t GetSize() const override { return m_desc.size; }
                uint32_t GetStride() const override { return m_desc.stride; }
                void* GetNativeHandle() const override { return m_resource.Get(); }

                ID3D12Resource* GetD3D12Resource() const { return m_resource.Get(); }
                ID3D12Resource* GetUploadResource() const { return m_uploadResource.Get(); }

                D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const
                {
                    return m_resource ? m_resource->GetGPUVirtualAddress() : 0;
                }

                /// CBV/SRV/UAV descriptor allocated from the shader-visible heap.
                void SetDescriptor(const DescriptorAllocation& descriptor) { m_descriptor = descriptor; }
                const DescriptorAllocation& GetDescriptor() const { return m_descriptor; }

                /// Persistent CPU-visible mapped pointer for dynamic/staging buffers.
                void SetMappedPointer(void* ptr) { m_mappedPointer = ptr; }
                void* GetMappedPointer() const { return m_mappedPointer; }

              private:
                RHIBufferDesc m_desc;
                ComPtr<ID3D12Resource> m_resource;
                ComPtr<ID3D12Resource> m_uploadResource;
                DescriptorAllocation m_descriptor;
                void* m_mappedPointer = nullptr;
            };

            /**
             * @brief D3D12 texture resource with associated descriptor heap entries.
             *
             * Up to three descriptors may be allocated depending on usage flags:
             *   - SRV for ShaderResource
             *   - RTV for RenderTarget
             *   - DSV for DepthStencil
             */
            class D3D12Texture : public IRHITexture
            {
              public:
                D3D12Texture(const RHITextureDesc& desc, ComPtr<ID3D12Resource> resource,
                             const DescriptorAllocation& srvDescriptor = {},
                             const DescriptorAllocation& rtvDescriptor = {},
                             const DescriptorAllocation& dsvDescriptor = {},
                             const DescriptorAllocation& uavDescriptor = {});
                ~D3D12Texture() override = default;

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

                /**
                 * @brief Returns a pointer to the SRV CPU descriptor handle.
                 * @note The caller must interpret this as D3D12_CPU_DESCRIPTOR_HANDLE*.
                 */
                void* GetShaderResourceView() const override
                {
                    return m_srvDescriptor.IsValid()
                               ? const_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(&m_srvDescriptor.cpuHandle)
                               : nullptr;
                }
                void* GetRenderTargetView() const override
                {
                    return m_rtvDescriptor.IsValid()
                               ? const_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(&m_rtvDescriptor.cpuHandle)
                               : nullptr;
                }
                void* GetDepthStencilView() const override
                {
                    return m_dsvDescriptor.IsValid()
                               ? const_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(&m_dsvDescriptor.cpuHandle)
                               : nullptr;
                }

                ID3D12Resource* GetD3D12Resource() const { return m_resource.Get(); }

                const DescriptorAllocation& GetSRVDescriptor() const { return m_srvDescriptor; }
                const DescriptorAllocation& GetRTVDescriptor() const { return m_rtvDescriptor; }
                const DescriptorAllocation& GetDSVDescriptor() const { return m_dsvDescriptor; }
                const DescriptorAllocation& GetUAVDescriptor() const { return m_uavDescriptor; }

                void SetSRVDescriptor(const DescriptorAllocation& d) { m_srvDescriptor = d; }
                void SetRTVDescriptor(const DescriptorAllocation& d) { m_rtvDescriptor = d; }
                void SetDSVDescriptor(const DescriptorAllocation& d) { m_dsvDescriptor = d; }
                void SetUAVDescriptor(const DescriptorAllocation& d) { m_uavDescriptor = d; }

                D3D12_RESOURCE_STATES GetCurrentState() const { return m_currentState; }
                void SetCurrentState(D3D12_RESOURCE_STATES state) { m_currentState = state; }

              private:
                RHITextureDesc m_desc;
                ComPtr<ID3D12Resource> m_resource;
                DescriptorAllocation m_srvDescriptor;
                DescriptorAllocation m_rtvDescriptor;
                DescriptorAllocation m_dsvDescriptor;
                DescriptorAllocation m_uavDescriptor;
                D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_COMMON;
            };

            /**
             * @brief D3D12 compiled shader, storing DXIL bytecode.
             *
             * D3D12 does not use separate shader objects at the API level; instead
             * bytecode is embedded into pipeline state objects. This class stores the
             * bytecode blob so it can be referenced during PSO creation.
             */
            class D3D12Shader : public IRHIShader
            {
              public:
                D3D12Shader(const RHIShaderDesc& desc, ComPtr<ID3DBlob> bytecodeBlob);
                ~D3D12Shader() override = default;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return m_bytecodeBlob != nullptr; }

                RHIShaderStage GetStage() const override { return m_desc.stage; }
                const std::string& GetEntryPoint() const override { return m_desc.entryPoint; }
                void* GetNativeHandle() const override { return m_bytecodeBlob.Get(); }
                const void* GetBytecode() const override
                {
                    return m_bytecodeBlob ? m_bytecodeBlob->GetBufferPointer() : nullptr;
                }
                size_t GetBytecodeSize() const override { return m_bytecodeBlob ? m_bytecodeBlob->GetBufferSize() : 0; }

                D3D12_SHADER_BYTECODE GetD3D12Bytecode() const { return {GetBytecode(), GetBytecodeSize()}; }

                ID3DBlob* GetBlob() const { return m_bytecodeBlob.Get(); }

              private:
                RHIShaderDesc m_desc;
                ComPtr<ID3DBlob> m_bytecodeBlob;
            };

            /**
             * @brief D3D12 static sampler backed by a descriptor heap entry.
             */
            class D3D12Sampler : public IRHISampler
            {
              public:
                D3D12Sampler(const RHISamplerDesc& desc, const DescriptorAllocation& descriptor);
                ~D3D12Sampler() override = default;

                const std::string& GetDebugName() const override { return m_debugName; }
                void SetDebugName(const std::string& name) override { m_debugName = name; }
                bool IsValid() const override { return m_descriptor.IsValid(); }

                const RHISamplerDesc& GetDesc() const override { return m_desc; }
                void* GetNativeHandle() const override
                {
                    return const_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(&m_descriptor.cpuHandle);
                }

                const DescriptorAllocation& GetDescriptor() const { return m_descriptor; }

              private:
                RHISamplerDesc m_desc;
                DescriptorAllocation m_descriptor;
                std::string m_debugName;
            };

            /**
             * @brief D3D12 graphics or compute pipeline state object.
             *
             * Bundles a root signature and an ID3D12PipelineState. The root
             * signature defines the resource binding layout, while the PSO
             * captures all fixed-function and shader state.
             */
            class D3D12PipelineState : public IRHIPipelineState
            {
              public:
                D3D12PipelineState(const RHIPipelineStateDesc& desc, ComPtr<ID3D12PipelineState> pso,
                                   ComPtr<ID3D12RootSignature> rootSignature);
                ~D3D12PipelineState() override = default;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return m_pso != nullptr; }

                const RHIPipelineStateDesc& GetDesc() const override { return m_desc; }
                void* GetNativeHandle() const override { return m_pso.Get(); }

                ID3D12PipelineState* GetPSO() const { return m_pso.Get(); }
                ID3D12RootSignature* GetRootSignature() const { return m_rootSignature.Get(); }

              private:
                RHIPipelineStateDesc m_desc;
                ComPtr<ID3D12PipelineState> m_pso;
                ComPtr<ID3D12RootSignature> m_rootSignature;
            };

            // ============================================================================
            // D3D12 SWAP CHAIN
            // ============================================================================

            /**
             * @brief DXGI 1.5+ swap chain wrapping IDXGISwapChain4.
             *
             * Uses a flip-model swap effect (DXGI_SWAP_EFFECT_FLIP_DISCARD) with
             * triple buffering by default. Each back-buffer has a dedicated RTV
             * descriptor. Resize releases existing back-buffer references before
             * calling ResizeBuffers().
             */
            class D3D12SwapChain : public IRHISwapChain
            {
              public:
                D3D12SwapChain(ID3D12Device* device, ID3D12CommandQueue* commandQueue, IDXGIFactory4* dxgiFactory,
                               DescriptorHeapAllocator* rtvAllocator, const RHISwapChainDesc& desc);
                ~D3D12SwapChain() override;

                bool Present(bool vsync) override;
                bool Resize(uint32_t width, uint32_t height) override;
                IRHITexture* GetBackBuffer() override;
                PixelFormat GetFormat() const override { return m_desc.format; }
                uint32_t GetWidth() const override { return m_desc.width; }
                uint32_t GetHeight() const override { return m_desc.height; }
                uint32_t GetCurrentBufferIndex() const override
                {
                    return m_swapChain ? m_swapChain->GetCurrentBackBufferIndex() : 0;
                }

                IDXGISwapChain4* GetDXGISwapChain() const { return m_swapChain.Get(); }

              private:
                bool CreateBackBufferResources();
                void ReleaseBackBufferResources();

                RHISwapChainDesc m_desc;
                ID3D12Device* m_device = nullptr;
                ID3D12CommandQueue* m_commandQueue = nullptr;
                DescriptorHeapAllocator* m_rtvAllocator = nullptr;

                ComPtr<IDXGISwapChain4> m_swapChain;
                std::array<std::unique_ptr<D3D12Texture>, MAX_BACK_BUFFER_COUNT> m_backBuffers;
                std::array<DescriptorAllocation, MAX_BACK_BUFFER_COUNT> m_backBufferRTVs;
            };

            // ============================================================================
            // D3D12 COMMAND LIST
            // ============================================================================

            /**
             * @brief D3D12 command list wrapping ID3D12GraphicsCommandList.
             *
             * Each command list owns a per-frame command allocator. The allocator
             * is reset at the start of each frame (once the GPU has finished
             * executing the commands from the previous use of this allocator).
             *
             * Resource barrier transitions are tracked internally so that the
             * caller does not need to manage resource states manually.
             */
            class D3D12CommandList : public IRHICommandList
            {
              public:
                D3D12CommandList(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type,
                                 ID3D12PipelineState* initialPSO = nullptr);
                ~D3D12CommandList() override = default;

                // IRHICommandList interface
                void Begin() override;
                void End() override;
                void Reset() override;

                void SetRenderTargets(IRHITexture** renderTargets, uint32_t count, IRHITexture* depthStencil) override;
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

                void BeginEvent(const char* name) override;
                void EndEvent() override;
                void SetMarker(const char* name) override;

                // D3D12-specific API

                /**
                 * @brief Inserts a resource barrier transition.
                 * @param resource    The texture whose state is changing.
                 * @param stateBefore The current resource state.
                 * @param stateAfter  The desired resource state.
                 */
                void TransitionBarrier(D3D12Texture* resource, D3D12_RESOURCE_STATES stateBefore,
                                       D3D12_RESOURCE_STATES stateAfter);

                /**
                 * @brief Inserts a UAV barrier for the given resource (or nullptr for all).
                 */
                void UAVBarrier(ID3D12Resource* resource = nullptr);

                /**
                 * @brief Flushes any pending resource barriers recorded via
                 *        TransitionBarrier() / UAVBarrier().
                 */
                void FlushBarriers();

                ID3D12GraphicsCommandList* GetCommandList() const { return m_commandList.Get(); }
                ID3D12CommandAllocator* GetAllocator() const { return m_commandAllocator.Get(); }

              private:
                ComPtr<ID3D12GraphicsCommandList> m_commandList;
                ComPtr<ID3D12CommandAllocator> m_commandAllocator;
                D3D12_COMMAND_LIST_TYPE m_type;

                /// Pending resource barriers batched for a single call.
                std::vector<D3D12_RESOURCE_BARRIER> m_pendingBarriers;

                /// Currently bound root signature (cached to avoid redundant sets).
                ID3D12RootSignature* m_currentRootSignature = nullptr;
            };

            // ============================================================================
            // PER-FRAME RESOURCES
            // ============================================================================

            /**
             * @brief Resources that are duplicated for each frame in flight.
             *
             * Because the GPU may still be consuming resources from a previous
             * frame while the CPU is recording commands for the current one, each
             * frame gets its own command allocator and fence value.
             */
            struct FrameResources
            {
                ComPtr<ID3D12CommandAllocator> commandAllocator;
                uint64_t fenceValue = 0;
            };

            // ============================================================================
            // D3D12 DEVICE
            // ============================================================================

            /**
             * @brief DirectX 12 implementation of IRHIDevice.
             *
             * Lifecycle:
             *   1. Construct via default constructor.
             *   2. Call Initialize() with a populated RHIDeviceDesc.
             *   3. Use CreateSwapChain(), CreateBuffer(), etc. for rendering.
             *   4. Call Shutdown() (also invoked by the destructor).
             *
             * Threading model:
             *   - Resource creation methods are **not** thread-safe and must be
             *     called from the main thread.
             *   - Command list recording is safe on multiple threads provided
             *     each thread uses its own D3D12CommandList obtained via
             *     CreateDeferredCommandList().
             *   - ExecuteCommandList() serialises submissions through an internal
             *     mutex.
             */
            class D3D12Device : public IRHIDevice
            {
              public:
                D3D12Device();
                ~D3D12Device() override;

                // -- IRHIDevice: Initialization -------------------------------------------

                bool Initialize(const RHIDeviceDesc& desc) override;
                void Shutdown() override;

                // -- IRHIDevice: Swap chain -----------------------------------------------

                std::unique_ptr<IRHISwapChain> CreateSwapChain(const RHISwapChainDesc& desc) override;

                // -- IRHIDevice: Resource creation ----------------------------------------

                IRHIBuffer* CreateBuffer(const RHIBufferDesc& desc) override;
                IRHITexture* CreateTexture(const RHITextureDesc& desc) override;
                IRHIShader* CreateShader(const RHIShaderDesc& desc) override;
                IRHISampler* CreateSampler(const RHISamplerDesc& desc) override;
                IRHIPipelineState* CreatePipelineState(const RHIPipelineStateDesc& desc, IRHIShader* vertexShader,
                                                       IRHIShader* pixelShader) override;

                // -- IRHIDevice: Resource destruction -------------------------------------

                void DestroyBuffer(IRHIBuffer* buffer) override;
                void DestroyTexture(IRHITexture* texture) override;
                void DestroyShader(IRHIShader* shader) override;
                void DestroySampler(IRHISampler* sampler) override;
                void DestroyPipelineState(IRHIPipelineState* state) override;

                // -- IRHIDevice: Resource updates -----------------------------------------

                void* MapBuffer(IRHIBuffer* buffer) override;
                void UnmapBuffer(IRHIBuffer* buffer) override;
                void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset) override;
                void UpdateTexture(IRHITexture* texture, const void* data, uint32_t mipLevel,
                                   uint32_t arraySlice) override;

                // -- IRHIDevice: Command lists --------------------------------------------

                IRHICommandList* GetImmediateCommandList() override;
                IRHICommandList* CreateDeferredCommandList() override;
                void ExecuteCommandList(IRHICommandList* commandList) override;
                void DestroyCommandList(IRHICommandList* commandList) override;

                // -- IRHIDevice: Frame management -----------------------------------------

                void BeginFrame() override;
                void EndFrame() override;
                void WaitForIdle() override;

                // -- IRHIDevice: Device info -----------------------------------------------

                GraphicsBackend GetBackendType() const override { return GraphicsBackend::D3D12; }
                const RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
                const RHIStatistics& GetStatistics() const override { return m_statistics; }
                void ResetStatistics() override;
                std::string GetDeviceInfo() const override;

                // -- D3D12-specific accessors ---------------------------------------------

                ID3D12Device* GetD3D12Device() const { return m_device.Get(); }

                /**
                 * @brief Returns ID3D12Device5 when DXR is supported, nullptr otherwise.
                 */
                ID3D12Device5* GetDXRDevice() const { return m_dxrDevice.Get(); }

                ID3D12CommandQueue* GetDirectQueue() const { return m_directQueue.Get(); }
                ID3D12CommandQueue* GetCopyQueue() const { return m_copyQueue.Get(); }
                ID3D12CommandQueue* GetComputeQueue() const { return m_computeQueue.Get(); }

                IDXGIFactory6* GetDXGIFactory() const { return m_dxgiFactory.Get(); }
                IDXGIAdapter1* GetAdapter() const { return m_adapter.Get(); }

                DescriptorHeapAllocator& GetCBVSRVUAVHeap() { return m_cbvSrvUavHeap; }
                DescriptorHeapAllocator& GetRTVHeap() { return m_rtvHeap; }
                DescriptorHeapAllocator& GetDSVHeap() { return m_dsvHeap; }
                DescriptorHeapAllocator& GetSamplerHeap() { return m_samplerHeap; }

                uint32_t GetCurrentFrameIndex() const { return m_currentFrameIndex; }

                // -- Root signature helpers -----------------------------------------------

                /**
                 * @brief Creates a default root signature suitable for most shaders.
                 *
                 * The layout provides:
                 *   - Root parameter 0: CBV descriptor table (b0-b13, all stages)
                 *   - Root parameter 1: SRV descriptor table (t0-t31, PS)
                 *   - Root parameter 2: Sampler descriptor table (s0-s15, PS)
                 *   - Root parameter 3: UAV descriptor table (u0-u7, all stages)
                 *
                 * @return The created root signature, or nullptr on failure.
                 */
                ComPtr<ID3D12RootSignature> CreateDefaultRootSignature() const;

                /**
                 * @brief Creates a root signature from a serialized blob.
                 * @param data      Pointer to the serialised root signature data.
                 * @param dataSize  Size of the data in bytes.
                 * @return The created root signature, or nullptr on failure.
                 */
                ComPtr<ID3D12RootSignature> CreateRootSignature(const void* data, size_t dataSize) const;

              private:
                // -- Internal helpers -----------------------------------------------------

                bool CreateDevice(const RHIDeviceDesc& desc);
                bool CreateCommandQueues();
                bool CreateDescriptorHeaps();
                bool CreateFrameResources();
                void DetectCapabilities();
                void DetectDXRSupport();

                DXGI_FORMAT ConvertFormat(PixelFormat format) const;
                D3D12_FILTER ConvertFilter(const RHISamplerDesc& desc) const;
                D3D12_TEXTURE_ADDRESS_MODE ConvertAddressMode(RHIAddressMode mode) const;
                D3D12_COMPARISON_FUNC ConvertCompareOp(RHICompareOp op) const;
                D3D12_STENCIL_OP ConvertStencilOp(RHIStencilOp op) const;
                D3D12_BLEND ConvertBlendFactor(RHIBlendFactor factor) const;
                D3D12_BLEND_OP ConvertBlendOp(RHIBlendOp op) const;
                DXGI_FORMAT ConvertVertexFormat(RHIVertexFormat format) const;
                uint32_t GetFormatSize(PixelFormat format) const;
                D3D12_RESOURCE_STATES GetInitialResourceState(RHIBufferAccess access) const;

                /**
                 * @brief Moves the frame index forward and waits for the oldest
                 *        in-flight frame to complete before reusing its allocator.
                 */
                void MoveToNextFrame();

                // -- DXGI & device --------------------------------------------------------

                ComPtr<IDXGIFactory6> m_dxgiFactory;
                ComPtr<IDXGIAdapter1> m_adapter;
                ComPtr<ID3D12Device> m_device;
                ComPtr<ID3D12Device5> m_dxrDevice; ///< Non-null when DXR is available.

                ComPtr<ID3D12InfoQueue> m_infoQueue; ///< Active when debug layer is enabled.

                // -- Command queues -------------------------------------------------------

                ComPtr<ID3D12CommandQueue> m_directQueue;
                ComPtr<ID3D12CommandQueue> m_copyQueue;
                ComPtr<ID3D12CommandQueue> m_computeQueue;

                // -- Synchronization ------------------------------------------------------

                D3D12Fence m_frameFence;
                std::mutex m_submitMutex;

                // -- Descriptor heaps -----------------------------------------------------

                DescriptorHeapAllocator m_cbvSrvUavHeap;
                DescriptorHeapAllocator m_rtvHeap;
                DescriptorHeapAllocator m_dsvHeap;
                DescriptorHeapAllocator m_samplerHeap;

                // -- Per-frame resources --------------------------------------------------

                std::array<FrameResources, MAX_FRAMES_IN_FLIGHT> m_frameResources;
                uint32_t m_currentFrameIndex = 0;

                // -- Immediate command list -----------------------------------------------

                std::unique_ptr<D3D12CommandList> m_immediateCommandList;

                // -- Capabilities & stats -------------------------------------------------

                RHIDeviceCapabilities m_capabilities;
                RHIStatistics m_statistics;
                bool m_debugEnabled = false;
                bool m_dxrSupported = false;

                // -- Deferred deletion queue ----------------------------------------------

                /**
                 * @brief Resources queued for deferred deletion.
                 *
                 * When a resource is destroyed it may still be referenced by an
                 * in-flight command list. The resource is moved into this queue
                 * along with the current fence value. Once the GPU passes that
                 * fence value the resource is released.
                 */
                struct DeferredRelease
                {
                    ComPtr<IUnknown> resource;
                    uint64_t fenceValue = 0;
                };
                std::queue<DeferredRelease> m_deferredReleaseQueue;
                std::mutex m_deferredReleaseMutex;

                /**
                 * @brief Processes the deferred-release queue, freeing resources
                 *        whose fence value has been reached by the GPU.
                 */
                void ProcessDeferredReleases();
            };

        } // namespace D3D12
    } // namespace RHI
} // namespace Spark

#endif // _WIN32
