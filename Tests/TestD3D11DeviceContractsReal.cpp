/**
 * @file TestD3D11DeviceContractsReal.cpp
 * @brief Production-linked contract tests for the D3D11 RHI backend and RHIBridge.
 *
 * Covers the release-readiness fixes in this area:
 *   - RHIBridge refuses to turn a failed windowed GPU request into a silent
 *     headless "success" (gfx-rhi-23).
 *   - ShaderCache reloads without freeing shaders a consumer pinned (gfx-rhi-22).
 *   - D3D11Device::CreateSwapChain returns nullptr instead of a dead swap chain
 *     (gfx-rhi-05).
 *   - Deferred command lists own their context and actually execute (gfx-rhi-06).
 *   - Sampled depth textures and structured/indirect buffers are created with the
 *     formats and flags D3D11 requires (gfx-rhi-20, gfx-rhi-18).
 *   - The device floor is feature level 11_0 (gfx-rhi-19).
 *   - GraphicsEngine remembers the window handle device-lost recovery needs
 *     (gfx-rhi-02).
 *
 * Every test that needs a GPU skips cleanly when no D3D11 device can be created.
 */

#include "TestFramework.h"

#include "Graphics/RHI/RHIBridge.h"
#include "Graphics/RHI/RHITypes.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#ifdef _WIN32
#include "Graphics/GraphicsEngine.h"
#include "Graphics/RHI/D3D11/D3D11Device.h"
#include <filesystem>
#include <fstream>
#include <windows.h>
#endif

namespace
{
    /// Temporarily disables every GPU backend through the documented env-var
    /// escape hatches, so RHIBridge::Initialize takes its "no GPU backend
    /// worked" path without depending on the host's drivers.
    class ScopedNoGPUBackends
    {
      public:
        ScopedNoGPUBackends() { Set("1"); }
        ~ScopedNoGPUBackends() { Set("0"); }

        ScopedNoGPUBackends(const ScopedNoGPUBackends&) = delete;
        ScopedNoGPUBackends& operator=(const ScopedNoGPUBackends&) = delete;

      private:
        static void Set(const char* value)
        {
#ifdef _WIN32
            _putenv_s("SPARK_DISABLE_D3D11", value);
            _putenv_s("SPARK_DISABLE_VULKAN", value);
            _putenv_s("SPARK_DISABLE_OPENGL", value);
#else
            setenv("SPARK_DISABLE_D3D11", value, 1);
            setenv("SPARK_DISABLE_VULKAN", value, 1);
            setenv("SPARK_DISABLE_OPENGL", value, 1);
#endif
        }
    };

    /// A non-null handle that is never a real window — enough to make
    /// RHIBridge treat the request as windowed.
    void* FakeWindowHandle()
    {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000));
    }
} // namespace

// ============================================================================
// RHIBridge: no silent headless fallback for a windowed request (gfx-rhi-23)
// ============================================================================

TEST(RHIBridgeReal_WindowedRequestWithNoGPUBackendFails)
{
    ScopedNoGPUBackends noGPU;

    Spark::RHI::RHIBridge bridge;
    const bool ok = bridge.Initialize(FakeWindowHandle(), 640, 480, Spark::RHI::GraphicsBackend::D3D11, false);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(bridge.IsHeadless());
    EXPECT_TRUE(bridge.GetDevice() == nullptr);
}

TEST(RHIBridgeReal_WindowedRequestCanOptIntoHeadlessFallback)
{
    ScopedNoGPUBackends noGPU;

    Spark::RHI::RHIBridge bridge;
    const bool ok = bridge.Initialize(FakeWindowHandle(), 640, 480, Spark::RHI::GraphicsBackend::D3D11, false, true);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(bridge.IsHeadless());
    bridge.Shutdown();
}

TEST(RHIBridgeReal_HeadlessRequestStillSucceeds)
{
    ScopedNoGPUBackends noGPU;

    Spark::RHI::RHIBridge bridge;
    // No window handle: the caller asked for headless, so NullRHI is the answer,
    // not a failure.
    EXPECT_TRUE(bridge.Initialize(nullptr, 640, 480, Spark::RHI::GraphicsBackend::None, false));
    EXPECT_TRUE(bridge.IsHeadless());
    bridge.Shutdown();
}

#ifdef _WIN32

namespace
{
    /// Creates a real (hardware or WARP) D3D11 RHI device, or reports why not.
    bool TryCreateD3D11Device(Spark::RHI::D3D11::D3D11Device& device)
    {
        Spark::RHI::RHIDeviceDesc desc;
        desc.preferredBackend = Spark::RHI::GraphicsBackend::D3D11;
        desc.enableDebugLayer = false;
        desc.enableGPUValidation = false;
        desc.applicationName = "SparkTests";
        return device.Initialize(desc);
    }

    std::filesystem::path WriteTempVertexShader()
    {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() / "SparkTests_D3D11ContractsReal_VS.hlsl";
        std::ofstream out(path, std::ios::trunc);
        out << "float4 main(float3 pos : POSITION) : SV_Position\n"
            << "{\n"
            << "    return float4(pos, 1.0f);\n"
            << "}\n";
        return path;
    }
} // namespace

// ============================================================================
// D3D11Device: swap chain / command list / resource contracts
// ============================================================================

TEST(D3D11DeviceReal_CreateSwapChainRejectsInvalidWindow)
{
    Spark::RHI::D3D11::D3D11Device device;
    if (!TryCreateD3D11Device(device))
        SKIP_TEST("No D3D11 device available (hardware or WARP)");

    Spark::RHI::RHISwapChainDesc desc;
    desc.windowHandle = nullptr; // DXGI cannot create a swap chain for this
    desc.width = 320;
    desc.height = 240;
    desc.bufferCount = 2;

    // A live object here would let RHIBridge report success and crash on Present().
    EXPECT_TRUE(device.CreateSwapChain(desc) == nullptr);
    device.Shutdown();
}

TEST(D3D11DeviceReal_DeferredCommandListOwnsContextAndExecutes)
{
    Spark::RHI::D3D11::D3D11Device device;
    if (!TryCreateD3D11Device(device))
        SKIP_TEST("No D3D11 device available (hardware or WARP)");

    auto list = device.CreateDeferredCommandList();
    ASSERT_TRUE(list != nullptr);

    auto* deferred = static_cast<Spark::RHI::D3D11::D3D11CommandList*>(list.get());
    EXPECT_FALSE(deferred->IsImmediate());

    // Recording touches the deferred context; at HEAD it had already been released.
    deferred->Begin();
    Spark::RHI::RHIViewport viewport;
    viewport.width = 320.0f;
    viewport.height = 240.0f;
    deferred->SetViewport(viewport);
    deferred->End();

    // End() must finish the context into a replayable command list...
    EXPECT_TRUE(deferred->GetRecordedCommandList() != nullptr);

    // ...and ExecuteCommandList must actually submit and consume it.
    device.ExecuteCommandList(list.get());
    EXPECT_TRUE(deferred->GetRecordedCommandList() == nullptr);

    list.reset();
    device.Shutdown();
}

TEST(D3D11DeviceReal_ExecuteCommandListPreservesImmediateContextState)
{
    Spark::RHI::D3D11::D3D11Device device;
    if (!TryCreateD3D11Device(device))
        SKIP_TEST("No D3D11 device available (hardware or WARP)");

    ID3D11DeviceContext1* immediate = device.GetD3D11Context();
    ASSERT_TRUE(immediate != nullptr);

    // The Windows renderer keeps its state bound on the immediate context for the
    // whole frame. Replaying a deferred list with RestoreContextState = FALSE wipes
    // that state, so every draw after a mid-frame execute would silently lose its
    // viewport, shaders and render targets.
    D3D11_VIEWPORT frameViewport = {};
    frameViewport.TopLeftX = 0.0f;
    frameViewport.TopLeftY = 0.0f;
    frameViewport.Width = 640.0f;
    frameViewport.Height = 480.0f;
    frameViewport.MinDepth = 0.0f;
    frameViewport.MaxDepth = 1.0f;
    immediate->RSSetViewports(1, &frameViewport);

    auto list = device.CreateDeferredCommandList();
    ASSERT_TRUE(list != nullptr);

    auto* deferred = static_cast<Spark::RHI::D3D11::D3D11CommandList*>(list.get());
    deferred->Begin();
    Spark::RHI::RHIViewport deferredViewport;
    deferredViewport.width = 320.0f;
    deferredViewport.height = 240.0f;
    deferred->SetViewport(deferredViewport);
    deferred->End();

    device.ExecuteCommandList(list.get());

    UINT viewportCount = 1;
    D3D11_VIEWPORT afterExecute = {};
    immediate->RSGetViewports(&viewportCount, &afterExecute);

    // FALSE here returns zero viewports (the context was cleared); TRUE restores
    // the caller's 640x480 frame viewport.
    EXPECT_EQ(viewportCount, 1u);
    EXPECT_NEAR(afterExecute.Width, 640.0f, 0.5f);
    EXPECT_NEAR(afterExecute.Height, 480.0f, 0.5f);

    list.reset();
    device.Shutdown();
}

TEST(D3D11DeviceReal_SampledDepthTextureGetsTypelessResourceAndDepthSRV)
{
    Spark::RHI::D3D11::D3D11Device device;
    if (!TryCreateD3D11Device(device))
        SKIP_TEST("No D3D11 device available (hardware or WARP)");

    Spark::RHI::RHITextureDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.format = Spark::RHI::PixelFormat::D24_UNORM_S8_UINT;
    desc.usage = Spark::RHI::RHITextureUsage::DepthStencil | Spark::RHI::RHITextureUsage::ShaderResource;
    desc.debugName = "ContractTest_Depth";

    auto texture = device.CreateTexture(desc);
    ASSERT_TRUE(texture != nullptr); // At HEAD the SRV creation failed and this was nullptr
    EXPECT_TRUE(texture->GetShaderResourceView() != nullptr);
    EXPECT_TRUE(texture->GetDepthStencilView() != nullptr);

    texture.reset();
    device.Shutdown();
}

TEST(D3D11DeviceReal_RejectsUnimplementedTextureTypes)
{
    Spark::RHI::D3D11::D3D11Device device;
    if (!TryCreateD3D11Device(device))
        SKIP_TEST("No D3D11 device available (hardware or WARP)");

    Spark::RHI::RHITextureDesc desc;
    desc.width = 8;
    desc.height = 8;
    desc.depth = 8;
    desc.type = Spark::RHI::RHITextureType::Texture3D;
    desc.debugName = "ContractTest_Volume";

    // The backend only implements Texture2D — say so instead of silently
    // creating a 2D texture with the wrong contents.
    EXPECT_TRUE(device.CreateTexture(desc) == nullptr);
    device.Shutdown();
}

TEST(D3D11DeviceReal_StructuredAndIndirectBuffersCarryTheRequiredFlags)
{
    Spark::RHI::D3D11::D3D11Device device;
    if (!TryCreateD3D11Device(device))
        SKIP_TEST("No D3D11 device available (hardware or WARP)");

    Spark::RHI::RHIBufferDesc structuredDesc;
    structuredDesc.size = 16 * sizeof(float) * 4;
    structuredDesc.stride = sizeof(float) * 4;
    structuredDesc.usage = Spark::RHI::RHIBufferUsage::Structured | Spark::RHI::RHIBufferUsage::Storage;
    structuredDesc.access = Spark::RHI::RHIBufferAccess::Static;
    structuredDesc.debugName = "ContractTest_Structured";

    auto structured = device.CreateBuffer(structuredDesc);
    ASSERT_TRUE(structured != nullptr);
    auto* d3dStructured = static_cast<Spark::RHI::D3D11::D3D11Buffer*>(structured.get());
    EXPECT_TRUE(d3dStructured->GetD3D11SRV() != nullptr);
    EXPECT_TRUE(d3dStructured->GetD3D11UAV() != nullptr);

    // An indirect-args buffer must be creatable; without
    // D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS the runtime drops every
    // DrawInstancedIndirect issued against it.
    Spark::RHI::RHIBufferDesc argsDesc;
    argsDesc.size = 4 * sizeof(uint32_t);
    argsDesc.stride = sizeof(uint32_t);
    argsDesc.usage = Spark::RHI::RHIBufferUsage::IndirectArgs;
    argsDesc.access = Spark::RHI::RHIBufferAccess::Static;
    argsDesc.debugName = "ContractTest_IndirectArgs";

    auto args = device.CreateBuffer(argsDesc);
    EXPECT_TRUE(args != nullptr);

    // A structured buffer without a stride cannot be expressed in D3D11.
    Spark::RHI::RHIBufferDesc badDesc;
    badDesc.size = 256;
    badDesc.stride = 0;
    badDesc.usage = Spark::RHI::RHIBufferUsage::Structured;
    badDesc.access = Spark::RHI::RHIBufferAccess::Static;
    badDesc.debugName = "ContractTest_StructuredNoStride";
    EXPECT_TRUE(device.CreateBuffer(badDesc) == nullptr);

    structured.reset();
    args.reset();
    device.Shutdown();
}

TEST(D3D11DeviceReal_DeviceFloorIsFeatureLevel11)
{
    Spark::RHI::D3D11::D3D11Device device;
    if (!TryCreateD3D11Device(device))
        SKIP_TEST("No D3D11 device available (hardware or WARP)");

    const auto& caps = device.GetCapabilities();
    // Every shader the engine compiles targets SM 5.0, which needs FL 11_0+.
    EXPECT_TRUE(caps.computeShaderSupport);
    EXPECT_TRUE(caps.tessellationSupport);
    EXPECT_TRUE(caps.apiVersion == "DirectX 11.1" || caps.apiVersion == "DirectX 11.0");
    device.Shutdown();
}

// ============================================================================
// ShaderCache: a reload must not free a shader someone still holds (gfx-rhi-22)
// ============================================================================

TEST(ShaderCacheReal_PinnedShaderSurvivesReloadAll)
{
    Spark::RHI::D3D11::D3D11Device device;
    if (!TryCreateD3D11Device(device))
        SKIP_TEST("No D3D11 device available (hardware or WARP)");

    const std::filesystem::path shaderPath = WriteTempVertexShader();

    Spark::RHI::ShaderCache cache;
    Spark::RHI::ShaderCache::ShaderEntry entry;
    entry.hlslPath = shaderPath.string();
    entry.entryPoint = "main";
    entry.stage = Spark::RHI::RHIShaderStage::Vertex;
    cache.RegisterShader("ContractTest_VS", entry);

    std::shared_ptr<Spark::RHI::IRHIShader> pinned = cache.GetShaderShared("ContractTest_VS", &device);
    ASSERT_TRUE(pinned != nullptr);
    EXPECT_TRUE(pinned->IsValid());
    EXPECT_TRUE(cache.GetShader("ContractTest_VS", &device) == pinned.get());

    cache.ReloadAll(&device);

    // The pinned shader is still a live object (at HEAD ReloadAll destroyed it
    // while pipeline states still pointed at it).
    EXPECT_TRUE(pinned->IsValid());

    Spark::RHI::IRHIShader* reloaded = cache.GetShader("ContractTest_VS", &device);
    ASSERT_TRUE(reloaded != nullptr);
    EXPECT_TRUE(reloaded != pinned.get());

    cache.Clear(&device);
    pinned.reset();
    device.Shutdown();

    std::error_code ec;
    std::filesystem::remove(shaderPath, ec);
}

// ============================================================================
// GraphicsEngine: device-lost recovery needs the window handle (gfx-rhi-02)
// ============================================================================

TEST(GraphicsEngineReal_InitializeStoresWindowHandleForDeviceLostRecovery)
{
    {
        // Cheap probe first — no point creating a window if there is no D3D11.
        Spark::RHI::D3D11::D3D11Device probe;
        if (!TryCreateD3D11Device(probe))
            SKIP_TEST("No D3D11 device available (hardware or WARP)");
        probe.Shutdown();
    }

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"SparkTestsGraphicsEngine", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd)
        SKIP_TEST("Could not create a test window");

    {
        GraphicsEngine engine;
        const HRESULT hr = engine.Initialize(hwnd);
        if (FAILED(hr))
        {
            DestroyWindow(hwnd);
            SKIP_TEST("GraphicsEngine::Initialize failed in this environment");
        }

        // RecoverFromDeviceLost() recreates the swap chain from this handle; at
        // HEAD it was never assigned, so recovery always failed.
        EXPECT_TRUE(engine.GetWindowHandle() == static_cast<Spark::NativeWindowHandle>(hwnd));
        EXPECT_TRUE(engine.GetDevice() != nullptr);
        engine.Shutdown();
    }

    DestroyWindow(hwnd);
}

#endif // _WIN32
