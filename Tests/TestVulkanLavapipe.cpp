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
    bufDesc.cpuAccess = Spark::RHI::RHICPUAccess::None;
    bufDesc.debugName = "TestVertexBuffer";

    auto buffer = device.CreateBuffer(bufDesc);
    EXPECT_TRUE(buffer != nullptr);
    if (buffer)
    {
        EXPECT_TRUE(buffer->IsValid());
        EXPECT_TRUE(buffer->GetSize() == 1024);
    }

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
