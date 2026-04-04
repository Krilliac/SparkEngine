/**
 * @file VulkanCommandList.cpp
 * @brief VulkanSwapChain and VulkanCommandList implementations
 *
 * Split from VulkanDevice.cpp for maintainability.
 */

#ifdef SPARK_VULKAN_SUPPORT

#include "VulkanDevice.h"
#include "../../../Utils/Validate.h"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace Spark
{
    namespace RHI
    {
        namespace Vulkan
        {

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
                if (!m_queueFamilies.graphicsFamily.has_value() || !m_queueFamilies.presentFamily.has_value())
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "Queue family indices not available for swap chain creation");
                    return false;
                }

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
                                                 RHIStatistics* statistics,
                                                 PFN_vkCmdPushDescriptorSetKHR pushDescriptorFn)
                : m_device(device), m_commandPool(commandPool), m_isImmediate(isImmediate), m_statistics(statistics),
                  m_vkCmdPushDescriptorSet(pushDescriptorFn)
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

            void VulkanCommandList::SetRenderTargets(IRHITexture* const* renderTargets, uint32_t count,
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
                VkIndexType indexType;
                if (vkBuf->GetStride() == 4)
                    indexType = VK_INDEX_TYPE_UINT32;
#ifdef VK_API_VERSION_1_4
                else if (vkBuf->GetStride() == 1)
                    indexType = VK_INDEX_TYPE_UINT8_KHR; // Vulkan 1.4 core (was VK_KHR_index_type_uint8)
#endif
                else
                    indexType = VK_INDEX_TYPE_UINT16;
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
                if (!m_isRecording)
                    return;

                // Push descriptor path: push pending bindings directly into the command
                // buffer without allocating from the descriptor pool. Available on
                // Vulkan 1.4 (core) or via VK_KHR_push_descriptor extension.
                if (m_vkCmdPushDescriptorSet && m_pendingBindings.dirty && layout != VK_NULL_HANDLE)
                {
                    std::vector<VkWriteDescriptorSet> writes;
                    std::vector<VkDescriptorBufferInfo> bufInfos;
                    std::vector<VkDescriptorImageInfo> imgInfos;
                    bufInfos.reserve(m_pendingBindings.constantBuffers.size());
                    imgInfos.reserve(m_pendingBindings.shaderResources.size());

                    for (const auto& [slot, buffer] : m_pendingBindings.constantBuffers)
                    {
                        auto& bi = bufInfos.emplace_back();
                        bi.buffer = buffer;
                        bi.offset = 0;
                        bi.range = m_pendingBindings.constantBufferSizes[slot];

                        VkWriteDescriptorSet w = {};
                        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        w.dstBinding = slot;
                        w.descriptorCount = 1;
                        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                        w.pBufferInfo = &bufInfos.back();
                        writes.push_back(w);
                    }

                    for (const auto& [slot, imageView] : m_pendingBindings.shaderResources)
                    {
                        auto& ii = imgInfos.emplace_back();
                        ii.imageView = imageView;
                        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        auto sampIt = m_pendingBindings.samplers.find(slot);
                        ii.sampler = (sampIt != m_pendingBindings.samplers.end()) ? sampIt->second : VK_NULL_HANDLE;

                        VkWriteDescriptorSet w = {};
                        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        w.dstBinding = 14 + slot; // Texture bindings start at 14
                        w.descriptorCount = 1;
                        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        w.pImageInfo = &imgInfos.back();
                        writes.push_back(w);
                    }

                    if (!writes.empty())
                    {
                        m_vkCmdPushDescriptorSet(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                                                 static_cast<uint32_t>(writes.size()), writes.data());
                    }
                    m_pendingBindings.dirty = false;
                    return;
                }

                // Traditional descriptor set path
                if (descriptorSet != VK_NULL_HANDLE)
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

            void VulkanCommandList::DrawInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
            {
                if (!argsBuffer)
                    return;
                auto* vkBuf = static_cast<VulkanBuffer*>(argsBuffer);
                vkCmdDrawIndirect(m_commandBuffer, vkBuf->GetVkBuffer(), argsOffset, 1, 0);
                if (m_statistics)
                    m_statistics->drawCalls++;
            }

            void VulkanCommandList::DrawIndexedInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
            {
                if (!argsBuffer)
                    return;
                auto* vkBuf = static_cast<VulkanBuffer*>(argsBuffer);
                vkCmdDrawIndexedIndirect(m_commandBuffer, vkBuf->GetVkBuffer(), argsOffset, 1, 0);
                if (m_statistics)
                    m_statistics->drawCalls++;
            }

            void VulkanCommandList::DispatchIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
            {
                if (!argsBuffer)
                    return;
                auto* vkBuf = static_cast<VulkanBuffer*>(argsBuffer);
                vkCmdDispatchIndirect(m_commandBuffer, vkBuf->GetVkBuffer(), argsOffset);
                if (m_statistics)
                    m_statistics->dispatchCalls++;
            }

            void VulkanCommandList::CopyTexture(IRHITexture* dst, IRHITexture* src)
            {
                if (!dst || !src)
                    return;
                auto* vkDst = static_cast<VulkanTexture*>(dst);
                auto* vkSrc = static_cast<VulkanTexture*>(src);
                VkImageCopy region = {};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.extent = {vkSrc->GetWidth(), vkSrc->GetHeight(), 1};
                vkCmdCopyImage(m_commandBuffer, vkSrc->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               vkDst->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
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


        } // namespace Vulkan
    } // namespace RHI
} // namespace Spark

#endif // SPARK_VULKAN_SUPPORT
