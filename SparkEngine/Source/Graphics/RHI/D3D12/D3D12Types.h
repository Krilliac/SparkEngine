/**
 * @file D3D12Types.h
 * @brief D3D12 type definitions, constants, and lightweight resource wrappers
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains all D3D12 RHI enums, structs, small utility classes (descriptor
 * heap allocator, fence wrapper), and resource implementation classes
 * (buffer, texture, shader, sampler, pipeline state, swap chain, command
 * list, per-frame resources). These are separated from D3D12Device.h to
 * keep the device class focused on its own responsibilities.
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
                D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {}; ///< CPU-side handle for resource binding and copying.
                D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle =
                    {};             ///< GPU-side handle for shader-visible heaps (0 if non-visible).
                uint32_t index = 0; ///< Zero-based index into the parent DescriptorHeapAllocator.
                uint32_t count = 0; ///< Number of contiguous descriptors in this allocation.

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
                ComPtr<ID3D12DescriptorHeap> m_heap;         ///< Underlying D3D12 descriptor heap.
                D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {}; ///< CPU handle of descriptor 0 in the heap.
                D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {}; ///< GPU handle of descriptor 0 (0 if non-shader-visible).
                uint32_t m_descriptorSize = 0;               ///< Byte stride between consecutive descriptors.
                uint32_t m_capacity = 0;                     ///< Total descriptor count in the heap.
                uint32_t m_nextFreeIndex = 0;                ///< Bump-pointer for sequential allocation.
                std::vector<uint32_t> m_freeList;            ///< Returned indices available for reuse.
                std::mutex m_mutex;                          ///< Guards concurrent Allocate/Free calls.
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
                ComPtr<ID3D12Fence> m_fence;   ///< The underlying D3D12 fence object.
                HANDLE m_fenceEvent = nullptr; ///< Win32 event used to block CPU in WaitForValue().
                uint64_t m_currentValue = 0;   ///< Last value passed to Signal(); monotonically increasing.
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
                    return m_srvDescriptor.IsValid() ? &m_srvDescriptor.cpuHandle : nullptr;
                }
                void* GetRenderTargetView() const override
                {
                    return m_rtvDescriptor.IsValid() ? &m_rtvDescriptor.cpuHandle : nullptr;
                }
                void* GetDepthStencilView() const override
                {
                    return m_dsvDescriptor.IsValid() ? &m_dsvDescriptor.cpuHandle : nullptr;
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
                mutable DescriptorAllocation m_srvDescriptor;
                mutable DescriptorAllocation m_rtvDescriptor;
                mutable DescriptorAllocation m_dsvDescriptor;
                mutable DescriptorAllocation m_uavDescriptor;
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
                void* GetNativeHandle() const override { return &m_descriptor.cpuHandle; }

                const DescriptorAllocation& GetDescriptor() const { return m_descriptor; }

              private:
                RHISamplerDesc m_desc;
                mutable DescriptorAllocation m_descriptor;
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

                /// Currently bound PSO (cached to avoid redundant pipeline state binds).
                ID3D12PipelineState* m_currentPSO = nullptr;
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

        } // namespace D3D12
    } // namespace RHI
} // namespace Spark

#endif // _WIN32
