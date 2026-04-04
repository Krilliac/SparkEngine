/**
 * @file TestVulkanLavapipe.cpp
 * @brief Tests Vulkan RHI backend with Mesa Lavapipe (software Vulkan)
 *
 * Verifies that the Vulkan backend initializes correctly on CPU-only devices.
 * Requires mesa-vulkan-drivers (Lavapipe) to be installed. Tests are skipped
 * gracefully when Vulkan or Lavapipe is unavailable.
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Graphics/RHI/RHIFactory.h"
#include "../SparkEngine/Source/Graphics/RHI/RHIBridge.h"

#ifdef SPARK_VULKAN_SUPPORT
#include "../SparkEngine/Source/Graphics/RHI/Vulkan/VulkanDevice.h"
#endif

// ============================================================================
// Vulkan device initialization (accepts Lavapipe as software fallback)
// ============================================================================

TEST(VulkanLavapipe_DeviceInit)
{
#ifndef SPARK_VULKAN_SUPPORT
    EXPECT_TRUE(true);
    return;
#else
    Spark::RHI::Vulkan::VulkanDevice device;
    Spark::RHI::RHIDeviceDesc desc;
    desc.enableDebugLayer = false;
    desc.applicationName = "LavapipeTest";

    bool ok = device.Initialize(desc);
    if (!ok)
    {
        // No Vulkan ICD available at all — not a test failure
        EXPECT_TRUE(true);
        return;
    }

    auto& caps = device.GetCapabilities();
    EXPECT_TRUE(!caps.deviceName.empty());
    EXPECT_TRUE(caps.maxTextureSize > 0);
    EXPECT_TRUE(caps.maxRenderTargets > 0);

    std::string info = device.GetDeviceInfo();
    EXPECT_TRUE(!info.empty());

    device.Shutdown();
#endif
}

// ============================================================================
// Verify software device detection
// ============================================================================

TEST(VulkanLavapipe_SoftwareDeviceFlag)
{
#ifndef SPARK_VULKAN_SUPPORT
    EXPECT_TRUE(true);
    return;
#else
    Spark::RHI::Vulkan::VulkanDevice device;
    Spark::RHI::RHIDeviceDesc desc;
    desc.enableDebugLayer = false;
    desc.applicationName = "LavapipeFlagTest";

    bool ok = device.Initialize(desc);
    if (!ok)
    {
        EXPECT_TRUE(true);
        return;
    }

    // If running on CI with only Lavapipe, isSoftwareDevice should be true.
    // On a machine with a real GPU, it should be false (GPU is preferred).
    // Either result is valid — we just verify the flag is consistent.
    bool isSoftware = device.IsSoftwareDevice();
    EXPECT_TRUE(device.GetCapabilities().isSoftwareDevice == isSoftware);

    device.Shutdown();
#endif
}

// ============================================================================
// Vulkan 1.4 feature detection
// ============================================================================

TEST(VulkanLavapipe_Vulkan14FeatureDetection)
{
#ifndef SPARK_VULKAN_SUPPORT
    EXPECT_TRUE(true);
    return;
#else
    Spark::RHI::Vulkan::VulkanDevice device;
    Spark::RHI::RHIDeviceDesc desc;
    desc.enableDebugLayer = false;
    desc.applicationName = "Vulkan14FeatureTest";

    bool ok = device.Initialize(desc);
    if (!ok)
    {
        EXPECT_TRUE(true);
        return;
    }

    // Vulkan 1.4 availability depends on driver — either result is valid.
    // We verify the flags are consistent and accessors work.
    bool isVk14 = device.IsVulkan14();
    bool hasPush = device.SupportsPushDescriptors();
    bool hasHIC = device.SupportsHostImageCopy();

    // Push descriptors and host image copy require Vulkan 1.4 or the extension
    // If Vulkan 1.4 is not available, push descriptors may still be available
    // via VK_KHR_push_descriptor extension
    if (!isVk14)
    {
        // Host image copy requires 1.4 — must be false without it
        EXPECT_TRUE(!hasHIC);
    }

    // Device info should mention Vulkan 1.4 status
    std::string info = device.GetDeviceInfo();
    EXPECT_TRUE(info.find("Vulkan 1.4") != std::string::npos);
    EXPECT_TRUE(info.find("Push Descriptors") != std::string::npos);
    EXPECT_TRUE(info.find("Host Image Copy") != std::string::npos);

    // API version string should be populated
    auto& caps = device.GetCapabilities();
    EXPECT_TRUE(caps.apiVersion.find("Vulkan") != std::string::npos);

    device.Shutdown();
#endif
}

// ============================================================================
// Resource creation on Vulkan (including Lavapipe)
// ============================================================================

TEST(VulkanLavapipe_BufferCreation)
{
#ifndef SPARK_VULKAN_SUPPORT
    EXPECT_TRUE(true);
    return;
#else
    Spark::RHI::Vulkan::VulkanDevice device;
    Spark::RHI::RHIDeviceDesc desc;
    desc.enableDebugLayer = false;
    desc.applicationName = "LavapipeBufferTest";

    bool ok = device.Initialize(desc);
    if (!ok)
    {
        EXPECT_TRUE(true);
        return;
    }

    // Create a simple vertex buffer
    Spark::RHI::RHIBufferDesc bufDesc;
    bufDesc.size = 1024;
    bufDesc.stride = 32;
    bufDesc.usage = Spark::RHI::RHIBufferUsage::Vertex;
    bufDesc.access = Spark::RHI::RHIBufferAccess::Static;
    bufDesc.debugName = "TestVertexBuffer";

    auto buffer = device.CreateBuffer(bufDesc);
    EXPECT_TRUE(buffer != nullptr);
    if (buffer)
    {
        EXPECT_TRUE(buffer->IsValid());
        EXPECT_TRUE(buffer->GetSize() == 1024);
    }

    // Destroy buffer BEFORE shutting down the device — the buffer's destructor
    // calls vkDestroyBuffer() which requires the VkDevice to still be alive.
    buffer.reset();

    device.Shutdown();
#endif
}

// ============================================================================
// Factory fallback: Vulkan backend creation through RHIFactory
// ============================================================================

TEST(VulkanLavapipe_FactoryCreate)
{
#ifndef SPARK_VULKAN_SUPPORT
    EXPECT_TRUE(true);
    return;
#else
    bool available = Spark::RHI::IsBackendAvailable(Spark::RHI::GraphicsBackend::Vulkan);
    if (!available)
    {
        EXPECT_TRUE(true);
        return;
    }

    auto device = Spark::RHI::CreateDevice(Spark::RHI::GraphicsBackend::Vulkan);
    EXPECT_TRUE(device != nullptr);

    if (device)
    {
        Spark::RHI::RHIDeviceDesc desc;
        desc.enableDebugLayer = false;
        desc.applicationName = "LavapipeFactoryTest";

        bool ok = device->Initialize(desc);
        EXPECT_TRUE(ok);

        if (ok)
        {
            EXPECT_TRUE(device->GetBackendType() == Spark::RHI::GraphicsBackend::Vulkan);
            device->Shutdown();
        }
    }
#endif
}
