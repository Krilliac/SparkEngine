/**
 * @file VulkanSwapChain.cpp
 * @brief VulkanSwapChain implementation: surface-limit clamping, host-synchronized acquire/present, resize
 *
 * Split from VulkanCommandList.cpp for maintainability.
 */

#ifdef SPARK_VULKAN_SUPPORT

#include "VulkanDevice.h"
#include "../../../Utils/Validate.h"
#include <algorithm>
#include <string>

namespace Spark
{
    namespace RHI
    {
        namespace Vulkan
        {
            namespace
            {
                constexpr uint64_t kHostWaitTimeoutNs = 10ull * 1000ull * 1000ull * 1000ull;
            } // namespace

            // ============================================================================
            // VULKAN SWAP CHAIN
            // ============================================================================

            VulkanSwapChain::VulkanSwapChain(VkInstance instance, VkDevice device, VkPhysicalDevice physDevice,
                                             VkSurfaceKHR surface, const RHISwapChainDesc& desc,
                                             const QueueFamilyIndices& queueFamilies, VkQueue presentQueue)
                : m_desc(desc), m_instance(instance), m_device(device), m_physDevice(physDevice), m_surface(surface),
                  m_queueFamilies(queueFamilies), m_presentQueue(presentQueue)
            {
                if (CreateSyncObjects() && CreateSwapChain(VK_NULL_HANDLE))
                    CreateImageViews();
            }

            VulkanSwapChain::~VulkanSwapChain()
            {
                if (m_presentQueue != VK_NULL_HANDLE)
                    vkQueueWaitIdle(m_presentQueue);
                Cleanup();
                if (m_transitionPool != VK_NULL_HANDLE)
                    vkDestroyCommandPool(m_device, m_transitionPool, nullptr);
                if (m_acquireFence != VK_NULL_HANDLE)
                    vkDestroyFence(m_device, m_acquireFence, nullptr);
                if (m_transitionFence != VK_NULL_HANDLE)
                    vkDestroyFence(m_device, m_transitionFence, nullptr);
                // The surface outlives Resize; it must be gone before the instance is destroyed.
                if (m_surface != VK_NULL_HANDLE && m_instance != VK_NULL_HANDLE)
                    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
            }

            bool VulkanSwapChain::CreateSwapChain(VkSwapchainKHR oldSwapChain)
            {
                if (!m_queueFamilies.graphicsFamily.has_value() || !m_queueFamilies.presentFamily.has_value())
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "Queue family indices not available for swap chain creation");
                    return false;
                }

                VkBool32 presentSupported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(m_physDevice, m_queueFamilies.presentFamily.value(), m_surface,
                                                     &presentSupported);
                if (presentSupported != VK_TRUE)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "VulkanSwapChain: present queue family cannot present to this surface");
                    return false;
                }

                VkSurfaceCapabilitiesKHR capabilities = {};
                if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physDevice, m_surface, &capabilities) != VK_SUCCESS)
                    return false;

                // Image count and extent must sit inside the surface limits (maxImageCount 0 = unbounded;
                // currentExtent 0xFFFFFFFF = the swap chain decides).
                uint32_t imageCount = std::max(m_desc.bufferCount, capabilities.minImageCount);
                if (capabilities.maxImageCount > 0)
                    imageCount = std::min(imageCount, capabilities.maxImageCount);

                VkExtent2D extent = capabilities.currentExtent;
                if (extent.width == UINT32_MAX)
                {
                    extent.width =
                        std::clamp(m_desc.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
                    extent.height = std::clamp(m_desc.height, capabilities.minImageExtent.height,
                                               capabilities.maxImageExtent.height);
                }
                if (extent.width == 0 || extent.height == 0)
                    return false;

                // Prefer BGRA8 UNORM (what the engine's back-buffer desc advertises); otherwise take the
                // surface's first format rather than requesting one it does not support.
                uint32_t formatCount = 0;
                vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDevice, m_surface, &formatCount, nullptr);
                std::vector<VkSurfaceFormatKHR> formats(formatCount);
                vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDevice, m_surface, &formatCount, formats.data());
                if (formats.empty())
                    return false;
                VkSurfaceFormatKHR surfaceFormat = formats[0];
                for (const auto& candidate : formats)
                {
                    if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM &&
                        candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    {
                        surfaceFormat = candidate;
                        break;
                    }
                }
                m_vkFormat = surfaceFormat.format;

                // FIFO is the only mode every implementation must support; MAILBOX only when offered.
                VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
                if (!m_desc.vsync)
                {
                    uint32_t modeCount = 0;
                    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physDevice, m_surface, &modeCount, nullptr);
                    std::vector<VkPresentModeKHR> modes(modeCount);
                    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physDevice, m_surface, &modeCount, modes.data());
                    if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end())
                        presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
                }

                VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
                if (!(capabilities.supportedCompositeAlpha & compositeAlpha))
                {
                    for (VkCompositeAlphaFlagBitsKHR candidate :
                         {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                          VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
                    {
                        if (capabilities.supportedCompositeAlpha & candidate)
                        {
                            compositeAlpha = candidate;
                            break;
                        }
                    }
                }

                VkSwapchainCreateInfoKHR createInfo = {};
                createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
                createInfo.surface = m_surface;
                createInfo.minImageCount = imageCount;
                createInfo.imageFormat = surfaceFormat.format;
                createInfo.imageColorSpace = surfaceFormat.colorSpace;
                createInfo.imageExtent = extent;
                createInfo.imageArrayLayers = 1;
                createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        (capabilities.supportedUsageFlags &
                                         (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));

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
                createInfo.compositeAlpha = compositeAlpha;
                createInfo.presentMode = presentMode;
                createInfo.clipped = VK_TRUE;
                createInfo.oldSwapchain = oldSwapChain;

                VkResult result = vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapChain);
                if (result != VK_SUCCESS)
                {
                    m_swapChain = VK_NULL_HANDLE;
                    return false;
                }

                m_desc.width = extent.width;
                m_desc.height = extent.height;
                m_imageAcquired = false;
                m_currentImageIndex = 0;

                uint32_t swapImageCount = 0;
                vkGetSwapchainImagesKHR(m_device, m_swapChain, &swapImageCount, nullptr);
                m_swapChainImages.resize(swapImageCount);
                vkGetSwapchainImagesKHR(m_device, m_swapChain, &swapImageCount, m_swapChainImages.data());

                return true;
            }

            bool VulkanSwapChain::CreateImageViews()
            {
                m_backBuffers.clear();
                m_backBuffers.reserve(m_swapChainImages.size());

                const PixelFormat backBufferFormat =
                    (m_vkFormat == VK_FORMAT_B8G8R8A8_UNORM) ? PixelFormat::B8G8R8A8_UNORM : m_desc.format;
                for (size_t i = 0; i < m_swapChainImages.size(); ++i)
                {
                    VkImageViewCreateInfo viewInfo = {};
                    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                    viewInfo.image = m_swapChainImages[i];
                    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    viewInfo.format = m_vkFormat;
                    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    viewInfo.subresourceRange.baseMipLevel = 0;
                    viewInfo.subresourceRange.levelCount = 1;
                    viewInfo.subresourceRange.baseArrayLayer = 0;
                    viewInfo.subresourceRange.layerCount = 1;

                    VkImageView view = VK_NULL_HANDLE;
                    if (vkCreateImageView(m_device, &viewInfo, nullptr, &view) != VK_SUCCESS)
                        return false;

                    RHITextureDesc texDesc;
                    texDesc.width = m_desc.width;
                    texDesc.height = m_desc.height;
                    texDesc.format = backBufferFormat;
                    texDesc.usage = RHITextureUsage::RenderTarget;
                    texDesc.debugName = "SwapChainImage_" + std::to_string(i);

                    // The wrapper owns (and destroys) the view but not the swap chain image. The view used to
                    // be destroyed a second time by Cleanup.
                    m_backBuffers.push_back(std::make_unique<VulkanTexture>(texDesc, m_swapChainImages[i],
                                                                            VK_NULL_HANDLE, view, m_device, false));
                }

                return true;
            }

            bool VulkanSwapChain::CreateSyncObjects()
            {
                VkFenceCreateInfo fenceInfo = {};
                fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_acquireFence) != VK_SUCCESS ||
                    vkCreateFence(m_device, &fenceInfo, nullptr, &m_transitionFence) != VK_SUCCESS)
                {
                    return false;
                }

                VkCommandPoolCreateInfo poolInfo = {};
                poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                poolInfo.queueFamilyIndex = m_queueFamilies.presentFamily.value_or(0);
                if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_transitionPool) != VK_SUCCESS)
                    return false;

                VkCommandBufferAllocateInfo allocInfo = {};
                allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocInfo.commandPool = m_transitionPool;
                allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocInfo.commandBufferCount = 1;
                return vkAllocateCommandBuffers(m_device, &allocInfo, &m_transitionCmd) == VK_SUCCESS;
            }

            void VulkanSwapChain::DestroyImageViews()
            {
                m_backBuffers.clear(); // each back buffer destroys its own view
                m_swapChainImages.clear();
            }

            void VulkanSwapChain::Cleanup()
            {
                DestroyImageViews();
                if (m_swapChain != VK_NULL_HANDLE)
                    vkDestroySwapchainKHR(m_device, m_swapChain, nullptr);

                // Reset handles so a repeat Cleanup (e.g. destructor after a failed Resize) is a no-op
                m_swapChain = VK_NULL_HANDLE;
                m_imageAcquired = false;
            }

            bool VulkanSwapChain::AcquireNextImage()
            {
                if (m_swapChain == VK_NULL_HANDLE)
                    return false;
                if (m_imageAcquired)
                    return true;

                // Acquire with a fence and wait on the host: the image is then free before any command that
                // writes it is even recorded, without needing a semaphore wired into engine submissions.
                VkResult result = vkAcquireNextImageKHR(m_device, m_swapChain, kHostWaitTimeoutNs, VK_NULL_HANDLE,
                                                        m_acquireFence, &m_currentImageIndex);
                if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
                    return false;

                vkWaitForFences(m_device, 1, &m_acquireFence, VK_TRUE, kHostWaitTimeoutNs);
                vkResetFences(m_device, 1, &m_acquireFence);
                m_imageAcquired = true;

                // Contents of a freshly acquired image are not preserved (flip-discard semantics).
                if (m_currentImageIndex < m_backBuffers.size())
                    m_backBuffers[m_currentImageIndex]->SetCurrentLayout(VK_IMAGE_LAYOUT_UNDEFINED);
                return true;
            }

            bool VulkanSwapChain::Present(bool)
            {
                if (m_swapChain == VK_NULL_HANDLE || !AcquireNextImage())
                    return false;

                // Everything submitted so far (including the frame's writes to this image) must finish before
                // the presentation engine reads it; no semaphore links engine submissions to the swap chain.
                vkQueueWaitIdle(m_presentQueue);

                VulkanTexture* backBuffer = m_backBuffers[m_currentImageIndex].get();
                VkCommandBufferBeginInfo beginInfo = {};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(m_transitionCmd, &beginInfo);
                VulkanCommandList::RecordTextureTransition(m_transitionCmd, backBuffer,
                                                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                vkEndCommandBuffer(m_transitionCmd);

                VkSubmitInfo submitInfo = {};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &m_transitionCmd;
                vkQueueSubmit(m_presentQueue, 1, &submitInfo, m_transitionFence);
                vkWaitForFences(m_device, 1, &m_transitionFence, VK_TRUE, kHostWaitTimeoutNs);
                vkResetFences(m_device, 1, &m_transitionFence);

                VkPresentInfoKHR presentInfo = {};
                presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                presentInfo.swapchainCount = 1;
                presentInfo.pSwapchains = &m_swapChain;
                presentInfo.pImageIndices = &m_currentImageIndex;

                VkResult result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
                m_imageAcquired = false;
                if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
                {
                    // Swap chain needs recreation - caller should handle resize
                    return false;
                }
                return result == VK_SUCCESS;
            }

            bool VulkanSwapChain::Resize(uint32_t width, uint32_t height)
            {
                if (width == 0 || height == 0)
                    return false;

                if (m_presentQueue != VK_NULL_HANDLE)
                    vkQueueWaitIdle(m_presentQueue);

                // An acquired-but-unpresented image of the old chain is released when it is destroyed.
                m_desc.width = width;
                m_desc.height = height;
                VkSwapchainKHR oldSwapChain = m_swapChain;
                DestroyImageViews();
                m_swapChain = VK_NULL_HANDLE;
                const bool created = CreateSwapChain(oldSwapChain);
                if (oldSwapChain != VK_NULL_HANDLE)
                    vkDestroySwapchainKHR(m_device, oldSwapChain, nullptr);
                return created && CreateImageViews();
            }

            IRHITexture* VulkanSwapChain::GetBackBuffer()
            {
                if (!AcquireNextImage())
                    return nullptr;
                if (m_currentImageIndex < m_backBuffers.size())
                    return m_backBuffers[m_currentImageIndex].get();
                return nullptr;
            }

        } // namespace Vulkan
    } // namespace RHI
} // namespace Spark

#endif // SPARK_VULKAN_SUPPORT
