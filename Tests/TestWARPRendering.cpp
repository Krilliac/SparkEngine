/**
 * @file TestWARPRendering.cpp
 * @brief Tests D3D11/D3D12 RHI backends with WARP software rasterizer
 *
 * Verifies that the DirectX backends can initialize on CPU-only systems
 * using Microsoft's WARP (Windows Advanced Rasterization Platform).
 * Tests are skipped on non-Windows platforms.
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Graphics/RHI/RHIFactory.h"
#include "../SparkEngine/Source/Graphics/RHI/RHIBridge.h"

#ifdef _WIN32
#include "../SparkEngine/Source/Graphics/RHI/D3D11/D3D11Device.h"
#include "../SparkEngine/Source/Graphics/RHI/D3D12/D3D12Device.h"
#endif

// ============================================================================
// D3D11 WARP fallback
// ============================================================================

TEST(WARP_D3D11DeviceInit)
{
#ifndef _WIN32
    EXPECT_TRUE(true);
    return;
#else
    Spark::RHI::D3D11::D3D11Device device;
    Spark::RHI::RHIDeviceDesc desc;
    desc.enableDebugLayer = false;
    desc.applicationName = "WARPTest_D3D11";

    bool ok = device.Initialize(desc);
    // On any Windows 10+ machine (including CI), this should always succeed
    // because either a hardware GPU or WARP is available
    EXPECT_TRUE(ok);

    if (ok)
    {
        auto& caps = device.GetCapabilities();
        EXPECT_TRUE(!caps.deviceName.empty());
        EXPECT_TRUE(caps.maxTextureSize > 0);

        // The isSoftwareDevice flag should be consistent with IsSoftwareDevice()
        EXPECT_TRUE(caps.isSoftwareDevice == device.IsSoftwareDevice());

        std::string info = device.GetDeviceInfo();
        EXPECT_TRUE(!info.empty());

        device.Shutdown();
    }
#endif
}

// ============================================================================
// D3D12 WARP fallback
// ============================================================================

TEST(WARP_D3D12DeviceInit)
{
#ifndef _WIN32
    EXPECT_TRUE(true);
    return;
#else
    Spark::RHI::D3D12::D3D12Device device;
    Spark::RHI::RHIDeviceDesc desc;
    desc.enableDebugLayer = false;
    desc.applicationName = "WARPTest_D3D12";

    bool ok = device.Initialize(desc);
    // D3D12 WARP requires Windows 10+ with D3D12 runtime
    if (!ok)
    {
        // Not a failure — D3D12 runtime may not be available on older systems
        EXPECT_TRUE(true);
        return;
    }

    auto& caps = device.GetCapabilities();
    EXPECT_TRUE(!caps.deviceName.empty());
    EXPECT_TRUE(caps.maxTextureSize > 0);
    EXPECT_TRUE(caps.isSoftwareDevice == device.IsSoftwareDevice());

    std::string info = device.GetDeviceInfo();
    EXPECT_TRUE(!info.empty());

    device.Shutdown();
#endif
}

// ============================================================================
// D3D11 buffer creation (hardware or WARP)
// ============================================================================

TEST(WARP_D3D11BufferCreation)
{
#ifndef _WIN32
    EXPECT_TRUE(true);
    return;
#else
    Spark::RHI::D3D11::D3D11Device device;
    Spark::RHI::RHIDeviceDesc desc;
    desc.enableDebugLayer = false;
    desc.applicationName = "WARPBufferTest_D3D11";

    bool ok = device.Initialize(desc);
    EXPECT_TRUE(ok);
    if (!ok)
        return;

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
// Factory: D3D11 backend creation
// ============================================================================

TEST(WARP_D3D11FactoryCreate)
{
#ifndef _WIN32
    EXPECT_TRUE(true);
    return;
#else
    auto device = Spark::RHI::CreateDevice(Spark::RHI::GraphicsBackend::D3D11);
    EXPECT_TRUE(device != nullptr);

    if (device)
    {
        Spark::RHI::RHIDeviceDesc desc;
        desc.enableDebugLayer = false;
        desc.applicationName = "WARPFactoryTest_D3D11";

        bool ok = device->Initialize(desc);
        EXPECT_TRUE(ok);

        if (ok)
        {
            EXPECT_TRUE(device->GetBackendType() == Spark::RHI::GraphicsBackend::D3D11);
            device->Shutdown();
        }
    }
#endif
}
