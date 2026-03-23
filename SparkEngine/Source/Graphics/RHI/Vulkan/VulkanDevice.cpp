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
#include "../RHIFormatUtils.h"
#include "../../../Utils/Validate.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <set>

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
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Vulkan Validation: %s", callbackData->pMessage);
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
                if (vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &m_pipelineCache) != VK_SUCCESS)
                    return false;

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

                if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
                    return false;

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
                    if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_frameFences[i]) != VK_SUCCESS)
                        return false;
                    if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]) !=
                        VK_SUCCESS)
                        return false;
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
                if (!m_queueFamilies.graphicsFamily.has_value())
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Graphics queue family not found");
                    return false;
                }

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

                // Build extension list — add RT extensions if available
                std::vector<const char*> enabledExtensions(m_deviceExtensions.begin(), m_deviceExtensions.end());
                {
                    uint32_t extCount = 0;
                    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, nullptr);
                    std::vector<VkExtensionProperties> availableExts(extCount);
                    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, availableExts.data());

                    auto hasExt = [&](const char* name)
                    {
                        for (const auto& e : availableExts)
                            if (std::strcmp(e.extensionName, name) == 0)
                                return true;
                        return false;
                    };

                    const char* rtExts[] = {
                        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                        VK_KHR_RAY_QUERY_EXTENSION_NAME,
                    };
                    for (const char* ext : rtExts)
                    {
                        if (hasExt(ext))
                            enabledExtensions.push_back(ext);
                    }
                }

                VkDeviceCreateInfo createInfo = {};
                createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
                createInfo.pNext = &vulkan12Features;
                createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
                createInfo.pQueueCreateInfos = queueCreateInfos.data();
                createInfo.pEnabledFeatures = &deviceFeatures;
                createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
                createInfo.ppEnabledExtensionNames = enabledExtensions.data();

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
                if (!m_queueFamilies.graphicsFamily.has_value())
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Graphics queue family not found");
                    return false;
                }

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
                        // Query actual max recursion depth via pNext chain
                        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtPipelineProps = {};
                        rtPipelineProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
                        VkPhysicalDeviceProperties2 props2 = {};
                        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                        props2.pNext = &rtPipelineProps;
                        vkGetPhysicalDeviceProperties2(m_physicalDevice, &props2);

                        m_capabilities.rayTracing.bestBackend = RayTracingBackend::HardwareVKRT;
                        m_capabilities.rayTracing.supportsHardwareRT = true;
                        m_capabilities.rayTracing.supportsInlineRT = hasRayQuery;
                        m_capabilities.rayTracing.maxRecursionDepth = rtPipelineProps.maxRayRecursionDepth > 0
                                                                          ? rtPipelineProps.maxRayRecursionDepth
                                                                          : 1; // Spec minimum
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
                if (vkCreateWin32SurfaceKHR(m_instance, &surfaceInfo, nullptr, &surface) != VK_SUCCESS)
                    return nullptr;
#endif

                return std::make_unique<VulkanSwapChain>(m_device, m_physicalDevice, surface, desc, m_queueFamilies,
                                                         m_presentQueue);
            }

            std::unique_ptr<IRHIBuffer> VulkanDevice::CreateBuffer(const RHIBufferDesc& desc)
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
                return std::make_unique<VulkanBuffer>(desc, buffer, memory, m_device);
            }

            std::unique_ptr<IRHITexture> VulkanDevice::CreateTexture(const RHITextureDesc& desc)
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

                return std::make_unique<VulkanTexture>(desc, image, memory, imageView, m_device);
            }

            std::unique_ptr<IRHITexture> VulkanDevice::WrapNativeTexture(void* nativeHandle, const RHITextureDesc& desc)
            {
                if (!nativeHandle)
                    return nullptr;
                // Wrap an externally-owned VkImage — caller manages lifetime
                auto image = static_cast<VkImage>(nativeHandle);
                return std::make_unique<VulkanTexture>(desc, image, VK_NULL_HANDLE, VK_NULL_HANDLE, m_device, false);
            }

            std::unique_ptr<IRHIShader> VulkanDevice::CreateShader(const RHIShaderDesc& desc)
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

                return std::make_unique<VulkanShader>(desc, module, m_device, std::move(spirvCode));
            }

            std::unique_ptr<IRHISampler> VulkanDevice::CreateSampler(const RHISamplerDesc& desc)
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

                return std::make_unique<VulkanSampler>(desc, sampler, m_device);
            }

            std::unique_ptr<IRHIPipelineState> VulkanDevice::CreatePipelineState(const RHIPipelineStateDesc& desc,
                                                                                 IRHIShader* vertexShader,
                                                                                 IRHIShader* pixelShader)
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
                    uint32_t elemSize = Spark::RHI::GetVertexFormatSize(elem.format);
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

                return std::make_unique<VulkanPipelineState>(desc, pipeline, pipelineLayout, m_device);
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

            std::unique_ptr<IRHICommandList> VulkanDevice::CreateDeferredCommandList()
            {
                return std::make_unique<VulkanCommandList>(m_device, m_commandPool, false);
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


        } // namespace Vulkan
    } // namespace RHI
} // namespace Spark

#endif // SPARK_VULKAN_SUPPORT
