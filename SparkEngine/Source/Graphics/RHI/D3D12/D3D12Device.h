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

// MinGW's d3d12.h only defines up to ID3D12Device1. Stub the newer
// interfaces so the header compiles — DXR features are disabled at runtime.
#if defined(__MINGW32__) && !defined(__ID3D12Device5_FWD_DEFINED__)
#define __ID3D12Device5_FWD_DEFINED__
typedef ID3D12Device1 ID3D12Device5; // Safe stub — never actually used under Wine
#endif

#include "D3D12Types.h"

#include <queue>
#include <unordered_map>

namespace Spark
{
    namespace RHI
    {
        namespace D3D12
        {

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

                [[nodiscard]] std::unique_ptr<IRHISwapChain> CreateSwapChain(const RHISwapChainDesc& desc) override;

                // -- IRHIDevice: Resource creation ----------------------------------------

                [[nodiscard]] std::unique_ptr<IRHIBuffer> CreateBuffer(const RHIBufferDesc& desc) override;
                [[nodiscard]] std::unique_ptr<IRHITexture> CreateTexture(const RHITextureDesc& desc) override;
                [[nodiscard]] std::unique_ptr<IRHITexture> WrapNativeTexture(void* nativeHandle,
                                                                             const RHITextureDesc& desc) override;
                [[nodiscard]] std::unique_ptr<IRHIShader> CreateShader(const RHIShaderDesc& desc) override;
                [[nodiscard]] std::unique_ptr<IRHISampler> CreateSampler(const RHISamplerDesc& desc) override;
                [[nodiscard]] std::unique_ptr<IRHIPipelineState> CreatePipelineState(const RHIPipelineStateDesc& desc,
                                                                                     IRHIShader* vertexShader,
                                                                                     IRHIShader* pixelShader) override;

                // -- D3D12-specific: Deferred GPU resource release ----------------------
                // These enqueue GPU resources for deferred deletion (fence-synchronized)
                // since D3D12 resources may still be in-flight on the GPU when destroyed.

                void DeferredReleaseBuffer(D3D12Buffer* buffer);
                void DeferredReleaseTexture(D3D12Texture* texture);

                // -- IRHIDevice: Resource updates -----------------------------------------

                void* MapBuffer(IRHIBuffer* buffer) override;
                void UnmapBuffer(IRHIBuffer* buffer) override;
                void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset) override;
                void UpdateTexture(IRHITexture* texture, const void* data, uint32_t mipLevel,
                                   uint32_t arraySlice) override;

                // -- IRHIDevice: Command lists --------------------------------------------

                IRHICommandList* GetImmediateCommandList() override;
                [[nodiscard]] std::unique_ptr<IRHICommandList> CreateDeferredCommandList() override;
                void ExecuteCommandList(IRHICommandList* commandList) override;

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
                bool IsSoftwareDevice() const { return m_isSoftwareDevice; }

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
                bool m_isSoftwareDevice = false;

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
