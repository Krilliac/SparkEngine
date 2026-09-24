/**
 * @file VulkanDevice.h
 * @brief Vulkan implementation of the RHI device interface
 * @author Spark Engine Team
 * @date 2025
 *
 * Vulkan 1.4 backend (with 1.3 fallback) for the RHI abstraction layer.
 * Supports modern Vulkan features including dynamic rendering, push
 * descriptors, timeline semaphores, host image copy, and descriptor indexing.
 */

#pragma once

#include "../RHIDevice.h"
#include "../RHIDeviceBase.h"
#include "../RHIResources.h"

// Vulkan availability is checked at build time
#ifdef SPARK_VULKAN_SUPPORT

// Platform detection for Vulkan surface creation.
//
// On Linux we want the union of XCB + Xlib + Wayland surface support
// because SDL2 decides at runtime which windowing-system surface to
// create (xlib by default, xcb/wayland if the env has been configured
// that way). Enabling all three at compile time means
// SDL_Vulkan_CreateSurface can pick whichever the host actually uses.
//
// Guard each platform define behind __has_include so we don't pull in
// headers that aren't installed (e.g. CI has libvulkan-dev but not
// libwayland-dev, and VK_USE_PLATFORM_WAYLAND_KHR would make
// <vulkan/vulkan.h> try to #include <wayland-client.h>).
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__linux__)
#define VK_USE_PLATFORM_XCB_KHR
#if __has_include(<X11/Xlib.h>)
#define VK_USE_PLATFORM_XLIB_KHR
#endif
#if __has_include(<wayland-client.h>)
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#elif defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#endif

#include <vulkan/vulkan.h>

// Xlib's headers — pulled in transitively on Linux via
// VK_USE_PLATFORM_XLIB_KHR — #define a handful of unqualified macros
// (`None`, `Status`, `Success`, `Bool`, `True`, `False`, `Always`) that
// collide with identifiers in the engine (e.g. `RHICullMode::None`).
// Undefine them immediately after the Vulkan include so the rest of
// the engine isn't poisoned. This is the standard mitigation used by
// every cross-platform C++ codebase that has to coexist with Xlib.
#if defined(__linux__) && defined(VK_USE_PLATFORM_XLIB_KHR)
#ifdef None
#undef None
#endif
#ifdef Status
#undef Status
#endif
#ifdef Success
#undef Success
#endif
#ifdef Bool
#undef Bool
#endif
#ifdef True
#undef True
#endif
#ifdef False
#undef False
#endif
#ifdef Always
#undef Always
#endif
#endif
#include <vector>
#include <unordered_map>
#include <optional>
#include <functional>

namespace Spark
{
    namespace RHI
    {
        namespace Vulkan
        {

            // ============================================================================
            // VULKAN QUEUE FAMILIES
            // ============================================================================

            struct QueueFamilyIndices
            {
                std::optional<uint32_t> graphicsFamily;
                std::optional<uint32_t> presentFamily;
                std::optional<uint32_t> computeFamily;
                std::optional<uint32_t> transferFamily;

                bool IsComplete() const { return graphicsFamily.has_value() && presentFamily.has_value(); }
            };

            // ============================================================================
            // VULKAN RESOURCE IMPLEMENTATIONS
            // ============================================================================

            class VulkanBuffer : public IRHIBuffer
            {
              public:
                VulkanBuffer(const RHIBufferDesc& desc, VkBuffer buffer, VkDeviceMemory memory, VkDevice device);
                ~VulkanBuffer() override;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return m_buffer != VK_NULL_HANDLE; }

                const RHIBufferDesc& GetDesc() const override { return m_desc; }
                uint64_t GetSize() const override { return m_desc.size; }
                uint32_t GetStride() const override { return m_desc.stride; }
                void* GetNativeHandle() const override { return reinterpret_cast<void*>(m_buffer); }

                VkBuffer GetVkBuffer() const { return m_buffer; }
                VkDeviceMemory GetVkMemory() const { return m_memory; }
                void* GetMappedPtr() const { return m_mappedPtr; }
                /// Dynamic/Staging/ReadBack buffers live in host-visible memory; Static buffers are device-local
                /// and can only be written through a staging copy.
                bool IsHostVisible() const { return m_desc.access != RHIBufferAccess::Static; }
                void SetMappedPtr(void* ptr) { m_mappedPtr = ptr; }

              private:
                RHIBufferDesc m_desc;
                VkBuffer m_buffer;
                VkDeviceMemory m_memory;
                VkDevice m_deviceRef;
                void* m_mappedPtr = nullptr;
            };

            class VulkanTexture : public IRHITexture
            {
              public:
                VulkanTexture(const RHITextureDesc& desc, VkImage image, VkDeviceMemory memory, VkImageView imageView,
                              VkDevice device, bool ownsImage = true);
                ~VulkanTexture() override;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return m_image != VK_NULL_HANDLE; }

                const RHITextureDesc& GetDesc() const override { return m_desc; }
                uint32_t GetWidth() const override { return m_desc.width; }
                uint32_t GetHeight() const override { return m_desc.height; }
                uint32_t GetDepth() const override { return m_desc.depth; }
                uint32_t GetMipLevels() const override { return m_desc.mipLevels; }
                PixelFormat GetFormat() const override { return m_desc.format; }
                void* GetNativeHandle() const override { return reinterpret_cast<void*>(m_image); }
                void* GetShaderResourceView() const override { return reinterpret_cast<void*>(m_imageView); }
                void* GetRenderTargetView() const override { return reinterpret_cast<void*>(m_imageView); }
                void* GetDepthStencilView() const override { return reinterpret_cast<void*>(m_imageView); }

                VkImage GetVkImage() const { return m_image; }
                VkImageView GetVkImageView() const { return m_imageView; }
                VkImageLayout GetCurrentLayout() const { return m_currentLayout; }
                void SetCurrentLayout(VkImageLayout layout) { m_currentLayout = layout; }

              private:
                RHITextureDesc m_desc;
                VkImage m_image;
                VkDeviceMemory m_memory;
                VkImageView m_imageView;
                VkDevice m_deviceRef;
                VkImageLayout m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                bool m_ownsImage;
            };

            class VulkanShader : public IRHIShader
            {
              public:
                VulkanShader(const RHIShaderDesc& desc, VkShaderModule module, VkDevice device,
                             std::vector<uint8_t> spirvCode);
                ~VulkanShader() override;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return m_module != VK_NULL_HANDLE; }

                RHIShaderStage GetStage() const override { return m_desc.stage; }
                const std::string& GetEntryPoint() const override { return m_desc.entryPoint; }
                void* GetNativeHandle() const override { return reinterpret_cast<void*>(m_module); }
                const void* GetBytecode() const override { return m_spirvCode.data(); }
                size_t GetBytecodeSize() const override { return m_spirvCode.size(); }

                VkShaderModule GetVkModule() const { return m_module; }

              private:
                RHIShaderDesc m_desc;
                VkShaderModule m_module;
                VkDevice m_deviceRef;
                std::vector<uint8_t> m_spirvCode;
            };

            class VulkanSampler : public IRHISampler
            {
              public:
                VulkanSampler(const RHISamplerDesc& desc, VkSampler sampler, VkDevice device);
                ~VulkanSampler() override;

                const std::string& GetDebugName() const override { return m_debugName; }
                void SetDebugName(const std::string& name) override { m_debugName = name; }
                bool IsValid() const override { return m_sampler != VK_NULL_HANDLE; }

                const RHISamplerDesc& GetDesc() const override { return m_desc; }
                void* GetNativeHandle() const override { return reinterpret_cast<void*>(m_sampler); }

                VkSampler GetVkSampler() const { return m_sampler; }

              private:
                RHISamplerDesc m_desc;
                VkSampler m_sampler;
                VkDevice m_deviceRef;
                std::string m_debugName;
            };

            class VulkanPipelineState : public IRHIPipelineState
            {
              public:
                VulkanPipelineState(const RHIPipelineStateDesc& desc, VkPipeline pipeline, VkPipelineLayout layout,
                                    VkDevice device);
                ~VulkanPipelineState() override;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return m_pipeline != VK_NULL_HANDLE; }

                const RHIPipelineStateDesc& GetDesc() const override { return m_desc; }
                void* GetNativeHandle() const override { return reinterpret_cast<void*>(m_pipeline); }

                VkPipeline GetVkPipeline() const { return m_pipeline; }
                VkPipelineLayout GetVkLayout() const { return m_layout; }

              private:
                RHIPipelineStateDesc m_desc;
                VkPipeline m_pipeline;
                VkPipelineLayout m_layout;
                VkDevice m_deviceRef;
            };

            // ============================================================================
            // VULKAN SWAP CHAIN
            // ============================================================================

            class VulkanSwapChain : public IRHISwapChain
            {
              public:
                /// Takes ownership of @p surface; it is destroyed with @p instance when the swap chain dies.
                VulkanSwapChain(VkInstance instance, VkDevice device, VkPhysicalDevice physDevice, VkSurfaceKHR surface,
                                const RHISwapChainDesc& desc, const QueueFamilyIndices& queueFamilies,
                                VkQueue presentQueue);
                ~VulkanSwapChain() override;

                /// Transitions the acquired image to PRESENT_SRC and queues it. Waits for the present queue to
                /// drain first, because engine submissions do not signal a per-image semaphore.
                bool Present(bool vsync) override;
                /// Recreates the chain at the clamped size, retiring the old one via oldSwapchain. A 0x0 size
                /// (minimized window) keeps the current chain and returns false.
                bool Resize(uint32_t width, uint32_t height) override;
                /// Acquires the next image on first use each frame, so the returned texture is always owned
                /// by the application when it is recorded into.
                IRHITexture* GetBackBuffer() override;
                PixelFormat GetFormat() const override { return m_desc.format; }
                uint32_t GetWidth() const override { return m_desc.width; }
                uint32_t GetHeight() const override { return m_desc.height; }
                uint32_t GetCurrentBufferIndex() const override { return m_currentImageIndex; }

                VkSwapchainKHR GetVkSwapChain() const { return m_swapChain; }
                bool IsValid() const { return m_swapChain != VK_NULL_HANDLE; }

                /// Acquire the next swap chain image (host-synchronized); returns false if a resize is needed.
                bool AcquireNextImage();

              private:
                bool CreateSwapChain(VkSwapchainKHR oldSwapChain);
                bool CreateImageViews();
                bool CreateSyncObjects();
                void DestroyImageViews();
                void Cleanup();

                RHISwapChainDesc m_desc;
                VkInstance m_instance;
                VkDevice m_device;
                VkPhysicalDevice m_physDevice;
                VkSurfaceKHR m_surface;
                VkSwapchainKHR m_swapChain = VK_NULL_HANDLE;
                VkFormat m_vkFormat = VK_FORMAT_B8G8R8A8_UNORM;
                QueueFamilyIndices m_queueFamilies;
                VkQueue m_presentQueue = VK_NULL_HANDLE;

                std::vector<VkImage> m_swapChainImages;
                std::vector<std::unique_ptr<VulkanTexture>> m_backBuffers;
                uint32_t m_currentImageIndex = 0;
                bool m_imageAcquired = false;

                // Host-waited acquire fence and a one-shot command buffer for the PRESENT_SRC transition.
                VkFence m_acquireFence = VK_NULL_HANDLE;
                VkFence m_transitionFence = VK_NULL_HANDLE;
                VkCommandPool m_transitionPool = VK_NULL_HANDLE;
                VkCommandBuffer m_transitionCmd = VK_NULL_HANDLE;
            };

            // ============================================================================
            // VULKAN COMMAND LIST
            // ============================================================================

            class VulkanCommandList : public IRHICommandList
            {
              public:
                /// @param descriptorPool Pool for the non-push descriptor path (VK_NULL_HANDLE when push
                ///        descriptors are used). @param bindingLayout Set layout matching every pipeline layout.
                VulkanCommandList(VkDevice device, VkCommandPool commandPool, bool isImmediate,
                                  RHIStatistics* statistics = nullptr,
                                  PFN_vkCmdPushDescriptorSetKHR pushDescriptorFn = nullptr,
                                  VkDescriptorPool descriptorPool = VK_NULL_HANDLE,
                                  VkDescriptorSetLayout bindingLayout = VK_NULL_HANDLE);
                ~VulkanCommandList() override;

                void Begin() override;
                void End() override;
                void Reset() override;

                /// Records the targets; dynamic rendering begins lazily at the next draw and is suspended
                /// around clears, copies and layout transitions.
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

                /// Barrier from the texture's tracked layout to @p newLayout (no-op if already there).
                /// Suspends dynamic rendering, since image barriers are not allowed inside it.
                void TransitionTexture(VulkanTexture* texture, VkImageLayout newLayout);

                /// Records a barrier moving @p texture from its tracked layout to @p newLayout into @p cmd and
                /// updates the tracking. Layouts are tracked at record time, so recordings touching the same
                /// texture must be submitted in the order they were recorded.
                static void RecordTextureTransition(VkCommandBuffer cmd, VulkanTexture* texture,
                                                    VkImageLayout newLayout);

                VkCommandBuffer GetVkCommandBuffer() const { return m_commandBuffer; }

                /// True once End() closed a recording that has not been submitted yet.
                bool IsExecutable() const { return m_executable; }
                /// Resets and returns the fence to pass to vkQueueSubmit; marks the list pending.
                VkFence PrepareSubmit();
                /// Blocks until the last submission finished and releases its descriptor sets.
                void WaitForCompletion();

              private:
                void ReleaseDescriptorSets(); ///< Frees pool sets of a completed/discarded recording
                void ResumeRendering();
                void SuspendRendering();
                void FlushBindings();

                VkDevice m_device;
                VkCommandPool m_commandPool;
                VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
                bool m_isImmediate;
                bool m_isRecording = false;
                bool m_executable = false;
                bool m_pending = false;
                RHIStatistics* m_statistics = nullptr;

                // Signaled when the last submission of this command buffer completes.
                VkFence m_submitFence = VK_NULL_HANDLE;

                // Push descriptor function (Vulkan 1.4 core / VK_KHR_push_descriptor)
                PFN_vkCmdPushDescriptorSetKHR m_vkCmdPushDescriptorSet = nullptr;
                VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
                VkDescriptorSetLayout m_bindingLayout = VK_NULL_HANDLE;
                // Pool-path sets referenced by the recording; freed only after the submission completes.
                std::vector<VkDescriptorSet> m_liveDescriptorSets;

                // Bound attachments and whether vkCmdBeginRendering is currently open.
                std::vector<VulkanTexture*> m_colorTargets;
                VulkanTexture* m_depthTarget = nullptr;
                bool m_renderingActive = false;

                // Tracked pipeline state for redundant bind elimination (reset at Begin)
                VkPipeline m_currentPipeline = VK_NULL_HANDLE;
                VkPipelineLayout m_currentPipelineLayout = VK_NULL_HANDLE;

                // Pending resource bindings (flushed before draw/dispatch)
                struct PendingBindings
                {
                    std::unordered_map<uint32_t, VkBuffer> constantBuffers;     // slot -> buffer
                    std::unordered_map<uint32_t, uint64_t> constantBufferSizes; // slot -> size
                    std::unordered_map<uint32_t, VkImageView> shaderResources;  // slot -> imageView
                    std::unordered_map<uint32_t, VkSampler> samplers;           // slot -> sampler
                    bool dirty = false;
                };
                PendingBindings m_pendingBindings;
            };

            // ============================================================================
            // VULKAN DEVICE
            // ============================================================================

            class VulkanDevice : public RHIDeviceBase
            {
              public:
                struct D3D11ParityMilestones
                {
                    bool frameLifecycle = false;
                    bool resourceBarriersAndSynchronization = false;
                    bool descriptorBindingModel = false;
                    bool shadowAndDeferredPassRoute = false;
                    bool postProcessRoute = false;
                    bool goldenSceneRenderRoute = false;
                    bool ciVulkanPresetAssertion = false;
                    bool ciShaderCompilePathAssertion = false;
                };

                VulkanDevice();
                ~VulkanDevice() override;

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

                GraphicsBackend GetBackendType() const override { return GraphicsBackend::Vulkan; }
                std::string GetDeviceInfo() const override;

                // Vulkan-specific accessors
                VkDevice GetVkDevice() const { return m_device; }
                VkPhysicalDevice GetVkPhysicalDevice() const { return m_physicalDevice; }
                VkInstance GetVkInstance() const { return m_instance; }
                bool IsSoftwareDevice() const { return m_isSoftwareDevice; }
                bool IsVulkan14() const { return m_vulkan14Available; }
                bool SupportsPushDescriptors() const { return m_pushDescriptorSupported; }
                bool SupportsHostImageCopy() const { return m_hostImageCopySupported; }
                /// VK_EXT_headless_surface was enabled, so a VulkanSwapChain can be built without a window.
                bool SupportsHeadlessSurface() const { return m_headlessSurfaceEnabled; }
                D3D11ParityMilestones GetD3D11ParityMilestones() const;
                std::vector<uint8_t> RenderCanonicalGoldenScene(uint32_t width, uint32_t height) const;
                /// Copies mip 0 of a color texture to host memory as tightly packed rows (blocking). Returns an
                /// empty vector for depth, compressed or unsupported formats, or textures without TransferSrc.
                std::vector<uint8_t> ReadbackTexture(IRHITexture* texture);
                VkQueue GetGraphicsQueue() const { return m_graphicsQueue; }
                VkQueue GetPresentQueue() const { return m_presentQueue; }
                VkCommandPool GetCommandPool() const { return m_commandPool; }
                const QueueFamilyIndices& GetQueueFamilies() const { return m_queueFamilies; }

                // Memory helpers
                uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

                // Descriptor set management for resource binding (VK_NULL_HANDLE when the binding layout is a
                // push-descriptor layout, which cannot back pool-allocated sets)
                VkDescriptorSet AllocateDescriptorSet();
                VkDescriptorSetLayout GetBindingLayout() const { return m_bindingLayout; }
                VkPipelineLayout GetDefaultPipelineLayout() const { return m_defaultPipelineLayout; }

              private:
                bool CreateInstance(const RHIDeviceDesc& desc);
                bool SelectPhysicalDevice();
                bool CreateLogicalDevice();
                bool CreateCommandPool();
                bool CreateDescriptorSetLayout();
                QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device) const;
                /// Records @p record into a one-shot command buffer, submits it and waits (10s bound).
                bool SubmitOneShot(const std::function<void(VkCommandBuffer)>& record);
                /// Creates a host-visible, host-coherent buffer (caller destroys both handles).
                bool CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                                      VkDeviceMemory& memory);
                /// Copies @p size bytes into a device-local buffer through a temporary staging buffer.
                bool UploadViaStaging(VkBuffer destination, const void* data, VkDeviceSize size, VkDeviceSize offset);
                bool CheckDeviceExtensionSupport(VkPhysicalDevice device) const;
                void QueryCapabilities();

                // Format conversion
                VkFormat ConvertFormat(PixelFormat format) const;
                VkFilter ConvertFilter(RHIFilterMode mode) const;
                VkSamplerAddressMode ConvertAddressMode(RHIAddressMode mode) const;
                VkCompareOp ConvertCompareOp(RHICompareOp op) const;
                VkStencilOp ConvertStencilOp(RHIStencilOp op) const;
                VkBlendFactor ConvertBlendFactor(RHIBlendFactor factor) const;
                VkBlendOp ConvertBlendOp(RHIBlendOp op) const;
                VkPrimitiveTopology ConvertTopology(RHIPrimitiveTopology topology) const;
                VkFormat ConvertVertexFormat(RHIVertexFormat format) const;
                VkBorderColor ConvertBorderColor(const float borderColor[4]) const;

                VkInstance m_instance = VK_NULL_HANDLE;
                VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
                VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
                VkDevice m_device = VK_NULL_HANDLE;
                VkQueue m_graphicsQueue = VK_NULL_HANDLE;
                VkQueue m_presentQueue = VK_NULL_HANDLE;
                VkQueue m_computeQueue = VK_NULL_HANDLE;
                VkQueue m_transferQueue = VK_NULL_HANDLE;
                VkCommandPool m_commandPool = VK_NULL_HANDLE;

                QueueFamilyIndices m_queueFamilies;

                std::unique_ptr<VulkanCommandList> m_immediateCommandList;
                VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
                VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;

                // Descriptor set layout for resource binding
                VkDescriptorSetLayout m_bindingLayout = VK_NULL_HANDLE;
                VkPipelineLayout m_defaultPipelineLayout = VK_NULL_HANDLE;

                // Frame synchronization
                static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
                uint32_t m_currentFrame = 0;
                std::vector<VkFence> m_frameFences;
                std::vector<VkSemaphore> m_renderFinishedSemaphores;

                // Upload fence — used for async staging copies instead of vkQueueWaitIdle
                VkFence m_uploadFence = VK_NULL_HANDLE;

                // Debug utilities function pointers
                PFN_vkCmdBeginDebugUtilsLabelEXT m_vkCmdBeginDebugUtilsLabel = nullptr;
                PFN_vkCmdEndDebugUtilsLabelEXT m_vkCmdEndDebugUtilsLabel = nullptr;
                PFN_vkCmdInsertDebugUtilsLabelEXT m_vkCmdInsertDebugUtilsLabel = nullptr;

                // Push descriptor function pointer (Vulkan 1.4 core / VK_KHR_push_descriptor)
                PFN_vkCmdPushDescriptorSetKHR m_vkCmdPushDescriptorSet = nullptr;

                bool m_validationEnabled = false;
                bool m_headlessSurfaceEnabled = false;
                bool m_isSoftwareDevice = false;
                bool m_vulkan14Available = false;
                bool m_pushDescriptorSupported = false;
                bool m_hostImageCopySupported = false;

                const std::vector<const char*> m_deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
            };

        } // namespace Vulkan
    } // namespace RHI
} // namespace Spark

#endif // SPARK_VULKAN_SUPPORT
