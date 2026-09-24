/**
 * @file VulkanCommandList.cpp
 * @brief VulkanCommandList implementation (layout tracking, dynamic rendering, descriptor flush)
 *
 * Split from VulkanDevice.cpp for maintainability; the swap chain lives in VulkanSwapChain.cpp.
 */

#ifdef SPARK_VULKAN_SUPPORT

#include "VulkanDevice.h"
#include "../RHIFormatUtils.h"
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
            namespace
            {
                VkImageAspectFlags AspectMaskFor(PixelFormat format)
                {
                    if (!IsDepthStencilFormat(format))
                        return VK_IMAGE_ASPECT_COLOR_BIT;
                    return HasStencilComponent(format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                                       : VK_IMAGE_ASPECT_DEPTH_BIT;
                }

                // Access mask and pipeline stage that produce (src side) or consume (dst side) a layout.
                void AccessAndStageFor(VkImageLayout layout, VkAccessFlags& access, VkPipelineStageFlags& stage,
                                       bool isDestination)
                {
                    switch (layout)
                    {
                    case VK_IMAGE_LAYOUT_UNDEFINED:
                        access = 0;
                        stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                        break;
                    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                        access = VK_ACCESS_TRANSFER_WRITE_BIT;
                        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                        break;
                    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                        access = VK_ACCESS_TRANSFER_READ_BIT;
                        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                        break;
                    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                        access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                        stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                        break;
                    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                        access =
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                        stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                        break;
                    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                        access = VK_ACCESS_SHADER_READ_BIT;
                        stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                        break;
                    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                        // Presentation is ordered by the host wait in Present, not by the barrier.
                        access = 0;
                        stage =
                            isDestination ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                        break;
                    default:
                        access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                        stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                        break;
                    }
                }
            } // namespace

            // ============================================================================
            // VULKAN COMMAND LIST
            // ============================================================================

            VulkanCommandList::VulkanCommandList(VkDevice device, VkCommandPool commandPool, bool isImmediate,
                                                 RHIStatistics* statistics,
                                                 PFN_vkCmdPushDescriptorSetKHR pushDescriptorFn,
                                                 VkDescriptorPool descriptorPool, VkDescriptorSetLayout bindingLayout)
                : m_device(device), m_commandPool(commandPool), m_isImmediate(isImmediate), m_statistics(statistics),
                  m_vkCmdPushDescriptorSet(pushDescriptorFn), m_descriptorPool(descriptorPool),
                  m_bindingLayout(bindingLayout)
            {
                VkCommandBufferAllocateInfo allocInfo = {};
                allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocInfo.commandPool = commandPool;
                allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocInfo.commandBufferCount = 1;

                vkAllocateCommandBuffers(device, &allocInfo, &m_commandBuffer);

                VkFenceCreateInfo fenceInfo = {};
                fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                vkCreateFence(device, &fenceInfo, nullptr, &m_submitFence);
            }

            VulkanCommandList::~VulkanCommandList()
            {
                // Freeing a command buffer (or its descriptor sets) while the queue still executes it is
                // undefined; a deferred list is routinely destroyed right after ExecuteCommandList.
                WaitForCompletion();
                ReleaseDescriptorSets();
                if (m_commandBuffer != VK_NULL_HANDLE)
                {
                    vkFreeCommandBuffers(m_device, m_commandPool, 1, &m_commandBuffer);
                }
                if (m_submitFence != VK_NULL_HANDLE)
                    vkDestroyFence(m_device, m_submitFence, nullptr);
            }

            VkFence VulkanCommandList::PrepareSubmit()
            {
                WaitForCompletion();
                vkResetFences(m_device, 1, &m_submitFence);
                m_executable = false;
                m_pending = true;
                return m_submitFence;
            }

            void VulkanCommandList::WaitForCompletion()
            {
                if (m_pending)
                {
                    constexpr uint64_t kSubmitTimeoutNs = 10ull * 1000ull * 1000ull * 1000ull;
                    if (vkWaitForFences(m_device, 1, &m_submitFence, VK_TRUE, kSubmitTimeoutNs) != VK_SUCCESS)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "VulkanCommandList: submission did not complete within 10s");
                    }
                    m_pending = false;
                }
                // The sets belong to the recording that references them. While that recording is still
                // being built or is executable-but-unsubmitted (PrepareSubmit waits on the *previous*
                // submission right before vkQueueSubmit), freeing them invalidates the command buffer:
                // VUID-vkQueueSubmit-pCommandBuffers-00070 "VkDescriptorSet ... was destroyed or updated
                // without UPDATE_AFTER_BIND". Only drivers without push descriptors take this pool path.
                if (!m_isRecording && !m_executable)
                    ReleaseDescriptorSets();
            }

            void VulkanCommandList::ReleaseDescriptorSets()
            {
                if (!m_liveDescriptorSets.empty())
                {
                    vkFreeDescriptorSets(m_device, m_descriptorPool, static_cast<uint32_t>(m_liveDescriptorSets.size()),
                                         m_liveDescriptorSets.data());
                    m_liveDescriptorSets.clear();
                }
            }

            void VulkanCommandList::Begin()
            {
                // Re-recording a command buffer that is still pending is invalid (the immediate list is reused
                // every frame), so drain its previous submission first. Any recording still held is discarded
                // by vkBeginCommandBuffer, so its descriptor sets go with it.
                WaitForCompletion();
                ReleaseDescriptorSets();

                VkCommandBufferBeginInfo beginInfo = {};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

                vkBeginCommandBuffer(m_commandBuffer, &beginInfo);
                m_isRecording = true;
                m_executable = false;

                // Nothing is bound in a fresh command buffer: the redundant-bind cache and the render-target
                // state from a previous recording must not leak into this one.
                m_currentPipeline = VK_NULL_HANDLE;
                m_currentPipelineLayout = VK_NULL_HANDLE;
                m_renderingActive = false;
                m_colorTargets.clear();
                m_depthTarget = nullptr;
                m_pendingBindings = {};
            }

            void VulkanCommandList::End()
            {
                if (m_isRecording)
                {
                    SuspendRendering();
                    vkEndCommandBuffer(m_commandBuffer);
                    m_isRecording = false;
                    m_executable = true;
                }
            }

            void VulkanCommandList::Reset()
            {
                WaitForCompletion();
                ReleaseDescriptorSets();
                vkResetCommandBuffer(m_commandBuffer, 0);
                m_isRecording = false;
                m_executable = false;
                m_renderingActive = false;
            }

            void VulkanCommandList::RecordTextureTransition(VkCommandBuffer cmd, VulkanTexture* texture,
                                                            VkImageLayout newLayout)
            {
                const VkImageLayout oldLayout = texture->GetCurrentLayout();
                if (oldLayout == newLayout)
                    return;

                VkImageMemoryBarrier barrier = {};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = oldLayout;
                barrier.newLayout = newLayout;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = texture->GetVkImage();
                barrier.subresourceRange.aspectMask = AspectMaskFor(texture->GetFormat());
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

                VkPipelineStageFlags srcStage = 0;
                VkPipelineStageFlags dstStage = 0;
                AccessAndStageFor(oldLayout, barrier.srcAccessMask, srcStage, false);
                AccessAndStageFor(newLayout, barrier.dstAccessMask, dstStage, true);

                vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                texture->SetCurrentLayout(newLayout);
            }

            void VulkanCommandList::TransitionTexture(VulkanTexture* texture, VkImageLayout newLayout)
            {
                if (!texture || texture->GetCurrentLayout() == newLayout)
                    return;
                SuspendRendering();
                RecordTextureTransition(m_commandBuffer, texture, newLayout);
            }

            void VulkanCommandList::SuspendRendering()
            {
                if (m_renderingActive)
                {
                    vkCmdEndRendering(m_commandBuffer);
                    m_renderingActive = false;
                }
            }

            void VulkanCommandList::ResumeRendering()
            {
                if (m_renderingActive || (m_colorTargets.empty() && !m_depthTarget))
                    return;

                for (VulkanTexture* target : m_colorTargets)
                    RecordTextureTransition(m_commandBuffer, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                if (m_depthTarget)
                    RecordTextureTransition(m_commandBuffer, m_depthTarget,
                                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

                // Dynamic rendering (Vulkan 1.3). LOAD keeps whatever clears/draws came before a suspension.
                std::vector<VkRenderingAttachmentInfo> colorAttachments(m_colorTargets.size());
                for (size_t i = 0; i < m_colorTargets.size(); ++i)
                {
                    colorAttachments[i] = {};
                    colorAttachments[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    colorAttachments[i].imageView = m_colorTargets[i]->GetVkImageView();
                    colorAttachments[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    colorAttachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    colorAttachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                }

                const VulkanTexture* extentSource = m_colorTargets.empty() ? m_depthTarget : m_colorTargets[0];
                VkRenderingInfo renderingInfo = {};
                renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                renderingInfo.renderArea = {{0, 0}, {extentSource->GetWidth(), extentSource->GetHeight()}};
                renderingInfo.layerCount = 1;
                renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
                renderingInfo.pColorAttachments = colorAttachments.data();

                VkRenderingAttachmentInfo depthAttachment = {};
                if (m_depthTarget)
                {
                    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    depthAttachment.imageView = m_depthTarget->GetVkImageView();
                    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    renderingInfo.pDepthAttachment = &depthAttachment;
                    if (AspectMaskFor(m_depthTarget->GetFormat()) & VK_IMAGE_ASPECT_STENCIL_BIT)
                        renderingInfo.pStencilAttachment = &depthAttachment;
                }

                vkCmdBeginRendering(m_commandBuffer, &renderingInfo);
                m_renderingActive = true;
            }

            void VulkanCommandList::SetRenderTargets(IRHITexture* const* renderTargets, uint32_t count,
                                                     IRHITexture* depthStencil)
            {
                SuspendRendering();
                m_colorTargets.clear();
                for (uint32_t i = 0; i < count; ++i)
                {
                    if (renderTargets && renderTargets[i])
                        m_colorTargets.push_back(static_cast<VulkanTexture*>(renderTargets[i]));
                }
                m_depthTarget = static_cast<VulkanTexture*>(depthStencil);
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
                TransitionTexture(vkTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

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
                TransitionTexture(vkTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

                VkClearDepthStencilValue clearValue;
                clearValue.depth = depth;
                clearValue.stencil = stencil;

                // Depth-only formats have no stencil aspect to clear.
                VkImageSubresourceRange range = {};
                range.aspectMask = AspectMaskFor(vkTex->GetFormat());
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
                VkPipeline pipeline = vkPSO->GetVkPipeline();
                if (pipeline == m_currentPipeline)
                    return; // Skip redundant pipeline bind
                vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                m_currentPipeline = pipeline;
                m_currentPipelineLayout = vkPSO->GetVkLayout();
                m_pendingBindings.dirty = true;
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
                    // Sampled images must be SHADER_READ_ONLY when the draw executes; the barrier cannot be
                    // recorded inside dynamic rendering, so it happens here at bind time.
                    TransitionTexture(vkTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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

            void VulkanCommandList::FlushBindings()
            {
                if (!m_pendingBindings.dirty || m_currentPipelineLayout == VK_NULL_HANDLE)
                    return;

                // SetConstantBuffer/SetShaderResource only record state; nothing reached the GPU until this
                // flush existed, so every descriptor a shader read was unbound at draw time.
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
                    w.pBufferInfo = &bi;
                    writes.push_back(w);
                }

                for (const auto& [slot, imageView] : m_pendingBindings.shaderResources)
                {
                    // A combined image sampler without a sampler is an invalid descriptor; leave the binding
                    // unwritten until SetSampler provides one.
                    auto sampIt = m_pendingBindings.samplers.find(slot);
                    if (sampIt == m_pendingBindings.samplers.end())
                        continue;

                    auto& ii = imgInfos.emplace_back();
                    ii.imageView = imageView;
                    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    ii.sampler = sampIt->second;

                    VkWriteDescriptorSet w = {};
                    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w.dstBinding = 14 + slot; // Texture bindings start at 14
                    w.descriptorCount = 1;
                    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    w.pImageInfo = &ii;
                    writes.push_back(w);
                }
                m_pendingBindings.dirty = false;
                if (writes.empty())
                    return;

                // Push descriptor path (Vulkan 1.4 core / VK_KHR_push_descriptor): no pool allocation.
                if (m_vkCmdPushDescriptorSet)
                {
                    m_vkCmdPushDescriptorSet(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_currentPipelineLayout,
                                             0, static_cast<uint32_t>(writes.size()), writes.data());
                    return;
                }

                // Pool path: one set per flush, kept alive until this recording's submission completes.
                if (m_descriptorPool == VK_NULL_HANDLE || m_bindingLayout == VK_NULL_HANDLE)
                    return;
                VkDescriptorSetAllocateInfo allocInfo = {};
                allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                allocInfo.descriptorPool = m_descriptorPool;
                allocInfo.descriptorSetCount = 1;
                allocInfo.pSetLayouts = &m_bindingLayout;
                VkDescriptorSet set = VK_NULL_HANDLE;
                if (vkAllocateDescriptorSets(m_device, &allocInfo, &set) != VK_SUCCESS)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "VulkanCommandList: descriptor pool exhausted");
                    return;
                }
                m_liveDescriptorSets.push_back(set);
                for (auto& w : writes)
                    w.dstSet = set;
                vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
                vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_currentPipelineLayout, 0, 1,
                                        &set, 0, nullptr);
            }

            void VulkanCommandList::Draw(uint32_t vertexCount, uint32_t startVertex)
            {
                ResumeRendering();
                FlushBindings();
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
                ResumeRendering();
                FlushBindings();
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
                ResumeRendering();
                FlushBindings();
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
                ResumeRendering();
                FlushBindings();
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
                SuspendRendering(); // dispatches are not allowed inside dynamic rendering
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
                ResumeRendering();
                FlushBindings();
                vkCmdDrawIndirect(m_commandBuffer, vkBuf->GetVkBuffer(), argsOffset, 1, 0);
                if (m_statistics)
                    m_statistics->drawCalls++;
            }

            void VulkanCommandList::DrawIndexedInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
            {
                if (!argsBuffer)
                    return;
                auto* vkBuf = static_cast<VulkanBuffer*>(argsBuffer);
                ResumeRendering();
                FlushBindings();
                vkCmdDrawIndexedIndirect(m_commandBuffer, vkBuf->GetVkBuffer(), argsOffset, 1, 0);
                if (m_statistics)
                    m_statistics->drawCalls++;
            }

            void VulkanCommandList::DispatchIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
            {
                if (!argsBuffer)
                    return;
                auto* vkBuf = static_cast<VulkanBuffer*>(argsBuffer);
                SuspendRendering(); // dispatches are not allowed inside dynamic rendering
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
                TransitionTexture(vkSrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                TransitionTexture(vkDst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
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

        } // namespace Vulkan
    } // namespace RHI
} // namespace Spark

#endif // SPARK_VULKAN_SUPPORT
