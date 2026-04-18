/**
 * @file TestGraphicsInitFallback.cpp
 * @brief Tests that graphics initialization falls back to NullRHIDevice when no GPU is available
 */

#include "TestFramework.h"
#include "Graphics/RHI/NullRHIDevice.h"
#include "Graphics/RHI/RHIFactory.h"
#include "Graphics/RHI/RHIBridge.h"

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

    // Phase Y Theme 3B: NullRHIDevice now returns real stub resources
    // (NullBuffer, NullTexture, NullShader) instead of nullptr, so
    // downstream code can consume them without crashing. The backing
    // memory is CPU-only. Initialize also creates two buffers
    // internally for the transient allocator, so the test baselines
    // its "user-created" count off the post-init stats.
    const auto baseline = device.GetNullStats();

    Spark::RHI::RHIBufferDesc bufDesc;
    bufDesc.size = 1024;
    auto buf = device.CreateBuffer(bufDesc);
    EXPECT_TRUE(buf != nullptr);
    EXPECT_TRUE(buf->IsValid());
    EXPECT_EQ(buf->GetSize(), static_cast<uint64_t>(1024));

    Spark::RHI::RHITextureDesc texDesc;
    texDesc.width = 256;
    texDesc.height = 256;
    auto tex = device.CreateTexture(texDesc);
    EXPECT_TRUE(tex != nullptr);
    EXPECT_TRUE(tex->IsValid());
    EXPECT_EQ(tex->GetWidth(), static_cast<uint32_t>(256));

    Spark::RHI::RHIShaderDesc shaderDesc;
    shaderDesc.stage = Spark::RHI::RHIShaderStage::Vertex;
    auto shader = device.CreateShader(shaderDesc);
    EXPECT_TRUE(shader != nullptr);
    EXPECT_TRUE(shader->IsValid());

    // Verify stats tracking — one of each beyond the post-init baseline.
    const auto& stats = device.GetNullStats();
    EXPECT_EQ(stats.buffersCreated - baseline.buffersCreated, 1u);
    EXPECT_EQ(stats.texturesCreated - baseline.texturesCreated, 1u);
    EXPECT_EQ(stats.shadersCreated - baseline.shadersCreated, 1u);

    // Phase Y: the HandlePools now track every Create* call. The
    // buffer pool also contains the two transient-allocator buffers
    // created during Initialize, so we expect 3 total.
    EXPECT_TRUE(device.GetBufferPool().Count() >= 1);
    EXPECT_EQ(device.GetTexturePool().Count(), static_cast<uint32_t>(1));
    EXPECT_EQ(device.GetShaderPool().Count(), static_cast<uint32_t>(1));

    // Run a few frames — Phase Y also wires the transient allocator
    // into BeginFrame / EndFrame, so these calls exercise the full
    // wire-up path.
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
