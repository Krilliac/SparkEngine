/**
 * @file TestRHIBridgeIntegration.cpp
 * @brief Integration tests for RHIBridge lifecycle, backend fallback, and resource creation
 *
 * Tests the RHI bridge layer that connects the engine to GPU backends.
 * All tests run against the NullRHIDevice (headless) so no GPU is required.
 */

#include "TestFramework.h"
#include "Graphics/RHI/RHIBridge.h"
#include "Graphics/RHI/RHIFactory.h"
#include "Graphics/RHI/RHITypes.h"

using namespace Spark::RHI;

// ============================================================================
// Lifecycle Tests
// ============================================================================

TEST(RHIBridge_InitializeWithNoneBackend_CreatesHeadlessDevice)
{
    RHIBridge bridge;
    bool ok = bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(bridge.IsHeadless());
    EXPECT_TRUE(bridge.GetDevice() != nullptr);
    EXPECT_EQ(static_cast<int>(bridge.GetActiveBackend()), static_cast<int>(GraphicsBackend::None));
    bridge.Shutdown();
}

TEST(RHIBridge_ShutdownWithoutInit_DoesNotCrash)
{
    RHIBridge bridge;
    bridge.Shutdown(); // Should be safe to call on uninitialized bridge
}

TEST(RHIBridge_DoubleInitialize_ReinitializesCleanly)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 800, 600, GraphicsBackend::None, false));
    EXPECT_TRUE(bridge.IsHeadless());

    // Second init should shut down first, then reinitialize
    EXPECT_TRUE(bridge.Initialize(nullptr, 1024, 768, GraphicsBackend::None, false));
    EXPECT_TRUE(bridge.IsHeadless());
    bridge.Shutdown();
}

TEST(RHIBridge_DoubleShutdown_DoesNotCrash)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 640, 480, GraphicsBackend::None, false));
    bridge.Shutdown();
    bridge.Shutdown(); // Second shutdown should be harmless
}

// ============================================================================
// Frame Management Tests
// ============================================================================

TEST(RHIBridge_BeginEndFrame_Headless_DoesNotCrash)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    for (int i = 0; i < 10; ++i)
    {
        bridge.BeginFrame();
        bridge.EndFrame();
    }

    bridge.Shutdown();
}

TEST(RHIBridge_Present_Headless_ReturnsFalse)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    // Headless has no swap chain, so Present returns false
    EXPECT_FALSE(bridge.Present(true));

    bridge.Shutdown();
}

TEST(RHIBridge_GetCommandList_Headless_ReturnsNonNull)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    auto* cmd = bridge.GetCommandList();
    EXPECT_TRUE(cmd != nullptr);

    bridge.Shutdown();
}

// ============================================================================
// Backend Fallback Tests
// ============================================================================

TEST(RHIBridge_FallbackToNullWhenNoGPU_Succeeds)
{
    // Requesting Auto backend with no window should eventually fall back
    // to NullRHIDevice if no GPU backend can initialize
    RHIBridge bridge;
    bool ok = bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::Auto, false);
    EXPECT_TRUE(ok);
    // The device should be valid regardless of which backend was chosen
    EXPECT_TRUE(bridge.GetDevice() != nullptr);
    bridge.Shutdown();
}

TEST(RHIBridge_GetAvailableBackends_DoesNotCrash)
{
    auto backends = RHIBridge::GetAvailableBackends();
    // May be empty if no GPU packages installed (CI headless environment)
    // Just verify the call doesn't crash — use the vector to suppress
    // -Wtype-limits on the always-true size()>=0 comparison.
    (void)backends;
    EXPECT_TRUE(true);
}

TEST(RHIBridge_GetRecommendedBackend_DoesNotCrash)
{
    auto backend = RHIBridge::GetRecommendedBackend();
    // In headless CI, may return Auto if no backends compiled in
    // Just verify no crash and a valid enum value
    EXPECT_TRUE(static_cast<int>(backend) >= 0);
}

// ============================================================================
// Resource Creation Tests (Headless)
// ============================================================================

TEST(RHIBridge_CreateVertexBuffer_Headless_ReturnsNonNull)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    float vertices[] = {0.0f, 0.5f, 0.0f, -0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f};
    auto buffer = bridge.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(float) * 3);
    EXPECT_TRUE(buffer != nullptr);

    bridge.Shutdown();
}

TEST(RHIBridge_CreateIndexBuffer_Headless_ReturnsNonNull)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    uint32_t indices[] = {0, 1, 2};
    auto buffer = bridge.CreateIndexBuffer(indices, sizeof(indices), sizeof(uint32_t));
    EXPECT_TRUE(buffer != nullptr);

    bridge.Shutdown();
}

TEST(RHIBridge_CreateConstantBuffer_Headless_ReturnsNonNull)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    auto buffer = bridge.CreateConstantBuffer(256);
    EXPECT_TRUE(buffer != nullptr);

    bridge.Shutdown();
}

TEST(RHIBridge_CreateTexture2D_Headless_ReturnsNonNull)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    auto texture = bridge.CreateTexture2D(256, 256, PixelFormat::R8G8B8A8_UNORM, RHITextureUsage::ShaderResource);
    EXPECT_TRUE(texture != nullptr);

    bridge.Shutdown();
}

// ============================================================================
// Capabilities & Info Tests
// ============================================================================

TEST(RHIBridge_GetBackendName_Headless_ReturnsUnknownOrNone)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    auto name = bridge.GetBackendName();
    EXPECT_TRUE(!name.empty());

    bridge.Shutdown();
}

TEST(RHIBridge_GetDeviceInfo_Headless_ReturnsNonEmpty)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    auto info = bridge.GetDeviceInfo();
    EXPECT_TRUE(!info.empty());

    bridge.Shutdown();
}

// ============================================================================
// Shader Cache Tests
// ============================================================================

TEST(RHIBridge_RegisterShader_DoesNotCrash)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    bridge.RegisterShader("TestVS", RHIShaderStage::Vertex, "Shaders/HLSL/BasicVS.hlsl", "Shaders/GLSL/BasicVS.glsl",
                          "", "main");

    // Getting a shader for a non-existent file returns null (no crash)
    auto* shader = bridge.GetShader("TestVS");
    // May or may not find the file — we're testing it doesn't crash
    (void)shader;

    bridge.Shutdown();
}

TEST(RHIBridge_GetShader_UnregisteredName_ReturnsNull)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    auto* shader = bridge.GetShader("NonexistentShader");
    EXPECT_TRUE(shader == nullptr);

    bridge.Shutdown();
}
