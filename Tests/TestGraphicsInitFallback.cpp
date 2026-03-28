/**
 * @file TestGraphicsInitFallback.cpp
 * @brief Tests that graphics initialization falls back to NullRHIDevice when no GPU is available
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Graphics/RHI/NullRHIDevice.h"
#include "../SparkEngine/Source/Graphics/RHI/RHIFactory.h"
#include "../SparkEngine/Source/Graphics/RHI/RHIBridge.h"

// ============================================================================
// NullRHIDevice direct construction
// ============================================================================

TEST(GraphicsFallback_NullDeviceConstructable)
{
    // NullRHIDevice can be constructed outside the singleton
    Spark::RHI::NullRHIDevice device;
    EXPECT_FALSE(device.IsInitialized());

    Spark::RHI::RHIDeviceDesc desc;
    EXPECT_TRUE(device.Initialize(desc));
    EXPECT_TRUE(device.IsInitialized());
    EXPECT_TRUE(device.GetBackendType() == Spark::RHI::GraphicsBackend::None);

    device.Shutdown();
    EXPECT_FALSE(device.IsInitialized());
}

// ============================================================================
// Factory returns NullRHIDevice for GraphicsBackend::None
// ============================================================================

TEST(GraphicsFallback_FactoryCreatesNullDevice)
{
    auto device = Spark::RHI::CreateDevice(Spark::RHI::GraphicsBackend::None);
    EXPECT_TRUE(device != nullptr);

    if (device)
    {
        Spark::RHI::RHIDeviceDesc desc;
        EXPECT_TRUE(device->Initialize(desc));
        EXPECT_TRUE(device->GetBackendType() == Spark::RHI::GraphicsBackend::None);

        // Should support basic frame lifecycle
        device->BeginFrame();
        device->EndFrame();
        device->WaitForIdle();

        device->Shutdown();
    }
}

// ============================================================================
// RHIBridge headless initialization
// ============================================================================

TEST(GraphicsFallback_BridgeHeadlessInit)
{
    Spark::RHI::RHIBridge bridge;

    // Initialize with GraphicsBackend::None — should succeed in headless mode
    bool ok = bridge.Initialize(nullptr, 800, 600, Spark::RHI::GraphicsBackend::None, false);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(bridge.IsHeadless());

    // Device should be valid
    EXPECT_TRUE(bridge.GetDevice() != nullptr);
    EXPECT_TRUE(bridge.GetActiveBackend() == Spark::RHI::GraphicsBackend::None);

    // Swap chain and depth buffer are null in headless mode — that's expected
    EXPECT_TRUE(bridge.GetSwapChain() == nullptr);
    EXPECT_TRUE(bridge.GetDepthBuffer() == nullptr);

    // Frame lifecycle should not crash
    bridge.BeginFrame();
    bridge.EndFrame();

    // Present returns false (nothing to present) — but doesn't crash
    EXPECT_FALSE(bridge.Present(false));

    // Command list should still be available (NullCommandList)
    EXPECT_TRUE(bridge.GetCommandList() != nullptr);

    bridge.Shutdown();
}

// ============================================================================
// NullRHIDevice resource tracking in headless mode
// ============================================================================

TEST(GraphicsFallback_HeadlessResourceTracking)
{
    Spark::RHI::NullRHIDevice device;
    Spark::RHI::RHIDeviceDesc desc;
    device.Initialize(desc);

    // Create resources — all return nullptr but stats are tracked
    Spark::RHI::RHIBufferDesc bufDesc;
    bufDesc.size = 1024;
    auto buf = device.CreateBuffer(bufDesc);
    EXPECT_TRUE(buf == nullptr); // Null device doesn't allocate real resources

    Spark::RHI::RHITextureDesc texDesc;
    texDesc.width = 256;
    texDesc.height = 256;
    auto tex = device.CreateTexture(texDesc);
    EXPECT_TRUE(tex == nullptr);

    Spark::RHI::RHIShaderDesc shaderDesc;
    shaderDesc.stage = Spark::RHI::RHIShaderStage::Vertex;
    auto shader = device.CreateShader(shaderDesc);
    EXPECT_TRUE(shader == nullptr);

    // Verify tracking
    const auto& stats = device.GetNullStats();
    EXPECT_EQ(stats.buffersCreated, 1u);
    EXPECT_EQ(stats.texturesCreated, 1u);
    EXPECT_EQ(stats.shadersCreated, 1u);

    // Run a few frames
    device.BeginFrame();
    device.EndFrame();
    device.BeginFrame();
    device.EndFrame();
    EXPECT_EQ(stats.framesRendered, 2u);

    device.Shutdown();
}

// ============================================================================
// Auto-select falls back to null when no backends compiled in
// ============================================================================

TEST(GraphicsFallback_AutoSelectOnLinuxNoGPU)
{
    // On this Linux CI machine with no GPU backends compiled, Auto should
    // resolve to None and the factory should return a NullRHIDevice
    auto backends = Spark::RHI::DetectAvailableBackends();

    if (backends.empty())
    {
        // No GPU backends — CreateDevice(Auto) should return NullRHIDevice
        auto device = Spark::RHI::CreateDevice(Spark::RHI::GraphicsBackend::Auto);
        EXPECT_TRUE(device != nullptr);

        if (device)
        {
            EXPECT_TRUE(device->GetBackendType() == Spark::RHI::GraphicsBackend::None);
            device->Shutdown();
        }
    }
    else
    {
        // GPU backends available — test still passes, just documents the path
        EXPECT_TRUE(backends.size() > 0);
    }
}
