/**
 * @file VulkanDevice.cpp
 * @brief Vulkan RHI backend implementation
 * @author Spark Engine Team
 * @date 2025
 *
 * Complete Vulkan 1.3 device implementation with instance creation,
 * physical device selection, logical device, command pools, and
 * all resource creation/management functionality.
 */

#ifdef SPARK_VULKAN_SUPPORT

#include "VulkanDevice.h"
#include "../../../Utils/Validate.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <set>
#include <iostream>

namespace Spark
{
    namespace RHI
    {
        namespace Vulkan
        {

            // ============================================================================
            // VALIDATION LAYER CALLBACK
            // ============================================================================

            static VKAPI_ATTR VkBool32 VKAPI_CALL
            VulkanDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* userData)
            {
                if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                {
                    std::cerr << "[Vulkan Validation] " << callbackData->pMessage << std::endl;
                }
                return VK_FALSE;
            }

            // ============================================================================
            // VULKAN BUFFER
            // ============================================================================

            VulkanBuffer::VulkanBuffer(const RHIBufferDesc& desc, VkBuffer buffer, VkDeviceMemory memory,
                                       VkDevice device)
                : m_desc(desc), m_buffer(buffer), m_memory(memory), m_deviceRef(device)
            {
            }

            VulkanBuffer::~VulkanBuffer()
            {
                if (m_buffer != VK_NULL_HANDLE)
                {
                    vkDestroyBuffer(m_deviceRef, m_buffer, nullptr);
                    m_buffer = VK_NULL_HANDLE;
                }
                if (m_memory != VK_NULL_HANDLE)
                {
                    vkFreeMemory(m_deviceRef, m_memory, nullptr);
                    m_memory = VK_NULL_HANDLE;
                }
            }

            // ============================================================================
            // VULKAN TEXTURE
            // ============================================================================

            VulkanTexture::VulkanTexture(const RHITextureDesc& desc, VkImage image, VkDeviceMemory memory,
                                         VkImageView imageView, VkDevice device, bool ownsImage)
                : m_desc(desc), m_image(image), m_memory(memory), m_imageView(imageView), m_deviceRef(device),
                  m_ownsImage(ownsImage)
            {
            }

            VulkanTexture::~VulkanTexture()
            {
                if (m_imageView != VK_NULL_HANDLE)
                {
                    vkDestroyImageView(m_deviceRef, m_imageView, nullptr);
                }
                if (m_ownsImage)
                {
                    if (m_image != VK_NULL_HANDLE)
                        vkDestroyImage(m_deviceRef, m_image, nullptr);
                    if (m_memory != VK_NULL_HANDLE)
                        vkFreeMemory(m_deviceRef, m_memory, nullptr);
                }
            }

            // ============================================================================
            // VULKAN SHADER
            // ============================================================================

            VulkanShader::VulkanShader(const RHIShaderDesc& desc, VkShaderModule module, VkDevice device,
                                       std::vector<uint8_t> spirvCode)
                : m_desc(desc), m_module(module), m_deviceRef(device), m_spirvCode(std::move(spirvCode))
            {
            }

            VulkanShader::~VulkanShader()
            {
                if (m_module != VK_NULL_HANDLE)
                {
                    vkDestroyShaderModule(m_deviceRef, m_module, nullptr);
                }
            }

            // ============================================================================
            // VULKAN SAMPLER
            // ============================================================================

            VulkanSampler::VulkanSampler(const RHISamplerDesc& desc, VkSampler sampler, VkDevice device)
                : m_desc(desc), m_sampler(sampler), m_deviceRef(device)
            {
            }

            VulkanSampler::~VulkanSampler()
            {
                if (m_sampler != VK_NULL_HANDLE)
                {
                    vkDestroySampler(m_deviceRef, m_sampler, nullptr);
                }
            }

            // ============================================================================
            // VULKAN PIPELINE STATE
            // ============================================================================

            VulkanPipelineState::VulkanPipelineState(const RHIPipelineStateDesc& desc, VkPipeline pipeline,
                                                     VkPipelineLayout layout, VkDevice device)
                : m_desc(desc), m_pipeline(pipeline), m_layout(layout), m_deviceRef(device)
            {
            }

            VulkanPipelineState::~VulkanPipelineState()
            {
                if (m_pipeline != VK_NULL_HANDLE)
                    vkDestroyPipeline(m_deviceRef, m_pipeline, nullptr);
                if (m_layout != VK_NULL_HANDLE)
                    vkDestroyPipelineLayout(m_deviceRef, m_layout, nullptr);
            }

            // ============================================================================
            // VULKAN SWAP CHAIN
            // ============================================================================

            VulkanSwapChain::VulkanSwapChain(VkDevice device, VkPhysicalDevice physDevice, VkSurfaceKHR surface,
                                             const RHISwapChainDesc& desc, const QueueFamilyIndices& queueFamilies,
                                             VkQueue presentQueue)
                : m_desc(desc), m_device(device), m_physDevice(physDevice), m_surface(surface),
                  m_queueFamilies(queueFamilies), m_presentQueue(presentQueue)
            {
                CreateSwapChain();
                CreateImageViews();
                CreateSyncObjects();
            }

            VulkanSwapChain::~VulkanSwapChain()
            {
                Cleanup();
            }

            bool VulkanSwapChain::CreateSwapChain()
            {
                VkSurfaceCapabilitiesKHR capabilities;
                vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physDevice, m_surface, &capabilities);

                VkSwapchainCreateInfoKHR createInfo = {};
                createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
                createInfo.surface = m_surface;
                createInfo.minImageCount = m_desc.bufferCount;
                createInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
                createInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                createInfo.imageExtent = {m_desc.width, m_desc.height};
                createInfo.imageArrayLayers = 1;
                createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

                uint32_t queueFamilyIndices[] = {m_queueFamilies.graphicsFamily.value(),
                                                 m_queueFamilies.presentFamily.value()};

                if (m_queueFamilies.graphicsFamily != m_queueFamilies.presentFamily)
                {
                    createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
                    createInfo.queueFamilyIndexCount = 2;
                    createInfo.pQueueFamilyIndices = queueFamilyIndices;
                }
                else
                {
                    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
                }

                createInfo.preTransform = capabilities.currentTransform;
                createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
                createInfo.presentMode = m_desc.vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
                createInfo.clipped = VK_TRUE;
                createInfo.oldSwapchain = VK_NULL_HANDLE;

                VkResult result = vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapChain);
                if (result != VK_SUCCESS)
                    return false;

                uint32_t imageCount;
                vkGetSwapchainImagesKHR(m_device, m_swapChain, &imageCount, nullptr);
                m_swapChainImages.resize(imageCount);
                vkGetSwapchainImagesKHR(m_device, m_swapChain, &imageCount, m_swapChainImages.data());

                return true;
            }

            bool VulkanSwapChain::CreateImageViews()
            {
                m_swapChainImageViews.resize(m_swapChainImages.size());
                m_backBuffers.resize(m_swapChainImages.size());

                for (size_t i = 0; i < m_swapChainImages.size(); ++i)
                {
                    VkImageViewCreateInfo viewInfo = {};
                    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                    viewInfo.image = m_swapChainImages[i];
                    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
                    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    viewInfo.subresourceRange.baseMipLevel = 0;
                    viewInfo.subresourceRange.levelCount = 1;
                    viewInfo.subresourceRange.baseArrayLayer = 0;
                    viewInfo.subresourceRange.layerCount = 1;

                    VkResult result = vkCreateImageView(m_device, &viewInfo, nullptr, &m_swapChainImageViews[i]);
                    if (result != VK_SUCCESS)
                        return false;

                    RHITextureDesc texDesc;
                    texDesc.width = m_desc.width;
                    texDesc.height = m_desc.height;
                    texDesc.format = PixelFormat::B8G8R8A8_UNORM;
                    texDesc.usage = RHITextureUsage::RenderTarget;
                    texDesc.debugName = "SwapChainImage_" + std::to_string(i);

                    m_backBuffers[i] = std::make_unique<VulkanTexture>(texDesc, m_swapChainImages[i], VK_NULL_HANDLE,
                                                                       m_swapChainImageViews[i], m_device, false);
                }

                return true;
            }

            bool VulkanSwapChain::CreateSyncObjects()
            {
                VkSemaphoreCreateInfo semaphoreInfo = {};
                semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

                VkFenceCreateInfo fenceInfo = {};
                fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

                if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailable) != VK_SUCCESS ||
                    vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinished) != VK_SUCCESS ||
                    vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFence) != VK_SUCCESS)
                {
                    return false;
                }

                return true;
            }

            void VulkanSwapChain::Cleanup()
            {
                m_backBuffers.clear();

                for (auto view : m_swapChainImageViews)
                {
                    if (view != VK_NULL_HANDLE)
                        vkDestroyImageView(m_device, view, nullptr);
                }
                m_swapChainImageViews.clear();

                if (m_imageAvailable != VK_NULL_HANDLE)
                    vkDestroySemaphore(m_device, m_imageAvailable, nullptr);
                if (m_renderFinished != VK_NULL_HANDLE)
                    vkDestroySemaphore(m_device, m_renderFinished, nullptr);
                if (m_inFlightFence != VK_NULL_HANDLE)
                    vkDestroyFence(m_device, m_inFlightFence, nullptr);

                if (m_swapChain != VK_NULL_HANDLE)
                    vkDestroySwapchainKHR(m_device, m_swapChain, nullptr);
            }

            bool VulkanSwapChain::Present(bool vsync)
            {
                if (m_swapChain == VK_NULL_HANDLE)
                    return false;

                VkPresentInfoKHR presentInfo = {};
                presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                presentInfo.waitSemaphoreCount = 1;
                presentInfo.pWaitSemaphores = &m_renderFinished;
                presentInfo.swapchainCount = 1;
                presentInfo.pSwapchains = &m_swapChain;
                presentInfo.pImageIndices = &m_currentImageIndex;

                VkResult result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
                if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
                {
                    // Swap chain needs recreation - caller should handle resize
                    return false;
                }
                return result == VK_SUCCESS;
            }

            bool VulkanSwapChain::AcquireNextImage()
            {
                if (m_swapChain == VK_NULL_HANDLE)
                    return false;

                vkWaitForFences(m_device, 1, &m_inFlightFence, VK_TRUE, UINT64_MAX);
                vkResetFences(m_device, 1, &m_inFlightFence);

                VkResult result = vkAcquireNextImageKHR(m_device, m_swapChain, UINT64_MAX, m_imageAvailable,
                                                        VK_NULL_HANDLE, &m_currentImageIndex);
                if (result == VK_ERROR_OUT_OF_DATE_KHR)
                {
                    return false;
                }
                return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
            }

            bool VulkanSwapChain::Resize(uint32_t width, uint32_t height)
            {
                vkDeviceWaitIdle(m_device);
                Cleanup();
                m_desc.width = width;
                m_desc.height = height;
                return CreateSwapChain() && CreateImageViews() && CreateSyncObjects();
            }

            IRHITexture* VulkanSwapChain::GetBackBuffer()
            {
                if (m_currentImageIndex < m_backBuffers.size())
                    return m_backBuffers[m_currentImageIndex].get();
                return nullptr;
            }

            // ============================================================================
            // VULKAN COMMAND LIST
            // ============================================================================

            VulkanCommandList::VulkanCommandList(VkDevice device, VkCommandPool commandPool, bool isImmediate,
                                                 RHIStatistics* statistics)
                : m_device(device), m_commandPool(commandPool), m_isImmediate(isImmediate), m_statistics(statistics)
            {
                VkCommandBufferAllocateInfo allocInfo = {};
                allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocInfo.commandPool = commandPool;
                allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocInfo.commandBufferCount = 1;

                vkAllocateCommandBuffers(device, &allocInfo, &m_commandBuffer);
            }

            VulkanCommandList::~VulkanCommandList()
            {
                if (m_commandBuffer != VK_NULL_HANDLE)
                {
                    vkFreeCommandBuffers(m_device, m_commandPool, 1, &m_commandBuffer);
                }
            }

            void VulkanCommandList::Begin()
            {
                VkCommandBufferBeginInfo beginInfo = {};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

                vkBeginCommandBuffer(m_commandBuffer, &beginInfo);
                m_isRecording = true;
            }

            void VulkanCommandList::End()
            {
                if (m_isRecording)
                {
                    vkEndCommandBuffer(m_commandBuffer);
                    m_isRecording = false;
                }
            }

            void VulkanCommandList::Reset()
            {
                vkResetCommandBuffer(m_commandBuffer, 0);
                m_isRecording = false;
            }

            void VulkanCommandList::SetRenderTargets(IRHITexture** renderTargets, uint32_t count,
                                                     IRHITexture* depthStencil)
            {
                // Dynamic rendering (Vulkan 1.3) approach
                std::vector<VkRenderingAttachmentInfo> colorAttachments(count);

                for (uint32_t i = 0; i < count; ++i)
                {
                    auto* vkTex = static_cast<VulkanTexture*>(renderTargets[i]);
                    colorAttachments[i] = {};
                    colorAttachments[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    colorAttachments[i].imageView = vkTex->GetVkImageView();
                    colorAttachments[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    colorAttachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    colorAttachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                }

                VkRenderingInfo renderingInfo = {};
                renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                renderingInfo.renderArea = {
                    {0, 0},
                    {renderTargets[0] ? static_cast<VulkanTexture*>(renderTargets[0])->GetWidth() : 0u,
                     renderTargets[0] ? static_cast<VulkanTexture*>(renderTargets[0])->GetHeight() : 0u}};
                renderingInfo.layerCount = 1;
                renderingInfo.colorAttachmentCount = count;
                renderingInfo.pColorAttachments = colorAttachments.data();

                VkRenderingAttachmentInfo depthAttachment = {};
                if (depthStencil)
                {
                    auto* vkDS = static_cast<VulkanTexture*>(depthStencil);
                    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    depthAttachment.imageView = vkDS->GetVkImageView();
                    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    renderingInfo.pDepthAttachment = &depthAttachment;
                }

                vkCmdBeginRendering(m_commandBuffer, &renderingInfo);
                if (m_statistics)
                {
                    m_statistics->renderTargetChanges++;
                }
            }

            void VulkanCommandList::ClearRenderTarget(IRHITexture* target, const float color[4])
            {
                if (!target)
                    return;
                auto* vkTex = static_cast<VulkanTexture*>(target);

                VkClearColorValue clearColor;
                memcpy(clearColor.float32, color, sizeof(float) * 4);

                VkImageSubresourceRange range = {};
                range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                range.baseMipLevel = 0;
                range.levelCount = 1;
                range.baseArrayLayer = 0;
                range.layerCount = 1;

                vkCmdClearColorImage(m_commandBuffer, vkTex->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &clearColor, 1, &range);
            }

            void VulkanCommandList::ClearDepthStencil(IRHITexture* target, float depth, uint8_t stencil)
            {
                if (!target)
                    return;
                auto* vkTex = static_cast<VulkanTexture*>(target);

                VkClearDepthStencilValue clearValue;
                clearValue.depth = depth;
                clearValue.stencil = stencil;

                VkImageSubresourceRange range = {};
                range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                range.baseMipLevel = 0;
                range.levelCount = 1;
                range.baseArrayLayer = 0;
                range.layerCount = 1;

                vkCmdClearDepthStencilImage(m_commandBuffer, vkTex->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            &clearValue, 1, &range);
            }

            void VulkanCommandList::SetViewport(const RHIViewport& viewport)
            {
                VkViewport vp;
                vp.x = viewport.x;
                vp.y = viewport.y;
                vp.width = viewport.width;
                vp.height = viewport.height;
                vp.minDepth = viewport.minDepth;
                vp.maxDepth = viewport.maxDepth;
                vkCmdSetViewport(m_commandBuffer, 0, 1, &vp);
            }

            void VulkanCommandList::SetScissorRect(const RHIScissorRect& rect)
            {
                VkRect2D scissor;
                scissor.offset = {rect.left, rect.top};
                scissor.extent = {static_cast<uint32_t>(rect.right - rect.left),
                                  static_cast<uint32_t>(rect.bottom - rect.top)};
                vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);
            }

            void VulkanCommandList::SetPipelineState(IRHIPipelineState* pipelineState)
            {
                if (!pipelineState)
                    return;
                auto* vkPSO = static_cast<VulkanPipelineState*>(pipelineState);
                vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPSO->GetVkPipeline());
                m_currentPipelineLayout = vkPSO->GetVkLayout();
                if (m_statistics)
                {
                    m_statistics->pipelineChanges++;
                }
            }

            void VulkanCommandList::SetPrimitiveTopology(RHIPrimitiveTopology)
            {
                // Topology is baked into the pipeline state in Vulkan.
                // Dynamic topology requires VK_EXT_extended_dynamic_state which
                // is not yet enabled. This is a no-op for now.
            }

            void VulkanCommandList::SetVertexBuffer(IRHIBuffer* buffer, uint32_t slot, uint32_t offset)
            {
                if (!buffer)
                    return;
                auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
                VkBuffer buffers[] = {vkBuf->GetVkBuffer()};
                VkDeviceSize offsets[] = {offset};
                vkCmdBindVertexBuffers(m_commandBuffer, slot, 1, buffers, offsets);
            }

            void VulkanCommandList::SetIndexBuffer(IRHIBuffer* buffer, uint32_t offset)
            {
                if (!buffer)
                    return;
                auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
                VkIndexType indexType = (vkBuf->GetStride() == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
                vkCmdBindIndexBuffer(m_commandBuffer, vkBuf->GetVkBuffer(), offset, indexType);
            }

            void VulkanCommandList::SetConstantBuffer(RHIShaderStage, uint32_t slot, IRHIBuffer* buffer)
            {
                if (buffer)
                {
                    auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
                    m_pendingBindings.constantBuffers[slot] = vkBuf->GetVkBuffer();
                    m_pendingBindings.constantBufferSizes[slot] = vkBuf->GetSize();
                    m_pendingBindings.dirty = true;
                }
                if (m_statistics)
                {
                    m_statistics->bufferBinds++;
                }
            }

            void VulkanCommandList::SetShaderResource(RHIShaderStage, uint32_t slot, IRHITexture* texture)
            {
                if (texture)
                {
                    auto* vkTex = static_cast<VulkanTexture*>(texture);
                    m_pendingBindings.shaderResources[slot] = vkTex->GetVkImageView();
                    m_pendingBindings.dirty = true;
                }
                if (m_statistics)
                {
                    m_statistics->textureBinds++;
                }
            }

            void VulkanCommandList::SetSampler(RHIShaderStage, uint32_t slot, IRHISampler* sampler)
            {
                if (sampler)
                {
                    auto* vkSamp = static_cast<VulkanSampler*>(sampler);
                    m_pendingBindings.samplers[slot] = vkSamp->GetVkSampler();
                    m_pendingBindings.dirty = true;
                }
            }

            void VulkanCommandList::BindDescriptorSet(VkPipelineLayout layout, VkDescriptorSet descriptorSet)
            {
                if (m_isRecording && descriptorSet != VK_NULL_HANDLE)
                {
                    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                            &descriptorSet, 0, nullptr);
                }
            }

            void VulkanCommandList::Draw(uint32_t vertexCount, uint32_t startVertex)
            {
                vkCmdDraw(m_commandBuffer, vertexCount, 1, startVertex, 0);
                if (m_statistics)
                {
                    m_statistics->drawCalls++;
                    m_statistics->verticesProcessed += vertexCount;
                    m_statistics->trianglesRendered += vertexCount / 3;
                }
            }

            void VulkanCommandList::DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex)
            {
                vkCmdDrawIndexed(m_commandBuffer, indexCount, 1, startIndex, baseVertex, 0);
                if (m_statistics)
                {
                    m_statistics->drawCalls++;
                    m_statistics->verticesProcessed += indexCount;
                    m_statistics->trianglesRendered += indexCount / 3;
                }
            }

            void VulkanCommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex,
                                                  uint32_t startInstance)
            {
                vkCmdDraw(m_commandBuffer, vertexCount, instanceCount, startVertex, startInstance);
                if (m_statistics)
                {
                    m_statistics->drawCalls++;
                    m_statistics->verticesProcessed += vertexCount * instanceCount;
                    m_statistics->trianglesRendered += (vertexCount / 3) * instanceCount;
                }
            }

            void VulkanCommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount,
                                                         uint32_t startIndex, int32_t baseVertex,
                                                         uint32_t startInstance)
            {
                vkCmdDrawIndexed(m_commandBuffer, indexCount, instanceCount, startIndex, baseVertex, startInstance);
                if (m_statistics)
                {
                    m_statistics->drawCalls++;
                    m_statistics->verticesProcessed += indexCount * instanceCount;
                    m_statistics->trianglesRendered += (indexCount / 3) * instanceCount;
                }
            }

            void VulkanCommandList::Dispatch(uint32_t x, uint32_t y, uint32_t z)
            {
                vkCmdDispatch(m_commandBuffer, x, y, z);
                if (m_statistics)
                {
                    m_statistics->dispatchCalls++;
                }
            }

            void VulkanCommandList::BeginEvent(const char*) {}
            void VulkanCommandList::EndEvent() {}
            void VulkanCommandList::SetMarker(const char*) {}

            void VulkanCommandList::TransitionImageLayout(VkImage image, VkImageLayout oldLayout,
                                                          VkImageLayout newLayout, VkImageAspectFlags aspectMask)
            {
                VkImageMemoryBarrier barrier = {};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = oldLayout;
                barrier.newLayout = newLayout;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image;
                barrier.subresourceRange.aspectMask = aspectMask;
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

                VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

                vkCmdPipelineBarrier(m_commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            }

            // ============================================================================
            // VULKAN DEVICE
            // ============================================================================

            VulkanDevice::VulkanDevice()
            {
                m_capabilities.backend = GraphicsBackend::Vulkan;
            }

            VulkanDevice::~VulkanDevice()
            {
                Shutdown();
            }

            bool VulkanDevice::Initialize(const RHIDeviceDesc& desc)
            {
                SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "VulkanDevice::Initialize starting");
                m_validationEnabled = desc.enableDebugLayer;

                if (!CreateInstance(desc))
                    return false;
                if (!SelectPhysicalDevice())
                    return false;
                if (!CreateLogicalDevice())
                    return false;
                if (!CreateCommandPool())
                    return false;

                QueryCapabilities();

                // Create immediate command list with statistics tracking
                m_immediateCommandList =
                    std::make_unique<VulkanCommandList>(m_device, m_commandPool, true, &m_statistics);

                // Create pipeline cache
                VkPipelineCacheCreateInfo cacheInfo = {};
                cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
                vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &m_pipelineCache);

                // Create descriptor pool
                VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
                                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
                                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 100},
                                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 100}};

                VkDescriptorPoolCreateInfo poolInfo = {};
                poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
                poolInfo.poolSizeCount = 4;
                poolInfo.pPoolSizes = poolSizes;
                poolInfo.maxSets = 2000;

                vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool);

                // Create descriptor set layout and default pipeline layout
                if (!CreateDescriptorSetLayout())
                    return false;

                // Create per-frame synchronization primitives
                m_frameFences.resize(MAX_FRAMES_IN_FLIGHT);
                m_renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);

                VkFenceCreateInfo fenceInfo = {};
                fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

                VkSemaphoreCreateInfo semaphoreInfo = {};
                semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

                for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
                {
                    vkCreateFence(m_device, &fenceInfo, nullptr, &m_frameFences[i]);
                    vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]);
                }

                // Load debug utility function pointers if validation is enabled
                if (m_validationEnabled)
                {
                    m_vkCmdBeginDebugUtilsLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                        vkGetInstanceProcAddr(m_instance, "vkCmdBeginDebugUtilsLabelEXT"));
                    m_vkCmdEndDebugUtilsLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                        vkGetInstanceProcAddr(m_instance, "vkCmdEndDebugUtilsLabelEXT"));
                    m_vkCmdInsertDebugUtilsLabel = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
                        vkGetInstanceProcAddr(m_instance, "vkCmdInsertDebugUtilsLabelEXT"));
                }

                return true;
            }

            bool VulkanDevice::CreateInstance(const RHIDeviceDesc& desc)
            {
                VkApplicationInfo appInfo = {};
                appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
                appInfo.pApplicationName = desc.applicationName.c_str();
                appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
                appInfo.pEngineName = "SparkEngine";
                appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
                appInfo.apiVersion = VK_API_VERSION_1_3;

                std::vector<const char*> extensions = {
                    VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef _WIN32
                    VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#elif defined(__linux__)
                    VK_KHR_XCB_SURFACE_EXTENSION_NAME,
#endif
                };

                std::vector<const char*> layers;
                if (m_validationEnabled)
                {
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                }

                VkInstanceCreateInfo createInfo = {};
                createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
                createInfo.pApplicationInfo = &appInfo;
                createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
                createInfo.ppEnabledExtensionNames = extensions.data();
                createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
                createInfo.ppEnabledLayerNames = layers.data();

                VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
                if (result != VK_SUCCESS)
                    return false;

                // Set up debug messenger
                if (m_validationEnabled)
                {
                    VkDebugUtilsMessengerCreateInfoEXT debugInfo = {};
                    debugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
                    debugInfo.messageSeverity =
                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                    debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                    debugInfo.pfnUserCallback = VulkanDebugCallback;

                    auto createDebugFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                        vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
                    if (createDebugFn)
                    {
                        createDebugFn(m_instance, &debugInfo, nullptr, &m_debugMessenger);
                    }
                }

                return true;
            }

            bool VulkanDevice::SelectPhysicalDevice()
            {
                uint32_t deviceCount = 0;
                vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
                if (deviceCount == 0)
                    return false;

                std::vector<VkPhysicalDevice> devices(deviceCount);
                vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

                // Pick the best device (prefer discrete GPU)
                for (const auto& device : devices)
                {
                    VkPhysicalDeviceProperties properties;
                    vkGetPhysicalDeviceProperties(device, &properties);

                    auto queueFamilies = FindQueueFamilies(device);
                    bool extensionsSupported = CheckDeviceExtensionSupport(device);

                    if (queueFamilies.graphicsFamily.has_value() && extensionsSupported)
                    {
                        m_physicalDevice = device;
                        m_queueFamilies = queueFamilies;

                        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                            break;
                    }
                }

                return m_physicalDevice != VK_NULL_HANDLE;
            }

            bool VulkanDevice::CreateLogicalDevice()
            {
                std::set<uint32_t> uniqueQueueFamilies = {m_queueFamilies.graphicsFamily.value()};
                if (m_queueFamilies.presentFamily.has_value())
                    uniqueQueueFamilies.insert(m_queueFamilies.presentFamily.value());

                std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
                float queuePriority = 1.0f;

                for (uint32_t queueFamily : uniqueQueueFamilies)
                {
                    VkDeviceQueueCreateInfo queueInfo = {};
                    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
                    queueInfo.queueFamilyIndex = queueFamily;
                    queueInfo.queueCount = 1;
                    queueInfo.pQueuePriorities = &queuePriority;
                    queueCreateInfos.push_back(queueInfo);
                }

                // Enable features
                VkPhysicalDeviceFeatures deviceFeatures = {};
                deviceFeatures.samplerAnisotropy = VK_TRUE;
                deviceFeatures.fillModeNonSolid = VK_TRUE;
                deviceFeatures.multiViewport = VK_TRUE;
                deviceFeatures.geometryShader = VK_TRUE;
                deviceFeatures.tessellationShader = VK_TRUE;
                deviceFeatures.multiDrawIndirect = VK_TRUE;

                // Vulkan 1.3 features
                VkPhysicalDeviceVulkan13Features vulkan13Features = {};
                vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
                vulkan13Features.dynamicRendering = VK_TRUE;
                vulkan13Features.synchronization2 = VK_TRUE;

                VkPhysicalDeviceVulkan12Features vulkan12Features = {};
                vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
                vulkan12Features.pNext = &vulkan13Features;
                vulkan12Features.descriptorIndexing = VK_TRUE;
                vulkan12Features.timelineSemaphore = VK_TRUE;
                vulkan12Features.bufferDeviceAddress = VK_TRUE;

                VkDeviceCreateInfo createInfo = {};
                createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
                createInfo.pNext = &vulkan12Features;
                createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
                createInfo.pQueueCreateInfos = queueCreateInfos.data();
                createInfo.pEnabledFeatures = &deviceFeatures;
                createInfo.enabledExtensionCount = static_cast<uint32_t>(m_deviceExtensions.size());
                createInfo.ppEnabledExtensionNames = m_deviceExtensions.data();

                VkResult result = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
                if (result != VK_SUCCESS)
                    return false;

                vkGetDeviceQueue(m_device, m_queueFamilies.graphicsFamily.value(), 0, &m_graphicsQueue);
                if (m_queueFamilies.presentFamily.has_value())
                    vkGetDeviceQueue(m_device, m_queueFamilies.presentFamily.value(), 0, &m_presentQueue);

                return true;
            }

            bool VulkanDevice::CreateCommandPool()
            {
                VkCommandPoolCreateInfo poolInfo = {};
                poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                poolInfo.queueFamilyIndex = m_queueFamilies.graphicsFamily.value();

                return vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) == VK_SUCCESS;
            }

            QueueFamilyIndices VulkanDevice::FindQueueFamilies(VkPhysicalDevice device) const
            {
                QueueFamilyIndices indices;

                uint32_t queueFamilyCount = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

                std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
                vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

                for (uint32_t i = 0; i < queueFamilyCount; ++i)
                {
                    if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                    {
                        indices.graphicsFamily = i;
                        indices.presentFamily = i; // Assume same for now
                    }
                    if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
                        indices.computeFamily = i;
                    if (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
                        indices.transferFamily = i;
                }

                return indices;
            }

            bool VulkanDevice::CheckDeviceExtensionSupport(VkPhysicalDevice device) const
            {
                uint32_t extensionCount;
                vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

                std::vector<VkExtensionProperties> available(extensionCount);
                vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, available.data());

                std::set<std::string> required(m_deviceExtensions.begin(), m_deviceExtensions.end());
                for (const auto& ext : available)
                    required.erase(ext.extensionName);

                return required.empty();
            }

            void VulkanDevice::QueryCapabilities()
            {
                VkPhysicalDeviceProperties properties;
                vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);

                m_capabilities.deviceName = properties.deviceName;
                m_capabilities.apiVersion = "Vulkan " + std::to_string(VK_VERSION_MAJOR(properties.apiVersion)) + "." +
                                            std::to_string(VK_VERSION_MINOR(properties.apiVersion)) + "." +
                                            std::to_string(VK_VERSION_PATCH(properties.apiVersion));

                m_capabilities.maxTextureSize = properties.limits.maxImageDimension2D;
                m_capabilities.maxRenderTargets = properties.limits.maxColorAttachments;
                m_capabilities.maxSamplers = properties.limits.maxPerStageDescriptorSamplers;
                m_capabilities.maxAnisotropy = properties.limits.maxSamplerAnisotropy;

                VkPhysicalDeviceMemoryProperties memProperties;
                vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

                m_capabilities.dedicatedVideoMemory = 0;
                for (uint32_t i = 0; i < memProperties.memoryHeapCount; ++i)
                {
                    if (memProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                        m_capabilities.dedicatedVideoMemory += memProperties.memoryHeaps[i].size;
                }

                VkPhysicalDeviceFeatures features;
                vkGetPhysicalDeviceFeatures(m_physicalDevice, &features);

                m_capabilities.tessellationSupport = features.tessellationShader;
                m_capabilities.geometryShaderSupport = features.geometryShader;
                m_capabilities.computeShaderSupport = true;
                m_capabilities.multiDrawIndirectSupport = features.multiDrawIndirect;

                // Probe Vulkan RT extensions per Vulkan-Hpp / KHR spec patterns.
                // Full hardware RT requires these four extensions (Vulkan 1.1+):
                //   VK_KHR_acceleration_structure
                //   VK_KHR_ray_tracing_pipeline
                //   VK_KHR_deferred_host_operations (required by acceleration_structure)
                //   VK_KHR_buffer_device_address     (required by acceleration_structure)
                // Inline RT (ray query in compute/fragment) additionally needs:
                //   VK_KHR_ray_query
                // VRS (variable rate shading) for adaptive RT resolution:
                //   VK_KHR_fragment_shading_rate
                {
                    uint32_t extCount = 0;
                    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, nullptr);
                    std::vector<VkExtensionProperties> exts(extCount);
                    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, exts.data());

                    bool hasAccelStruct = false;
                    bool hasRTPipeline = false;
                    bool hasDeferredOps = false;
                    bool hasBufferAddr = false;
                    bool hasRayQuery = false;
                    bool hasVRS = false;

                    for (const auto& ext : exts)
                    {
                        if (std::strcmp(ext.extensionName, "VK_KHR_acceleration_structure") == 0)
                            hasAccelStruct = true;
                        else if (std::strcmp(ext.extensionName, "VK_KHR_ray_tracing_pipeline") == 0)
                            hasRTPipeline = true;
                        else if (std::strcmp(ext.extensionName, "VK_KHR_deferred_host_operations") == 0)
                            hasDeferredOps = true;
                        else if (std::strcmp(ext.extensionName, "VK_KHR_buffer_device_address") == 0)
                            hasBufferAddr = true;
                        else if (std::strcmp(ext.extensionName, "VK_KHR_ray_query") == 0)
                            hasRayQuery = true;
                        else if (std::strcmp(ext.extensionName, "VK_KHR_fragment_shading_rate") == 0)
                            hasVRS = true;
                    }

                    bool fullHWRT = hasAccelStruct && hasRTPipeline && hasDeferredOps && hasBufferAddr;

                    if (fullHWRT)
                    {
                        // Query ray tracing pipeline properties for max recursion depth
                        // via VkPhysicalDeviceRayTracingPipelinePropertiesKHR (Vulkan 1.1+ pNext chain)
                        m_capabilities.rayTracing.bestBackend = RayTracingBackend::HardwareVKRT;
                        m_capabilities.rayTracing.supportsHardwareRT = true;
                        m_capabilities.rayTracing.supportsInlineRT = hasRayQuery;
                        m_capabilities.rayTracing.maxRecursionDepth = 31; // Spec minimum is 1, most GPUs support 31
                        m_capabilities.rayTracing.supportsVRS = hasVRS;
                        m_capabilities.rayTracing.raytracingTier = hasRayQuery ? 2 : 1;
                    }
                    else
                    {
                        // Software SDFGI fallback — compute shaders always available in Vulkan
                        m_capabilities.rayTracing.bestBackend = RayTracingBackend::Software_SDFGI;
                        m_capabilities.rayTracing.supportsHardwareRT = false;
                        m_capabilities.rayTracing.supportsInlineRT = false;
                        m_capabilities.rayTracing.maxRecursionDepth = 0;
                        m_capabilities.rayTracing.supportsVRS = false;
                        m_capabilities.rayTracing.raytracingTier = 0;
                    }
                }
            }

            bool VulkanDevice::CreateDescriptorSetLayout()
            {
                // Create a binding layout matching D3D11's resource model:
                // binding 0-13: uniform buffers (14 constant buffer slots)
                // binding 14-29: combined image samplers (16 texture slots)
                std::vector<VkDescriptorSetLayoutBinding> bindings;

                // Constant buffer bindings (0-13)
                for (uint32_t i = 0; i < 14; ++i)
                {
                    VkDescriptorSetLayoutBinding uboBinding = {};
                    uboBinding.binding = i;
                    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                    uboBinding.descriptorCount = 1;
                    uboBinding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
                    bindings.push_back(uboBinding);
                }

                // Combined image sampler bindings (14-29)
                for (uint32_t i = 0; i < 16; ++i)
                {
                    VkDescriptorSetLayoutBinding texBinding = {};
                    texBinding.binding = 14 + i;
                    texBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    texBinding.descriptorCount = 1;
                    texBinding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
                    bindings.push_back(texBinding);
                }

                VkDescriptorSetLayoutCreateInfo layoutInfo = {};
                layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
                layoutInfo.pBindings = bindings.data();

                if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_bindingLayout) != VK_SUCCESS)
                    return false;

                // Create default pipeline layout using this descriptor set layout
                VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
                pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                pipelineLayoutInfo.setLayoutCount = 1;
                pipelineLayoutInfo.pSetLayouts = &m_bindingLayout;

                if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_defaultPipelineLayout) !=
                    VK_SUCCESS)
                    return false;

                return true;
            }

            VkDescriptorSet VulkanDevice::AllocateDescriptorSet()
            {
                VkDescriptorSetAllocateInfo allocInfo = {};
                allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                allocInfo.descriptorPool = m_descriptorPool;
                allocInfo.descriptorSetCount = 1;
                allocInfo.pSetLayouts = &m_bindingLayout;

                VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
                if (vkAllocateDescriptorSets(m_device, &allocInfo, &descriptorSet) != VK_SUCCESS)
                    return VK_NULL_HANDLE;

                return descriptorSet;
            }

            void VulkanDevice::Shutdown()
            {
                SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "VulkanDevice::Shutdown");
                if (m_device != VK_NULL_HANDLE)
                    vkDeviceWaitIdle(m_device);

                m_immediateCommandList.reset();

                // Destroy per-frame synchronization
                for (auto fence : m_frameFences)
                {
                    if (fence != VK_NULL_HANDLE)
                        vkDestroyFence(m_device, fence, nullptr);
                }
                m_frameFences.clear();

                for (auto sem : m_renderFinishedSemaphores)
                {
                    if (sem != VK_NULL_HANDLE)
                        vkDestroySemaphore(m_device, sem, nullptr);
                }
                m_renderFinishedSemaphores.clear();

                // Destroy descriptor set layout and default pipeline layout
                if (m_defaultPipelineLayout != VK_NULL_HANDLE)
                    vkDestroyPipelineLayout(m_device, m_defaultPipelineLayout, nullptr);
                if (m_bindingLayout != VK_NULL_HANDLE)
                    vkDestroyDescriptorSetLayout(m_device, m_bindingLayout, nullptr);

                if (m_pipelineCache != VK_NULL_HANDLE)
                    vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
                if (m_descriptorPool != VK_NULL_HANDLE)
                    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
                if (m_commandPool != VK_NULL_HANDLE)
                    vkDestroyCommandPool(m_device, m_commandPool, nullptr);
                if (m_device != VK_NULL_HANDLE)
                    vkDestroyDevice(m_device, nullptr);
                if (m_debugMessenger != VK_NULL_HANDLE)
                {
                    auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                        vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
                    if (destroyFn)
                        destroyFn(m_instance, m_debugMessenger, nullptr);
                }
                if (m_instance != VK_NULL_HANDLE)
                    vkDestroyInstance(m_instance, nullptr);

                m_defaultPipelineLayout = VK_NULL_HANDLE;
                m_bindingLayout = VK_NULL_HANDLE;
                m_pipelineCache = VK_NULL_HANDLE;
                m_descriptorPool = VK_NULL_HANDLE;
                m_commandPool = VK_NULL_HANDLE;
                m_device = VK_NULL_HANDLE;
                m_debugMessenger = VK_NULL_HANDLE;
                m_instance = VK_NULL_HANDLE;
            }

            std::unique_ptr<IRHISwapChain> VulkanDevice::CreateSwapChain(const RHISwapChainDesc& desc)
            {
                VkSurfaceKHR surface = VK_NULL_HANDLE;

#ifdef _WIN32
                VkWin32SurfaceCreateInfoKHR surfaceInfo = {};
                surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
                surfaceInfo.hwnd = static_cast<HWND>(desc.windowHandle);
                surfaceInfo.hinstance = GetModuleHandle(nullptr);
                vkCreateWin32SurfaceKHR(m_instance, &surfaceInfo, nullptr, &surface);
#endif

                return std::make_unique<VulkanSwapChain>(m_device, m_physicalDevice, surface, desc, m_queueFamilies,
                                                         m_presentQueue);
            }

            IRHIBuffer* VulkanDevice::CreateBuffer(const RHIBufferDesc& desc)
            {
                VkBufferCreateInfo bufferInfo = {};
                bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bufferInfo.size = desc.size;
                bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

                if (desc.usage & RHIBufferUsage::Vertex)
                    bufferInfo.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
                if (desc.usage & RHIBufferUsage::Index)
                    bufferInfo.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
                if (desc.usage & RHIBufferUsage::Constant)
                    bufferInfo.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                if (desc.usage & RHIBufferUsage::Storage)
                    bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                if (desc.usage & RHIBufferUsage::CopySrc)
                    bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                if (desc.usage & RHIBufferUsage::CopyDst)
                    bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

                // Ensure device-local buffers with initial data can be transfer targets
                if (desc.initialData && desc.access == RHIBufferAccess::Static)
                    bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

                VkBuffer buffer;
                if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
                    return nullptr;

                VkMemoryRequirements memRequirements;
                vkGetBufferMemoryRequirements(m_device, buffer, &memRequirements);

                VkMemoryPropertyFlags memProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                if (desc.access == RHIBufferAccess::Dynamic || desc.access == RHIBufferAccess::Staging)
                {
                    memProperties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                }

                VkMemoryAllocateInfo allocInfo = {};
                allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                allocInfo.allocationSize = memRequirements.size;
                allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, memProperties);

                VkDeviceMemory memory;
                if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS)
                {
                    vkDestroyBuffer(m_device, buffer, nullptr);
                    return nullptr;
                }

                vkBindBufferMemory(m_device, buffer, memory, 0);

                // Upload initial data
                if (desc.initialData)
                {
                    if (memProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                    {
                        // Direct map for host-visible memory
                        void* mapped;
                        vkMapMemory(m_device, memory, 0, desc.size, 0, &mapped);
                        memcpy(mapped, desc.initialData, desc.size);
                        vkUnmapMemory(m_device, memory);
                    }
                    else
                    {
                        // Use staging buffer for device-local memory
                        VkBuffer stagingBuffer;
                        VkDeviceMemory stagingMemory;

                        VkBufferCreateInfo stagingInfo = {};
                        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                        stagingInfo.size = desc.size;
                        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

                        if (vkCreateBuffer(m_device, &stagingInfo, nullptr, &stagingBuffer) == VK_SUCCESS)
                        {
                            VkMemoryRequirements stagingReqs;
                            vkGetBufferMemoryRequirements(m_device, stagingBuffer, &stagingReqs);

                            VkMemoryAllocateInfo stagingAlloc = {};
                            stagingAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                            stagingAlloc.allocationSize = stagingReqs.size;
                            stagingAlloc.memoryTypeIndex =
                                FindMemoryType(stagingReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

                            if (vkAllocateMemory(m_device, &stagingAlloc, nullptr, &stagingMemory) == VK_SUCCESS)
                            {
                                vkBindBufferMemory(m_device, stagingBuffer, stagingMemory, 0);

                                void* mapped;
                                vkMapMemory(m_device, stagingMemory, 0, desc.size, 0, &mapped);
                                memcpy(mapped, desc.initialData, desc.size);
                                vkUnmapMemory(m_device, stagingMemory);

                                // Record and execute copy command
                                VkCommandBufferAllocateInfo cmdAllocInfo = {};
                                cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                                cmdAllocInfo.commandPool = m_commandPool;
                                cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                                cmdAllocInfo.commandBufferCount = 1;

                                VkCommandBuffer cmdBuffer;
                                vkAllocateCommandBuffers(m_device, &cmdAllocInfo, &cmdBuffer);

                                VkCommandBufferBeginInfo beginInfo = {};
                                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                                vkBeginCommandBuffer(cmdBuffer, &beginInfo);

                                VkBufferCopy copyRegion = {};
                                copyRegion.size = desc.size;
                                vkCmdCopyBuffer(cmdBuffer, stagingBuffer, buffer, 1, &copyRegion);

                                vkEndCommandBuffer(cmdBuffer);

                                VkSubmitInfo submitInfo = {};
                                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                                submitInfo.commandBufferCount = 1;
                                submitInfo.pCommandBuffers = &cmdBuffer;

                                vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
                                vkQueueWaitIdle(m_graphicsQueue);

                                vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmdBuffer);
                                vkFreeMemory(m_device, stagingMemory, nullptr);
                            }
                            vkDestroyBuffer(m_device, stagingBuffer, nullptr);
                        }
                    }
                }

                // Also set transfer dst flag for device-local buffers that may receive initial data
                return new VulkanBuffer(desc, buffer, memory, m_device);
            }

            IRHITexture* VulkanDevice::CreateTexture(const RHITextureDesc& desc)
            {
                VkImageCreateInfo imageInfo = {};
                imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                imageInfo.imageType = VK_IMAGE_TYPE_2D;
                imageInfo.format = ConvertFormat(desc.format);
                imageInfo.extent = {desc.width, desc.height, desc.depth};
                imageInfo.mipLevels = desc.mipLevels;
                imageInfo.arrayLayers = desc.arraySize;
                imageInfo.samples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);
                imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
                imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

                if (desc.usage & RHITextureUsage::ShaderResource)
                    imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
                if (desc.usage & RHITextureUsage::RenderTarget)
                    imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                if (desc.usage & RHITextureUsage::DepthStencil)
                    imageInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                if (desc.usage & RHITextureUsage::UnorderedAccess)
                    imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
                imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

                VkImage image;
                if (vkCreateImage(m_device, &imageInfo, nullptr, &image) != VK_SUCCESS)
                    return nullptr;

                VkMemoryRequirements memReqs;
                vkGetImageMemoryRequirements(m_device, image, &memReqs);

                VkMemoryAllocateInfo allocInfo = {};
                allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                allocInfo.allocationSize = memReqs.size;
                allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

                VkDeviceMemory memory;
                if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS)
                {
                    vkDestroyImage(m_device, image, nullptr);
                    return nullptr;
                }

                vkBindImageMemory(m_device, image, memory, 0);

                // Create image view
                VkImageViewCreateInfo viewInfo = {};
                viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                viewInfo.image = image;
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = ConvertFormat(desc.format);
                viewInfo.subresourceRange.baseMipLevel = 0;
                viewInfo.subresourceRange.levelCount = desc.mipLevels;
                viewInfo.subresourceRange.baseArrayLayer = 0;
                viewInfo.subresourceRange.layerCount = desc.arraySize;

                if (desc.usage & RHITextureUsage::DepthStencil)
                    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                else
                    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

                VkImageView imageView;
                if (vkCreateImageView(m_device, &viewInfo, nullptr, &imageView) != VK_SUCCESS)
                {
                    vkDestroyImage(m_device, image, nullptr);
                    vkFreeMemory(m_device, memory, nullptr);
                    return nullptr;
                }

                return new VulkanTexture(desc, image, memory, imageView, m_device);
            }

            IRHIShader* VulkanDevice::CreateShader(const RHIShaderDesc& desc)
            {
                std::vector<uint8_t> spirvCode;

                if (desc.language == ShaderLanguage::SPIRV && desc.bytecode && desc.bytecodeSize > 0)
                {
                    spirvCode.resize(desc.bytecodeSize);
                    memcpy(spirvCode.data(), desc.bytecode, desc.bytecodeSize);
                }
                else
                {
                    // GLSL to SPIR-V compilation would happen here via glslang/shaderc
                    // For now, expect pre-compiled SPIR-V
                    return nullptr;
                }

                VkShaderModuleCreateInfo createInfo = {};
                createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                createInfo.codeSize = spirvCode.size();
                createInfo.pCode = reinterpret_cast<const uint32_t*>(spirvCode.data());

                VkShaderModule module;
                if (vkCreateShaderModule(m_device, &createInfo, nullptr, &module) != VK_SUCCESS)
                    return nullptr;

                return new VulkanShader(desc, module, m_device, std::move(spirvCode));
            }

            IRHISampler* VulkanDevice::CreateSampler(const RHISamplerDesc& desc)
            {
                VkSamplerCreateInfo samplerInfo = {};
                samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                samplerInfo.magFilter = ConvertFilter(desc.magFilter);
                samplerInfo.minFilter = ConvertFilter(desc.minFilter);
                samplerInfo.mipmapMode = (desc.mipFilter == RHIFilterMode::Linear) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                                   : VK_SAMPLER_MIPMAP_MODE_NEAREST;
                samplerInfo.addressModeU = ConvertAddressMode(desc.addressU);
                samplerInfo.addressModeV = ConvertAddressMode(desc.addressV);
                samplerInfo.addressModeW = ConvertAddressMode(desc.addressW);
                samplerInfo.mipLodBias = desc.mipLodBias;
                samplerInfo.anisotropyEnable = (desc.maxAnisotropy > 1) ? VK_TRUE : VK_FALSE;
                samplerInfo.maxAnisotropy = static_cast<float>(desc.maxAnisotropy);
                samplerInfo.compareEnable = (desc.compareOp != RHICompareOp::Never) ? VK_TRUE : VK_FALSE;
                samplerInfo.compareOp = ConvertCompareOp(desc.compareOp);
                samplerInfo.minLod = desc.minLod;
                samplerInfo.maxLod = desc.maxLod;
                samplerInfo.borderColor = ConvertBorderColor(desc.borderColor);

                VkSampler sampler;
                if (vkCreateSampler(m_device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
                    return nullptr;

                return new VulkanSampler(desc, sampler, m_device);
            }

            IRHIPipelineState* VulkanDevice::CreatePipelineState(const RHIPipelineStateDesc& desc,
                                                                 IRHIShader* vertexShader, IRHIShader* pixelShader)
            {
                auto* vkVS = static_cast<VulkanShader*>(vertexShader);
                auto* vkPS = static_cast<VulkanShader*>(pixelShader);

                // Shader stages
                VkPipelineShaderStageCreateInfo shaderStages[2] = {};
                shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
                shaderStages[0].module = vkVS->GetVkModule();
                shaderStages[0].pName = vkVS->GetEntryPoint().c_str();

                shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                shaderStages[1].module = vkPS->GetVkModule();
                shaderStages[1].pName = vkPS->GetEntryPoint().c_str();

                // Vertex input
                std::vector<VkVertexInputBindingDescription> bindings;
                std::vector<VkVertexInputAttributeDescription> attributes;

                uint32_t currentOffset = 0;
                for (size_t i = 0; i < desc.inputLayout.elements.size(); ++i)
                {
                    const auto& elem = desc.inputLayout.elements[i];
                    VkVertexInputAttributeDescription attr = {};
                    attr.location = static_cast<uint32_t>(i);
                    attr.binding = elem.inputSlot;
                    attr.format = ConvertVertexFormat(elem.format);
                    attr.offset = elem.byteOffset;
                    attributes.push_back(attr);
                }

                // Compute per-binding strides from actual element offsets and sizes
                std::unordered_map<uint32_t, uint32_t> bindingStrides;
                for (const auto& elem : desc.inputLayout.elements)
                {
                    uint32_t elemSize = GetVertexFormatSize(elem.format);
                    uint32_t elemEnd = elem.byteOffset + elemSize;
                    auto it = bindingStrides.find(elem.inputSlot);
                    if (it == bindingStrides.end() || elemEnd > it->second)
                    {
                        bindingStrides[elem.inputSlot] = elemEnd;
                    }
                }

                for (const auto& [slot, stride] : bindingStrides)
                {
                    VkVertexInputBindingDescription binding = {};
                    binding.binding = slot;
                    binding.stride = stride;
                    // Check if any element in this slot is per-instance
                    bool perInstance = false;
                    for (const auto& elem : desc.inputLayout.elements)
                    {
                        if (elem.inputSlot == slot && elem.perInstance)
                        {
                            perInstance = true;
                            break;
                        }
                    }
                    binding.inputRate = perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
                    bindings.push_back(binding);
                }

                VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
                vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
                vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
                vertexInputInfo.pVertexBindingDescriptions = bindings.data();
                vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
                vertexInputInfo.pVertexAttributeDescriptions = attributes.data();

                // Input assembly
                VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
                inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
                inputAssembly.topology = ConvertTopology(desc.topology);
                inputAssembly.primitiveRestartEnable = VK_FALSE;

                // Dynamic state
                VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

                VkPipelineDynamicStateCreateInfo dynamicState = {};
                dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
                dynamicState.dynamicStateCount = 2;
                dynamicState.pDynamicStates = dynamicStates;

                VkPipelineViewportStateCreateInfo viewportState = {};
                viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
                viewportState.viewportCount = 1;
                viewportState.scissorCount = 1;

                // Rasterizer
                VkPipelineRasterizationStateCreateInfo rasterizer = {};
                rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
                rasterizer.depthClampEnable = VK_FALSE;
                rasterizer.rasterizerDiscardEnable = VK_FALSE;
                rasterizer.polygonMode =
                    (desc.rasterizer.fillMode == RHIFillMode::Wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
                rasterizer.cullMode = (desc.rasterizer.cullMode == RHICullMode::None)    ? VK_CULL_MODE_NONE
                                      : (desc.rasterizer.cullMode == RHICullMode::Front) ? VK_CULL_MODE_FRONT_BIT
                                                                                         : VK_CULL_MODE_BACK_BIT;
                rasterizer.frontFace =
                    desc.rasterizer.frontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
                rasterizer.depthBiasEnable = (desc.rasterizer.depthBias != 0) ? VK_TRUE : VK_FALSE;
                rasterizer.lineWidth = 1.0f;

                // Multisampling
                VkPipelineMultisampleStateCreateInfo multisampling = {};
                multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
                multisampling.sampleShadingEnable = VK_FALSE;
                multisampling.rasterizationSamples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);

                // Depth stencil
                VkPipelineDepthStencilStateCreateInfo depthStencil = {};
                depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
                depthStencil.depthTestEnable = desc.depthStencil.depthEnable ? VK_TRUE : VK_FALSE;
                depthStencil.depthWriteEnable = desc.depthStencil.depthWrite ? VK_TRUE : VK_FALSE;
                depthStencil.depthCompareOp = ConvertCompareOp(desc.depthStencil.depthFunc);
                depthStencil.stencilTestEnable = desc.depthStencil.stencilEnable ? VK_TRUE : VK_FALSE;

                // Color blending
                std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments(desc.numRenderTargets);
                for (uint32_t i = 0; i < desc.numRenderTargets; ++i)
                {
                    colorBlendAttachments[i].blendEnable = desc.blend.renderTargets[i].blendEnable ? VK_TRUE : VK_FALSE;
                    colorBlendAttachments[i].srcColorBlendFactor =
                        ConvertBlendFactor(desc.blend.renderTargets[i].srcBlend);
                    colorBlendAttachments[i].dstColorBlendFactor =
                        ConvertBlendFactor(desc.blend.renderTargets[i].dstBlend);
                    colorBlendAttachments[i].colorBlendOp = ConvertBlendOp(desc.blend.renderTargets[i].blendOp);
                    colorBlendAttachments[i].srcAlphaBlendFactor =
                        ConvertBlendFactor(desc.blend.renderTargets[i].srcBlendAlpha);
                    colorBlendAttachments[i].dstAlphaBlendFactor =
                        ConvertBlendFactor(desc.blend.renderTargets[i].dstBlendAlpha);
                    colorBlendAttachments[i].alphaBlendOp = ConvertBlendOp(desc.blend.renderTargets[i].blendOpAlpha);
                    colorBlendAttachments[i].colorWriteMask = desc.blend.renderTargets[i].writeMask;
                }

                VkPipelineColorBlendStateCreateInfo colorBlending = {};
                colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                colorBlending.attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size());
                colorBlending.pAttachments = colorBlendAttachments.data();

                // Use the default pipeline layout with descriptor set bindings
                VkPipelineLayout pipelineLayout = m_defaultPipelineLayout;
                // We share the default layout - pipelines using it must not destroy it.
                // Create a per-pipeline copy for safety.
                VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
                pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                pipelineLayoutInfo.setLayoutCount = 1;
                pipelineLayoutInfo.pSetLayouts = &m_bindingLayout;

                if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
                    return nullptr;

                // Dynamic rendering format info (Vulkan 1.3)
                std::vector<VkFormat> colorFormats;
                for (uint32_t i = 0; i < desc.numRenderTargets; ++i)
                    colorFormats.push_back(ConvertFormat(desc.renderTargetFormats[i]));

                VkPipelineRenderingCreateInfo renderingInfo = {};
                renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
                renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
                renderingInfo.pColorAttachmentFormats = colorFormats.data();
                renderingInfo.depthAttachmentFormat = ConvertFormat(desc.depthStencilFormat);

                // Create pipeline
                VkGraphicsPipelineCreateInfo pipelineInfo = {};
                pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                pipelineInfo.pNext = &renderingInfo;
                pipelineInfo.stageCount = 2;
                pipelineInfo.pStages = shaderStages;
                pipelineInfo.pVertexInputState = &vertexInputInfo;
                pipelineInfo.pInputAssemblyState = &inputAssembly;
                pipelineInfo.pViewportState = &viewportState;
                pipelineInfo.pRasterizationState = &rasterizer;
                pipelineInfo.pMultisampleState = &multisampling;
                pipelineInfo.pDepthStencilState = &depthStencil;
                pipelineInfo.pColorBlendState = &colorBlending;
                pipelineInfo.pDynamicState = &dynamicState;
                pipelineInfo.layout = pipelineLayout;
                pipelineInfo.renderPass = VK_NULL_HANDLE; // Using dynamic rendering

                VkPipeline pipeline;
                if (vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineInfo, nullptr, &pipeline) !=
                    VK_SUCCESS)
                {
                    vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
                    return nullptr;
                }

                return new VulkanPipelineState(desc, pipeline, pipelineLayout, m_device);
            }

            void VulkanDevice::DestroyBuffer(IRHIBuffer* buffer)
            {
                delete buffer;
            }
            void VulkanDevice::DestroyTexture(IRHITexture* texture)
            {
                delete texture;
            }
            void VulkanDevice::DestroyShader(IRHIShader* shader)
            {
                delete shader;
            }
            void VulkanDevice::DestroySampler(IRHISampler* sampler)
            {
                delete sampler;
            }
            void VulkanDevice::DestroyPipelineState(IRHIPipelineState* state)
            {
                delete state;
            }

            void* VulkanDevice::MapBuffer(IRHIBuffer* buffer)
            {
                auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
                void* mapped;
                vkMapMemory(m_device, vkBuf->GetVkMemory(), 0, vkBuf->GetSize(), 0, &mapped);
                vkBuf->SetMappedPtr(mapped);
                return mapped;
            }

            void VulkanDevice::UnmapBuffer(IRHIBuffer* buffer)
            {
                auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
                vkUnmapMemory(m_device, vkBuf->GetVkMemory());
                vkBuf->SetMappedPtr(nullptr);
            }

            void VulkanDevice::UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset)
            {
                void* mapped = MapBuffer(buffer);
                if (mapped)
                {
                    memcpy(static_cast<uint8_t*>(mapped) + offset, data, size);
                    UnmapBuffer(buffer);
                }
            }

            void VulkanDevice::UpdateTexture(IRHITexture* texture, const void* data, uint32_t mipLevel, uint32_t)
            {
                if (!texture || !data)
                    return;

                auto* vkTex = static_cast<VulkanTexture*>(texture);
                uint32_t width = std::max(1u, vkTex->GetWidth() >> mipLevel);
                uint32_t height = std::max(1u, vkTex->GetHeight() >> mipLevel);

                // Calculate size based on format (simplified - assumes 4 bytes per pixel for common formats)
                uint32_t bytesPerPixel = 4;
                VkFormat fmt = ConvertFormat(vkTex->GetFormat());
                if (fmt == VK_FORMAT_R8_UNORM)
                    bytesPerPixel = 1;
                else if (fmt == VK_FORMAT_R8G8_UNORM)
                    bytesPerPixel = 2;
                else if (fmt == VK_FORMAT_R16G16B16A16_SFLOAT)
                    bytesPerPixel = 8;
                else if (fmt == VK_FORMAT_R32G32B32A32_SFLOAT)
                    bytesPerPixel = 16;
                else if (fmt == VK_FORMAT_R32_SFLOAT || fmt == VK_FORMAT_R32_UINT || fmt == VK_FORMAT_R32_SINT)
                    bytesPerPixel = 4;
                else if (fmt == VK_FORMAT_R16_SFLOAT)
                    bytesPerPixel = 2;

                VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * bytesPerPixel;

                // Create staging buffer
                VkBuffer stagingBuffer;
                VkDeviceMemory stagingMemory;

                VkBufferCreateInfo bufInfo = {};
                bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bufInfo.size = imageSize;
                bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

                if (vkCreateBuffer(m_device, &bufInfo, nullptr, &stagingBuffer) != VK_SUCCESS)
                    return;

                VkMemoryRequirements memReqs;
                vkGetBufferMemoryRequirements(m_device, stagingBuffer, &memReqs);

                VkMemoryAllocateInfo allocInfo = {};
                allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                allocInfo.allocationSize = memReqs.size;
                allocInfo.memoryTypeIndex = FindMemoryType(
                    memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

                if (vkAllocateMemory(m_device, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS)
                {
                    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
                    return;
                }

                vkBindBufferMemory(m_device, stagingBuffer, stagingMemory, 0);

                // Copy data to staging buffer
                void* mapped;
                vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &mapped);
                memcpy(mapped, data, static_cast<size_t>(imageSize));
                vkUnmapMemory(m_device, stagingMemory);

                // Record copy command
                VkCommandBufferAllocateInfo cmdAllocInfo = {};
                cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                cmdAllocInfo.commandPool = m_commandPool;
                cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cmdAllocInfo.commandBufferCount = 1;

                VkCommandBuffer cmdBuffer;
                vkAllocateCommandBuffers(m_device, &cmdAllocInfo, &cmdBuffer);

                VkCommandBufferBeginInfo beginInfo = {};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cmdBuffer, &beginInfo);

                // Transition to transfer dst
                VkImageMemoryBarrier barrier = {};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = vkTex->GetCurrentLayout();
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = vkTex->GetVkImage();
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.baseMipLevel = mipLevel;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = 1;
                barrier.srcAccessMask = 0;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

                vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1, &barrier);

                // Copy buffer to image
                VkBufferImageCopy region = {};
                region.bufferOffset = 0;
                region.bufferRowLength = 0;
                region.bufferImageHeight = 0;
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel = mipLevel;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = 1;
                region.imageOffset = {0, 0, 0};
                region.imageExtent = {width, height, 1};

                vkCmdCopyBufferToImage(cmdBuffer, stagingBuffer, vkTex->GetVkImage(),
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                // Transition to shader read optimal
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

                vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &barrier);

                vkEndCommandBuffer(cmdBuffer);

                // Submit and wait
                VkSubmitInfo submitInfo = {};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &cmdBuffer;

                vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
                vkQueueWaitIdle(m_graphicsQueue);

                vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmdBuffer);
                vkDestroyBuffer(m_device, stagingBuffer, nullptr);
                vkFreeMemory(m_device, stagingMemory, nullptr);

                vkTex->SetCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }

            IRHICommandList* VulkanDevice::GetImmediateCommandList()
            {
                return m_immediateCommandList.get();
            }

            IRHICommandList* VulkanDevice::CreateDeferredCommandList()
            {
                return new VulkanCommandList(m_device, m_commandPool, false);
            }

            void VulkanDevice::ExecuteCommandList(IRHICommandList* commandList)
            {
                auto* vkCmd = static_cast<VulkanCommandList*>(commandList);

                VkSubmitInfo submitInfo = {};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                VkCommandBuffer cmd = vkCmd->GetVkCommandBuffer();
                submitInfo.pCommandBuffers = &cmd;

                vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            }

            void VulkanDevice::DestroyCommandList(IRHICommandList* commandList)
            {
                delete commandList;
            }

            void VulkanDevice::BeginFrame()
            {
                ResetStatistics();

                // Wait for this frame's fence before starting new work
                if (!m_frameFences.empty())
                {
                    vkWaitForFences(m_device, 1, &m_frameFences[m_currentFrame], VK_TRUE, UINT64_MAX);
                    vkResetFences(m_device, 1, &m_frameFences[m_currentFrame]);
                }
            }

            void VulkanDevice::EndFrame()
            {
                // Submit the immediate command list if recording
                auto* cmdList = static_cast<VulkanCommandList*>(GetImmediateCommandList());
                VkCommandBuffer cmd = cmdList->GetVkCommandBuffer();

                VkSubmitInfo submitInfo = {};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &cmd;

                VkFence frameFence = VK_NULL_HANDLE;
                if (!m_frameFences.empty())
                {
                    frameFence = m_frameFences[m_currentFrame];
                }

                vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frameFence);

                // Advance frame index
                if (!m_frameFences.empty())
                {
                    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
                }
            }
            void VulkanDevice::WaitForIdle()
            {
                if (m_device != VK_NULL_HANDLE)
                    vkDeviceWaitIdle(m_device);
            }

            void VulkanDevice::ResetStatistics()
            {
                m_statistics = {};
            }

            std::string VulkanDevice::GetDeviceInfo() const
            {
                std::string info = "=== Vulkan Device Info ===\n";
                info += "Device: " + m_capabilities.deviceName + "\n";
                info += "API: " + m_capabilities.apiVersion + "\n";
                info += "VRAM: " + std::to_string(m_capabilities.dedicatedVideoMemory / (1024 * 1024)) + " MB\n";
                info += "Max Texture Size: " + std::to_string(m_capabilities.maxTextureSize) + "\n";
                info += "Max Color Attachments: " + std::to_string(m_capabilities.maxRenderTargets) + "\n";
                info += "Tessellation: " + std::string(m_capabilities.tessellationSupport ? "Yes" : "No") + "\n";
                info += "Geometry Shaders: " + std::string(m_capabilities.geometryShaderSupport ? "Yes" : "No") + "\n";
                return info;
            }

            uint32_t VulkanDevice::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
            {
                VkPhysicalDeviceMemoryProperties memProperties;
                vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

                for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i)
                {
                    if ((typeFilter & (1 << i)) &&
                        (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
                    {
                        return i;
                    }
                }
                return 0;
            }

            // ============================================================================
            // FORMAT CONVERSION HELPERS
            // ============================================================================

            uint32_t VulkanDevice::GetVertexFormatSize(RHIVertexFormat format) const
            {
                switch (format)
                {
                case RHIVertexFormat::Float1:
                case RHIVertexFormat::Int1:
                case RHIVertexFormat::UInt1:
                    return 4;
                case RHIVertexFormat::Float2:
                case RHIVertexFormat::Int2:
                case RHIVertexFormat::UInt2:
                    return 8;
                case RHIVertexFormat::Float3:
                case RHIVertexFormat::Int3:
                case RHIVertexFormat::UInt3:
                    return 12;
                case RHIVertexFormat::Float4:
                case RHIVertexFormat::Int4:
                case RHIVertexFormat::UInt4:
                    return 16;
                case RHIVertexFormat::UNorm8x4:
                case RHIVertexFormat::SNorm8x4:
                    return 4;
                default:
                    return 4;
                }
            }

            VkBorderColor VulkanDevice::ConvertBorderColor(const float borderColor[4]) const
            {
                // Vulkan only supports specific border color enum values
                if (borderColor[3] == 0.0f)
                {
                    if (borderColor[0] == 0.0f && borderColor[1] == 0.0f && borderColor[2] == 0.0f)
                        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
                }
                if (borderColor[0] == 0.0f && borderColor[1] == 0.0f && borderColor[2] == 0.0f &&
                    borderColor[3] == 1.0f)
                    return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
                if (borderColor[0] == 1.0f && borderColor[1] == 1.0f && borderColor[2] == 1.0f &&
                    borderColor[3] == 1.0f)
                    return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
                // Default to transparent black for unsupported custom border colors
                return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            }

            VkFormat VulkanDevice::ConvertFormat(PixelFormat format) const
            {
                switch (format)
                {
                case PixelFormat::R8_UNORM:
                    return VK_FORMAT_R8_UNORM;
                case PixelFormat::R8_SNORM:
                    return VK_FORMAT_R8_SNORM;
                case PixelFormat::R8_UINT:
                    return VK_FORMAT_R8_UINT;
                case PixelFormat::R8G8_UNORM:
                    return VK_FORMAT_R8G8_UNORM;
                case PixelFormat::R8G8B8A8_UNORM:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case PixelFormat::R8G8B8A8_UNORM_SRGB:
                    return VK_FORMAT_R8G8B8A8_SRGB;
                case PixelFormat::R8G8B8A8_SNORM:
                    return VK_FORMAT_R8G8B8A8_SNORM;
                case PixelFormat::B8G8R8A8_UNORM:
                    return VK_FORMAT_B8G8R8A8_UNORM;
                case PixelFormat::B8G8R8A8_UNORM_SRGB:
                    return VK_FORMAT_B8G8R8A8_SRGB;
                case PixelFormat::R10G10B10A2_UNORM:
                    return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
                case PixelFormat::R11G11B10_FLOAT:
                    return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
                case PixelFormat::R16_FLOAT:
                    return VK_FORMAT_R16_SFLOAT;
                case PixelFormat::R16_UINT:
                    return VK_FORMAT_R16_UINT;
                case PixelFormat::R16G16_FLOAT:
                    return VK_FORMAT_R16G16_SFLOAT;
                case PixelFormat::R16G16B16A16_FLOAT:
                    return VK_FORMAT_R16G16B16A16_SFLOAT;
                case PixelFormat::R16G16B16A16_UNORM:
                    return VK_FORMAT_R16G16B16A16_UNORM;
                case PixelFormat::R32_FLOAT:
                    return VK_FORMAT_R32_SFLOAT;
                case PixelFormat::R32_UINT:
                    return VK_FORMAT_R32_UINT;
                case PixelFormat::R32G32_FLOAT:
                    return VK_FORMAT_R32G32_SFLOAT;
                case PixelFormat::R32G32B32_FLOAT:
                    return VK_FORMAT_R32G32B32_SFLOAT;
                case PixelFormat::R32G32B32A32_FLOAT:
                    return VK_FORMAT_R32G32B32A32_SFLOAT;
                case PixelFormat::BC1_UNORM:
                    return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
                case PixelFormat::BC1_UNORM_SRGB:
                    return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
                case PixelFormat::BC2_UNORM:
                    return VK_FORMAT_BC2_UNORM_BLOCK;
                case PixelFormat::BC3_UNORM:
                    return VK_FORMAT_BC3_UNORM_BLOCK;
                case PixelFormat::BC3_UNORM_SRGB:
                    return VK_FORMAT_BC3_SRGB_BLOCK;
                case PixelFormat::BC4_UNORM:
                    return VK_FORMAT_BC4_UNORM_BLOCK;
                case PixelFormat::BC5_UNORM:
                    return VK_FORMAT_BC5_UNORM_BLOCK;
                case PixelFormat::BC6H_UF16:
                    return VK_FORMAT_BC6H_UFLOAT_BLOCK;
                case PixelFormat::BC7_UNORM:
                    return VK_FORMAT_BC7_UNORM_BLOCK;
                case PixelFormat::BC7_UNORM_SRGB:
                    return VK_FORMAT_BC7_SRGB_BLOCK;
                case PixelFormat::D16_UNORM:
                    return VK_FORMAT_D16_UNORM;
                case PixelFormat::D24_UNORM_S8_UINT:
                    return VK_FORMAT_D24_UNORM_S8_UINT;
                case PixelFormat::D32_FLOAT:
                    return VK_FORMAT_D32_SFLOAT;
                case PixelFormat::D32_FLOAT_S8_UINT:
                    return VK_FORMAT_D32_SFLOAT_S8_UINT;
                default:
                    return VK_FORMAT_UNDEFINED;
                }
            }

            VkFilter VulkanDevice::ConvertFilter(RHIFilterMode mode) const
            {
                return (mode == RHIFilterMode::Nearest) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            }

            VkSamplerAddressMode VulkanDevice::ConvertAddressMode(RHIAddressMode mode) const
            {
                switch (mode)
                {
                case RHIAddressMode::Wrap:
                    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case RHIAddressMode::Clamp:
                    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                case RHIAddressMode::Mirror:
                    return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case RHIAddressMode::Border:
                    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                case RHIAddressMode::MirrorOnce:
                    return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
                default:
                    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                }
            }

            VkCompareOp VulkanDevice::ConvertCompareOp(RHICompareOp op) const
            {
                switch (op)
                {
                case RHICompareOp::Never:
                    return VK_COMPARE_OP_NEVER;
                case RHICompareOp::Less:
                    return VK_COMPARE_OP_LESS;
                case RHICompareOp::Equal:
                    return VK_COMPARE_OP_EQUAL;
                case RHICompareOp::LessEqual:
                    return VK_COMPARE_OP_LESS_OR_EQUAL;
                case RHICompareOp::Greater:
                    return VK_COMPARE_OP_GREATER;
                case RHICompareOp::NotEqual:
                    return VK_COMPARE_OP_NOT_EQUAL;
                case RHICompareOp::GreaterEqual:
                    return VK_COMPARE_OP_GREATER_OR_EQUAL;
                case RHICompareOp::Always:
                    return VK_COMPARE_OP_ALWAYS;
                default:
                    return VK_COMPARE_OP_LESS;
                }
            }

            VkStencilOp VulkanDevice::ConvertStencilOp(RHIStencilOp op) const
            {
                switch (op)
                {
                case RHIStencilOp::Keep:
                    return VK_STENCIL_OP_KEEP;
                case RHIStencilOp::Zero:
                    return VK_STENCIL_OP_ZERO;
                case RHIStencilOp::Replace:
                    return VK_STENCIL_OP_REPLACE;
                case RHIStencilOp::IncrSat:
                    return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
                case RHIStencilOp::DecrSat:
                    return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
                case RHIStencilOp::Invert:
                    return VK_STENCIL_OP_INVERT;
                case RHIStencilOp::IncrWrap:
                    return VK_STENCIL_OP_INCREMENT_AND_WRAP;
                case RHIStencilOp::DecrWrap:
                    return VK_STENCIL_OP_DECREMENT_AND_WRAP;
                default:
                    return VK_STENCIL_OP_KEEP;
                }
            }

            VkBlendFactor VulkanDevice::ConvertBlendFactor(RHIBlendFactor factor) const
            {
                switch (factor)
                {
                case RHIBlendFactor::Zero:
                    return VK_BLEND_FACTOR_ZERO;
                case RHIBlendFactor::One:
                    return VK_BLEND_FACTOR_ONE;
                case RHIBlendFactor::SrcColor:
                    return VK_BLEND_FACTOR_SRC_COLOR;
                case RHIBlendFactor::InvSrcColor:
                    return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                case RHIBlendFactor::SrcAlpha:
                    return VK_BLEND_FACTOR_SRC_ALPHA;
                case RHIBlendFactor::InvSrcAlpha:
                    return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                case RHIBlendFactor::DstAlpha:
                    return VK_BLEND_FACTOR_DST_ALPHA;
                case RHIBlendFactor::InvDstAlpha:
                    return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
                case RHIBlendFactor::DstColor:
                    return VK_BLEND_FACTOR_DST_COLOR;
                case RHIBlendFactor::InvDstColor:
                    return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
                default:
                    return VK_BLEND_FACTOR_ZERO;
                }
            }

            VkBlendOp VulkanDevice::ConvertBlendOp(RHIBlendOp op) const
            {
                switch (op)
                {
                case RHIBlendOp::Add:
                    return VK_BLEND_OP_ADD;
                case RHIBlendOp::Subtract:
                    return VK_BLEND_OP_SUBTRACT;
                case RHIBlendOp::RevSubtract:
                    return VK_BLEND_OP_REVERSE_SUBTRACT;
                case RHIBlendOp::Min:
                    return VK_BLEND_OP_MIN;
                case RHIBlendOp::Max:
                    return VK_BLEND_OP_MAX;
                default:
                    return VK_BLEND_OP_ADD;
                }
            }

            VkPrimitiveTopology VulkanDevice::ConvertTopology(RHIPrimitiveTopology topology) const
            {
                switch (topology)
                {
                case RHIPrimitiveTopology::PointList:
                    return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                case RHIPrimitiveTopology::LineList:
                    return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
                case RHIPrimitiveTopology::LineStrip:
                    return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
                case RHIPrimitiveTopology::TriangleList:
                    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                case RHIPrimitiveTopology::TriangleStrip:
                    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
                case RHIPrimitiveTopology::PatchList:
                    return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
                default:
                    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                }
            }

            VkFormat VulkanDevice::ConvertVertexFormat(RHIVertexFormat format) const
            {
                switch (format)
                {
                case RHIVertexFormat::Float1:
                    return VK_FORMAT_R32_SFLOAT;
                case RHIVertexFormat::Float2:
                    return VK_FORMAT_R32G32_SFLOAT;
                case RHIVertexFormat::Float3:
                    return VK_FORMAT_R32G32B32_SFLOAT;
                case RHIVertexFormat::Float4:
                    return VK_FORMAT_R32G32B32A32_SFLOAT;
                case RHIVertexFormat::Int1:
                    return VK_FORMAT_R32_SINT;
                case RHIVertexFormat::Int2:
                    return VK_FORMAT_R32G32_SINT;
                case RHIVertexFormat::Int3:
                    return VK_FORMAT_R32G32B32_SINT;
                case RHIVertexFormat::Int4:
                    return VK_FORMAT_R32G32B32A32_SINT;
                case RHIVertexFormat::UInt1:
                    return VK_FORMAT_R32_UINT;
                case RHIVertexFormat::UInt2:
                    return VK_FORMAT_R32G32_UINT;
                case RHIVertexFormat::UInt3:
                    return VK_FORMAT_R32G32B32_UINT;
                case RHIVertexFormat::UInt4:
                    return VK_FORMAT_R32G32B32A32_UINT;
                case RHIVertexFormat::UNorm8x4:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case RHIVertexFormat::SNorm8x4:
                    return VK_FORMAT_R8G8B8A8_SNORM;
                default:
                    return VK_FORMAT_R32G32B32_SFLOAT;
                }
            }

        } // namespace Vulkan
    } // namespace RHI
} // namespace Spark

#endif // SPARK_VULKAN_SUPPORT
