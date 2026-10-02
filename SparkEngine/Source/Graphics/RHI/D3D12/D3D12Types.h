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
#include <deque>
#include <memory>
#include <mutex>
#include <span>
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

            /// Default descriptor heap sizes per type. All four are CPU-only: views are created
            /// here and copied into the shader-visible table pages (D3D12DescriptorPagePool) per draw.
            static constexpr uint32_t CBV_SRV_UAV_HEAP_SIZE = 1'000'000;
            static constexpr uint32_t RTV_HEAP_SIZE = 256;
            static constexpr uint32_t DSV_HEAP_SIZE = 64;
            static constexpr uint32_t SAMPLER_HEAP_SIZE = 2048;

            /**
             * @brief Layout of the root signature every RHI graphics pipeline uses
             *        (D3D12Device::CreateDefaultRootSignature).
             *
             * 8 root CBVs (2 DWORDs each) + 3 descriptor tables (1 DWORD each) = 19 of the
             * 64 root-signature DWORDs. All parameters are visible to every stage, so the RHI
             * stage argument of SetConstantBuffer/SetShaderResource/SetSampler selects no
             * separate binding space on D3D12: one slot number means one register for all stages.
             */
            namespace DefaultRootLayout
            {
                /// Root parameters 0..7 are root CBVs for registers b0..b7.
                static constexpr uint32_t kConstantBufferCount = 8;
                /// Root parameter of the SRV table (t0..t31).
                static constexpr uint32_t kShaderResourceTable = kConstantBufferCount;
                /// Root parameter of the sampler table (s0..s15).
                static constexpr uint32_t kSamplerTable = kConstantBufferCount + 1;
                /// Root parameter of the UAV table (u0..u7). Declared, not bound by the RHI yet.
                static constexpr uint32_t kUnorderedAccessTable = kConstantBufferCount + 2;
                static constexpr uint32_t kParameterCount = kConstantBufferCount + 3;

                static constexpr uint32_t kShaderResourceSlots = 32;
                static constexpr uint32_t kSamplerSlots = 16;
                static constexpr uint32_t kUnorderedAccessSlots = 8;
            } // namespace DefaultRootLayout

            /// Shader-visible table pages: 64 x 1024 CBV/SRV/UAV descriptors (32 SRV tables per
            /// page) and 16 x 128 samplers (8 sampler tables per page, 2048 = the D3D12 limit
            /// for a shader-visible sampler heap).
            static constexpr uint32_t SRV_TABLE_PAGE_SIZE = 1024;
            static constexpr uint32_t SRV_TABLE_PAGE_COUNT = 64;
            static constexpr uint32_t SAMPLER_TABLE_PAGE_SIZE = 128;
            static constexpr uint32_t SAMPLER_TABLE_PAGE_COUNT = 16;

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
            // DEFERRED RELEASE QUEUE
            // ============================================================================

            /**
             * @brief Fence-tagged release queue shared by a D3D12Device and the resources it creates.
             *
             * D3D12 command lists hold no references to the resources they use, so a resource
             * and the descriptor slots that name it must outlive every submission that touched
             * them. D3D12Buffer and D3D12Texture destructors hand their ComPtr and descriptor
             * allocations here instead of releasing them; Process() drops the COM reference and
             * returns the descriptor slots to their heaps once the frame fence has passed the
             * value recorded at destruction.
             *
             * - Thread affinity: async-safe; every method takes the internal mutex.
             * - Ownership: owned by D3D12Device through std::shared_ptr. Resources hold a
             *   std::weak_ptr, so a resource destroyed after its device has shut down releases
             *   immediately (Shutdown waited for the GPU and drained this queue first).
             * - Allocation: one deque node per destroyed resource; never on the per-draw path.
             * - Scalability: entries are released in fence order, O(1) per entry.
             */
            class D3D12DeferredReleaseQueue
            {
              public:
                /// Heap a descriptor allocation was taken from.
                enum class Heap : uint8_t
                {
                    CbvSrvUav,
                    Rtv,
                    Dsv
                };

                /// One descriptor range to recycle together with a resource.
                struct Descriptor
                {
                    Heap heap = Heap::CbvSrvUav;
                    DescriptorAllocation allocation;
                };

                /// Most descriptor ranges a single resource can carry (SRV, RTV, DSV, UAV).
                static constexpr size_t kMaxDescriptorsPerResource = 4;

                D3D12DeferredReleaseQueue(const D3D12Fence& fence, DescriptorHeapAllocator& cbvSrvUavHeap,
                                          DescriptorHeapAllocator& rtvHeap, DescriptorHeapAllocator& dsvHeap);

                /**
                 * @brief Queue a resource and its descriptors for release once the GPU has
                 *        finished every submission made before this call.
                 *
                 * The entry is tagged with the value the next fence Signal() will produce, so
                 * any work already submitted to the direct queue completes before release.
                 * @param resource     COM reference to release (may be null).
                 * @param descriptors  At most kMaxDescriptorsPerResource ranges; invalid ones are skipped.
                 */
                void Enqueue(ComPtr<IUnknown> resource, std::span<const Descriptor> descriptors);

                /// Queue a resource for release once the fence reaches @p fenceValue.
                void EnqueueAtFence(ComPtr<IUnknown> resource, uint64_t fenceValue);

                /// Release every entry whose fence value the GPU has completed.
                void Process();

                /// Release everything unconditionally. The caller must have waited for GPU idle.
                void ReleaseAll();

                /// Number of entries still waiting on the GPU (test seam and diagnostics).
                size_t GetPendingCount() const;

              private:
                struct Entry
                {
                    ComPtr<IUnknown> resource;
                    std::array<Descriptor, kMaxDescriptorsPerResource> descriptors = {};
                    uint32_t descriptorCount = 0;
                    uint64_t fenceValue = 0;
                };

                void ReleaseEntry(Entry& entry);
                DescriptorHeapAllocator& HeapFor(Heap heap);

                const D3D12Fence& m_fence;
                DescriptorHeapAllocator& m_cbvSrvUavHeap;
                DescriptorHeapAllocator& m_rtvHeap;
                DescriptorHeapAllocator& m_dsvHeap;
                mutable std::mutex m_mutex;
                std::deque<Entry> m_entries; ///< Non-decreasing fenceValue order.
            };

            // ============================================================================
            // SHADER-VISIBLE DESCRIPTOR TABLE PAGES
            // ============================================================================

            /**
             * @brief Fixed pool of equal-sized pages in one shader-visible descriptor heap.
             *
             * A command list takes a page, carves per-draw descriptor tables out of it linearly
             * and hands every page it filled back through Retire() when it is submitted. A
             * retired page returns to the free list only once the frame fence reaches the value
             * recorded at submission, so a table is never overwritten while the GPU reads it.
             *
             * - Thread affinity: async-safe (internal mutex); in practice the render thread.
             * - Ownership: shared by the D3D12Device and its command lists (D3D12DescriptorTables),
             *   so a command list that outlives its device never touches a freed heap.
             * - Allocation: the free list and retire queue are sized once in Initialize(); Acquire
             *   and Retire never allocate. The per-draw path does not touch the pool at all.
             * - Scalability: pageCount x pageSize descriptors in flight; exhaustion is reported
             *   by Acquire() returning false, never by reusing a live page.
             */
            class D3D12DescriptorPagePool
            {
              public:
                /// Creates the shader-visible heap. @p fence is the device frame fence.
                bool Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t pageSize,
                                uint32_t pageCount, ID3D12Fence* fence);

                /// Takes a free page, first reclaiming pages the GPU has finished with.
                /// @return False when every page is still in flight.
                bool Acquire(uint32_t& page);

                /// Returns @p pages to the pool once the frame fence reaches @p fenceValue
                /// (0 returns them immediately: pages the GPU never saw).
                void Retire(std::span<const uint32_t> pages, uint64_t fenceValue);

                ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
                uint32_t GetPageSize() const { return m_pageSize; }
                uint32_t GetDescriptorSize() const { return m_descriptorSize; }
                D3D12_CPU_DESCRIPTOR_HANDLE CpuStart(uint32_t page) const;
                D3D12_GPU_DESCRIPTOR_HANDLE GpuStart(uint32_t page) const;

                /// Free pages right now, after reclaiming completed ones (test seam and diagnostics).
                size_t GetFreePageCount();

              private:
                struct RetiredPage
                {
                    uint64_t fenceValue = 0;
                    uint32_t page = 0;
                };

                void ReclaimCompleted(); ///< Caller holds m_mutex.

                ComPtr<ID3D12DescriptorHeap> m_heap;
                ComPtr<ID3D12Fence> m_fence; ///< Keeps the fence valid for as long as the pool lives.
                D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
                D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {};
                uint32_t m_descriptorSize = 0;
                uint32_t m_pageSize = 0;
                std::vector<uint32_t> m_freePages;
                std::vector<RetiredPage> m_retired; ///< FIFO in [m_retiredHead, size); non-decreasing fence values.
                size_t m_retiredHead = 0;
                std::mutex m_mutex;
            };

            /**
             * @brief The shader-visible heaps command lists bind, plus the CPU null descriptors
             *        that fill unbound table slots. Created by D3D12Device::Initialize().
             */
            struct D3D12DescriptorTables
            {
                D3D12DescriptorPagePool shaderResources; ///< CBV_SRV_UAV pages (SRV tables).
                D3D12DescriptorPagePool samplers;        ///< Sampler pages (sampler tables).
                DescriptorHeapAllocator nullDescriptors; ///< CPU-only: one null Texture2D SRV.
                DescriptorHeapAllocator nullSamplers;    ///< CPU-only: one default point sampler.
                D3D12_CPU_DESCRIPTOR_HANDLE nullShaderResource = {};
                D3D12_CPU_DESCRIPTOR_HANDLE nullSampler = {};
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

                /// Hands the resources and descriptor to the release queue when one is attached;
                /// otherwise releases them immediately.
                ~D3D12Buffer() override;

                D3D12Buffer(const D3D12Buffer&) = delete;
                D3D12Buffer& operator=(const D3D12Buffer&) = delete;

                /// Route destruction through @p queue (set by the creating D3D12Device).
                void SetReleaseQueue(std::weak_ptr<D3D12DeferredReleaseQueue> queue)
                {
                    m_releaseQueue = std::move(queue);
                }

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

                /// CBV/SRV/UAV descriptor allocated from the CPU-only CBV/SRV/UAV heap.
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
                std::weak_ptr<D3D12DeferredReleaseQueue> m_releaseQueue;
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

                /// Hands the resource and its SRV/RTV/DSV/UAV slots to the release queue when one
                /// is attached; otherwise (swap-chain back buffers) releases them immediately.
                ~D3D12Texture() override;

                D3D12Texture(const D3D12Texture&) = delete;
                D3D12Texture& operator=(const D3D12Texture&) = delete;

                /// Route destruction through @p queue (set by the creating D3D12Device).
                void SetReleaseQueue(std::weak_ptr<D3D12DeferredReleaseQueue> queue)
                {
                    m_releaseQueue = std::move(queue);
                }

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
                std::weak_ptr<D3D12DeferredReleaseQueue> m_releaseQueue;
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
             * Resource binding (DefaultRootLayout): SetConstantBuffer stages a root CBV,
             * SetShaderResource/SetSampler stage CPU descriptor handles, and every draw first
             * applies what changed. Dirty SRV and sampler slots are copied, with null
             * descriptors in unbound slots, into a table carved from a page of the shader-visible
             * heaps (D3D12DescriptorTables) and bound with SetGraphicsRootDescriptorTable. Pages
             * are handed back at submission (D3D12Device::ExecuteCommandList) and reused only
             * after the frame fence passes. Bindings do not survive Begin()/Reset().
             *
             * Contract: render thread (one thread per list); no heap allocation per draw (fixed
             * staging arrays, pages from a fixed pool); a draw whose tables cannot be allocated
             * is logged and skipped rather than bound to stale descriptors. SetShaderResource
             * moves a texture that is not in a shader-resource state there (batched barrier), so a
             * render target can be sampled by a later pass as on D3D11; SetRenderTargets and the
             * clears move targets back to their write states. Compute root signatures are out of
             * scope: Dispatch binds nothing.
             */
            class D3D12CommandList : public IRHICommandList
            {
              public:
                D3D12CommandList(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type,
                                 std::shared_ptr<D3D12DescriptorTables> tables,
                                 ID3D12PipelineState* initialPSO = nullptr);
                /// Returns pages no submission used; submitted pages were retired at submission.
                ~D3D12CommandList() override;

                D3D12CommandList(const D3D12CommandList&) = delete;
                D3D12CommandList& operator=(const D3D12CommandList&) = delete;

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
                 * @brief Batches a transition of @p texture to @p state unless its tracked state
                 *        already includes it. SetRenderTargets, the clears and SetShaderResource use
                 *        it, so RHI callers never issue barriers (the swap-chain PRESENT
                 *        transition is not covered).
                 */
                void RequireState(D3D12Texture* texture, D3D12_RESOURCE_STATES state);

                /**
                 * @brief Inserts a UAV barrier for the given resource (or nullptr for all).
                 */
                void UAVBarrier(ID3D12Resource* resource = nullptr);

                /**
                 * @brief Flushes any pending resource barriers recorded via
                 *        TransitionBarrier() / UAVBarrier().
                 */
                void FlushBarriers();

                /**
                 * @brief Hands every descriptor page this list filled to the pool, reusable once
                 *        the frame fence reaches @p fenceValue. Called by the device right after
                 *        it submits this list.
                 */
                void RetireDescriptorPages(uint64_t fenceValue);

                ID3D12GraphicsCommandList* GetCommandList() const { return m_commandList.Get(); }
                ID3D12CommandAllocator* GetAllocator() const { return m_commandAllocator.Get(); }

              private:
                /// Linear cursor into the page a list is currently filling.
                struct TablePage
                {
                    uint32_t page = UINT32_MAX;
                    uint32_t used = 0;
                };

                /// Maximum pages one list can hold between submissions.
                static constexpr size_t kMaxHeldPages = 32;

                /// Applies staged bindings before a draw. False: the draw must be skipped.
                bool PrepareDraw();
                /// Copies @p slots (null descriptor where unbound) into a fresh table and binds it.
                bool BindTable(D3D12DescriptorPagePool& pool, TablePage& cursor,
                               std::array<uint32_t, kMaxHeldPages>& held, uint32_t& heldCount,
                               const D3D12_CPU_DESCRIPTOR_HANDLE* slots, uint32_t slotCount,
                               D3D12_CPU_DESCRIPTOR_HANDLE nullDescriptor, D3D12_DESCRIPTOR_HEAP_TYPE heapType,
                               uint32_t rootParameter);
                /// Clears staged bindings (a reset list has no root arguments).
                void ResetBindings();
                /// Binds the shader-visible heaps on a freshly reset list.
                void BindDescriptorHeaps();

                ComPtr<ID3D12GraphicsCommandList> m_commandList;
                ComPtr<ID3D12CommandAllocator> m_commandAllocator;
                D3D12_COMMAND_LIST_TYPE m_type;
                ID3D12Device* m_device = nullptr;
                std::shared_ptr<D3D12DescriptorTables> m_tables;

                // -- Staged bindings (DefaultRootLayout) ---------------------------------
                std::array<D3D12_GPU_VIRTUAL_ADDRESS, DefaultRootLayout::kConstantBufferCount> m_constantBuffers = {};
                uint32_t m_dirtyConstantBuffers = 0; ///< Bit i: root CBV i must be re-set.
                std::array<D3D12_CPU_DESCRIPTOR_HANDLE, DefaultRootLayout::kShaderResourceSlots> m_shaderResources = {};
                std::array<D3D12_CPU_DESCRIPTOR_HANDLE, DefaultRootLayout::kSamplerSlots> m_samplers = {};
                uint32_t m_boundShaderResources = 0; ///< Bit i: SRV slot i holds a view.
                uint32_t m_boundSamplers = 0;        ///< Bit i: sampler slot i holds a sampler.
                bool m_shaderResourcesDirty = false;
                bool m_samplersDirty = false;

                // -- Shader-visible table pages held until submission ---------------------
                TablePage m_shaderResourcePage;
                TablePage m_samplerPage;
                std::array<uint32_t, kMaxHeldPages> m_heldShaderResourcePages = {};
                std::array<uint32_t, kMaxHeldPages> m_heldSamplerPages = {};
                uint32_t m_heldShaderResourcePageCount = 0;
                uint32_t m_heldSamplerPageCount = 0;

                /// ExecuteIndirect signature for D3D12_DRAW_ARGUMENTS (null if creation failed).
                ComPtr<ID3D12CommandSignature> m_drawSignature;
                /// ExecuteIndirect signature for D3D12_DRAW_INDEXED_ARGUMENTS (null if creation failed).
                ComPtr<ID3D12CommandSignature> m_drawIndexedSignature;
                /// ExecuteIndirect signature for D3D12_DISPATCH_ARGUMENTS (null if creation failed).
                ComPtr<ID3D12CommandSignature> m_dispatchSignature;

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
