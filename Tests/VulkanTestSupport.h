/**
 * @file VulkanTestSupport.h
 * @brief Shared harness for the real-Vulkan test files (RHI-230): a VulkanDevice
 *        with VK_LAYER_KHRONOS_validation on and ERROR-severity messages counted
 *        as defects, plus the SPARK_REQUIRE_VULKAN_VALIDATION skip rule.
 *
 * Included only by translation units built with SPARK_VULKAN_SUPPORT. Every test
 * that uses ValidatedDevice must end with `v.ExpectClean()`.
 *
 * Lavapipe is a CPU implementation: a clean run proves API-usage correctness
 * (layouts, barriers, descriptor lifetime, submission state), not hardware
 * certification.
 */

#pragma once

#ifdef SPARK_VULKAN_SUPPORT

#include "TestFramework.h"

#include "Graphics/RHI/Vulkan/VulkanDevice.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace SparkVkTest
{
    struct ValidationCounter
    {
        uint32_t errors = 0;
        std::string firstError;
    };

    inline VKAPI_ATTR VkBool32 VKAPI_CALL CountValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                                 VkDebugUtilsMessageTypeFlagsEXT,
                                                                 const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                                 void* userData)
    {
        auto* counter = static_cast<ValidationCounter*>(userData);
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        {
            if (counter->errors == 0 && data && data->pMessage)
                counter->firstError = data->pMessage;
            ++counter->errors;
        }
        return VK_FALSE;
    }

    inline bool ValidationLayerInstalled()
    {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());
        for (const auto& layer : layers)
        {
            if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                return true;
        }
        return false;
    }

    /// A lane that sets SPARK_REQUIRE_VULKAN_VALIDATION=1 (the vulkan-lavapipe CI row) must not pass by
    /// skipping every test, so a missing driver or layer becomes a failure there.
    [[noreturn]] inline void SkipOrFail(const char* reason)
    {
        if (std::getenv("SPARK_REQUIRE_VULKAN_VALIDATION") != nullptr)
            throw std::runtime_error(std::string("required Vulkan validation unavailable: ") + reason);
        SKIP_TEST(reason);
    }

    /// Real VulkanDevice with the validation layer on and an error counter attached.
    struct ValidatedDevice
    {
        Spark::RHI::Vulkan::VulkanDevice device;
        ValidationCounter counter;
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

        explicit ValidatedDevice(const char* applicationName = "RHI230Validation")
        {
            if (!ValidationLayerInstalled())
                SkipOrFail("VK_LAYER_KHRONOS_validation is not installed");

            Spark::RHI::RHIDeviceDesc desc;
            desc.enableDebugLayer = true;
            desc.applicationName = applicationName;
            if (!device.Initialize(desc))
                SkipOrFail("no Vulkan ICD available (install mesa-vulkan-drivers for Lavapipe)");

            VkDebugUtilsMessengerCreateInfoEXT info = {};
            info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            info.pfnUserCallback = CountValidationMessage;
            info.pUserData = &counter;
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(device.GetVkInstance(), "vkCreateDebugUtilsMessengerEXT"));
            if (create)
                create(device.GetVkInstance(), &info, nullptr, &messenger);
        }

        ~ValidatedDevice()
        {
            device.WaitForIdle();
            DestroyMessenger();
            device.Shutdown();
        }

        ValidatedDevice(const ValidatedDevice&) = delete;
        ValidatedDevice& operator=(const ValidatedDevice&) = delete;

        void DestroyMessenger()
        {
            if (messenger == VK_NULL_HANDLE)
                return;
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(device.GetVkInstance(), "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy)
                destroy(device.GetVkInstance(), messenger, nullptr);
            messenger = VK_NULL_HANDLE;
        }

        bool HasMessenger() const { return messenger != VK_NULL_HANDLE; }

        void ExpectClean()
        {
            EXPECT_TRUE(HasMessenger());
            if (counter.errors != 0)
                std::cerr << "  first validation error: " << counter.firstError << "\n";
            EXPECT_EQ(counter.errors, 0u);
        }
    };
} // namespace SparkVkTest

#endif // SPARK_VULKAN_SUPPORT
