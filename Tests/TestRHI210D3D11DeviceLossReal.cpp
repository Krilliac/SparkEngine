/**
 * @file TestRHI210D3D11DeviceLossReal.cpp
 * @brief RHI-210: D3D11 device loss, resize and resource stress recover.
 *
 * gfx_reset_device (Console_ResetDevice) used to re-run ResizeBuffers and call
 * that a device reset; nothing ever exercised the real recovery path EndFrame
 * takes on DXGI_ERROR_DEVICE_REMOVED/RESET. The console now injects
 * DXGI_ERROR_DEVICE_RESET into that same HandleDeviceLost path. A real driver
 * TDR cannot be forced from inside the process, so removal is injected at the
 * HRESULT boundary where Present reports it.
 *
 *   - D3D11_DeviceLoss_RecoveryRecreatesDeviceAndRendersAgain: after the reset
 *     the engine owns a new, healthy device and renders the same frame again.
 *   - D3D11_DeviceLoss_NoWindowHandleFailsWithoutTeardown: device-attach mode has
 *     no window to rebuild a swap chain for, so recovery refuses before releasing
 *     the caller's device.
 *   - D3D11_Resource_ResizeKeepsRendering: 1x1, 1920x1080 and back while rendering.
 *   - D3D11_Resource_CreateDestroyStressReturnsToBaseline: 2,000 texture/buffer/
 *     pipeline create-destroy cycles leave the debug layer's live-object count
 *     where it started.
 */

#include "TestFramework.h"

#ifdef _WIN32

#include "RHI210D3D11EngineFixture.h"
#include "Utils/GoldenImageTest.h"

#include "Graphics/RHI/D3D11/D3D11Device.h"
#include "Graphics/RHI/RHIPipelineTypes.h"
#include "Graphics/RHI/RHITypes.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using Microsoft::WRL::ComPtr;

namespace
{
    /// Reviewed threshold for the same scene before and after recovery: WARP is
    /// deterministic, but post-processing restarts with fresh history, so allow a
    /// small global shift (mean absolute RGB difference, 0-255 scale).
    constexpr double kRecoveredFrameMeanDifference = 6.0;

    bool PixelsDiffer(const uint8_t* a, const uint8_t* b)
    {
        return std::abs(int(a[0]) - int(b[0])) + std::abs(int(a[1]) - int(b[1])) + std::abs(int(a[2]) - int(b[2])) > 24;
    }

    /// Every live D3D11 object of @p device, as reported one message per object by
    /// ReportLiveDeviceObjects(DETAIL). Deferred destruction is flushed first.
    uint64_t CountLiveObjects(Spark::RHI::D3D11::D3D11Device& device, ID3D11Debug* debug)
    {
        ID3D11InfoQueue* queue = device.GetInfoQueue();
        device.GetD3D11Context()->ClearState();
        device.GetD3D11Context()->Flush();
        queue->ClearStoredMessages();
        debug->ReportLiveDeviceObjects(static_cast<D3D11_RLDO_FLAGS>(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL));
        const uint64_t live = queue->GetNumStoredMessages();
        queue->ClearStoredMessages();
        return live;
    }
} // namespace

TEST(D3D11_DeviceLoss_RecoveryRecreatesDeviceAndRendersAgain)
{
    RHI210::ScopedEnvironmentVariable warp(L"SPARK_D3D11_DRIVER", L"warp");
    RHI210::HiddenWindow window;
    ASSERT_TRUE(window.Get() != nullptr);

    GraphicsEngine engine;
    ASSERT_TRUE(SUCCEEDED(engine.Initialize(window.Get())));
    ASSERT_TRUE(RHI210::LoadCube(engine));

    RHI210::Frame before;
    for (int frame = 0; frame < 4; ++frame)
        before = RHI210::RenderCubeFrame(engine);
    ASSERT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(before.rgba, 0.95));

    // Hold the old device so the new one cannot reuse its address and make the
    // pointer comparison below pass by coincidence.
    ComPtr<ID3D11Device> lostDevice(engine.GetDevice());
    engine.Console_ResetDevice();

    ID3D11Device* recovered = engine.GetDevice();
    ASSERT_TRUE(recovered != nullptr);
    EXPECT_TRUE(recovered != lostDevice.Get());
    EXPECT_EQ(recovered->GetDeviceRemovedReason(), S_OK);
    EXPECT_TRUE(engine.GetSwapChain() != nullptr);
    lostDevice.Reset();

    // Recovery rebuilds the AssetPipeline on the new device; mesh owners upload
    // their geometry again, exactly as after a real removal.
    ASSERT_TRUE(RHI210::LoadCube(engine));
    RHI210::Frame after;
    for (int frame = 0; frame < 4; ++frame)
        after = RHI210::RenderCubeFrame(engine);

    ASSERT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(after.rgba, 0.95));
    EXPECT_EQ(after.width, before.width);
    EXPECT_EQ(after.height, before.height);
    const double difference = RHI210::MeanAbsoluteDifference(before, after);
    if (difference >= kRecoveredFrameMeanDifference)
        std::cerr << "  recovered frame mean RGB difference " << difference << "\n";
    EXPECT_LT(difference, kRecoveredFrameMeanDifference);
    // The cube is still drawn where it was: centre differs from the corner.
    EXPECT_TRUE(PixelsDiffer(after.At(after.width / 2, after.height / 2), after.At(1, 1)));
    engine.Shutdown();
}

TEST(D3D11_DeviceLoss_NoWindowHandleFailsWithoutTeardown)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel{};
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                            &device, &featureLevel, &context)));

    GraphicsEngine engine;
    ASSERT_TRUE(SUCCEEDED(engine.InitializeFromDevice(device.Get(), context.Get())));
    ASSERT_TRUE(engine.GetDevice() == device.Get());

    engine.Console_ResetDevice();

    // Nothing was torn down: the caller's device, context and basic pipeline stay live.
    EXPECT_TRUE(engine.GetDevice() == device.Get());
    EXPECT_TRUE(engine.GetContext() == context.Get());
    engine.SetBasicShaders();
    ComPtr<ID3D11VertexShader> boundVertexShader;
    context->VSGetShader(&boundVertexShader, nullptr, nullptr);
    EXPECT_TRUE(boundVertexShader != nullptr);
    engine.Shutdown();
}

TEST(D3D11_Resource_ResizeKeepsRendering)
{
    RHI210::ScopedEnvironmentVariable warp(L"SPARK_D3D11_DRIVER", L"warp");
    RHI210::HiddenWindow window;
    ASSERT_TRUE(window.Get() != nullptr);

    GraphicsEngine engine;
    ASSERT_TRUE(SUCCEEDED(engine.Initialize(window.Get())));
    ASSERT_TRUE(RHI210::LoadCube(engine));
    ID3D11Device* device = engine.GetDevice();

    struct Size
    {
        uint32_t width;
        uint32_t height;
    };
    for (const Size size : {Size{1, 1}, Size{1920, 1080}, Size{320, 240}})
    {
        engine.OnResize(size.width, size.height);
        const RHI210::Frame frame = RHI210::RenderCubeFrame(engine);
        EXPECT_EQ(frame.width, size.width);
        EXPECT_EQ(frame.height, size.height);
        EXPECT_EQ(device->GetDeviceRemovedReason(), S_OK);
        EXPECT_TRUE(engine.GetDevice() == device);
        // A single pixel cannot show geometry; every larger size must.
        if (size.width > 1)
            EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame.rgba, 0.95));
    }
    engine.Shutdown();
}

TEST(D3D11_Resource_CreateDestroyStressReturnsToBaseline)
{
    using namespace Spark::RHI;

    D3D11::D3D11Device device;
    RHIDeviceDesc deviceDesc;
    deviceDesc.preferredBackend = GraphicsBackend::D3D11;
    deviceDesc.enableDebugLayer = true;
    deviceDesc.applicationName = "SparkTests_RHI210_Stress";
    const bool initialized = device.Initialize(deviceDesc);
    if (!initialized)
        std::cerr << "  D3D11Device::Initialize failed with enableDebugLayer; is the D3D11 debug layer installed?\n";
    ASSERT_TRUE(initialized);
    ASSERT_TRUE(device.GetInfoQueue() != nullptr);
    ComPtr<ID3D11Debug> debug;
    ASSERT_TRUE(SUCCEEDED(device.GetD3D11Device()->QueryInterface(IID_PPV_ARGS(&debug))));

    RHIShaderDesc vsDesc;
    vsDesc.stage = RHIShaderStage::Vertex;
    vsDesc.debugName = "RHI210_StressVS";
    vsDesc.sourceCode = "float4 main(float2 pos : POSITION) : SV_Position { return float4(pos, 0.5f, 1.0f); }\n";
    RHIShaderDesc psDesc;
    psDesc.stage = RHIShaderStage::Pixel;
    psDesc.debugName = "RHI210_StressPS";
    psDesc.sourceCode = "float4 main() : SV_Target { return float4(0.2f, 0.6f, 0.9f, 1.0f); }\n";
    auto vs = device.CreateShader(vsDesc);
    auto ps = device.CreateShader(psDesc);
    ASSERT_TRUE(vs != nullptr && ps != nullptr);

    RHIPipelineStateDesc pipelineDesc;
    RHIInputElement position;
    position.semanticName = "POSITION";
    position.format = RHIVertexFormat::Float2;
    pipelineDesc.inputLayout.elements.push_back(position);
    pipelineDesc.renderTargetFormats[0] = PixelFormat::R8G8B8A8_UNORM;
    pipelineDesc.debugName = "RHI210_StressPSO";

    const float vertices[] = {-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, -0.5f};
    RHIBufferDesc bufferDesc;
    bufferDesc.size = sizeof(vertices);
    bufferDesc.stride = sizeof(float) * 2;
    bufferDesc.usage = RHIBufferUsage::Vertex;
    bufferDesc.access = RHIBufferAccess::Static;
    bufferDesc.initialData = vertices;
    bufferDesc.debugName = "RHI210_StressVB";

    RHITextureDesc textureDesc;
    textureDesc.width = 64;
    textureDesc.height = 64;
    textureDesc.format = PixelFormat::R8G8B8A8_UNORM;
    textureDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    textureDesc.debugName = "RHI210_StressRT";

    const uint64_t baseline = CountLiveObjects(device, debug.Get());
    ASSERT_TRUE(baseline > 0);

    // Control: the count sees one extra live object, so an unchanged count after
    // the loop is a measurement, not a counter that stopped counting.
    {
        auto held = device.CreateBuffer(bufferDesc);
        ASSERT_TRUE(held != nullptr);
        EXPECT_GT(CountLiveObjects(device, debug.Get()), baseline);
    }
    EXPECT_EQ(CountLiveObjects(device, debug.Get()), baseline);

    constexpr int kCycles = 2000;
    int created = 0;
    GraphicsEngine::ValidationCounts counts;
    for (int cycle = 0; cycle < kCycles; ++cycle)
    {
        auto texture = device.CreateTexture(textureDesc);
        auto buffer = device.CreateBuffer(bufferDesc);
        auto pipeline = device.CreatePipelineState(pipelineDesc, vs.get(), ps.get());
        if (texture && buffer && pipeline)
            ++created;
        // Drain every cycle: the queue keeps its default storage limit, and a
        // message dropped by it would never be classified.
        GraphicsEngine::AccumulateValidationMessages(device.GetInfoQueue(), counts);
    }
    EXPECT_EQ(created, kCycles);
    EXPECT_EQ(device.GetInfoQueue()->GetNumMessagesDiscardedByMessageCountLimit(), 0u);
    EXPECT_EQ(counts.corruption, 0u);
    EXPECT_EQ(counts.errors, 0u);

    EXPECT_EQ(CountLiveObjects(device, debug.Get()), baseline);

    vs.reset();
    ps.reset();
    debug.Reset();
    device.Shutdown();
}

#endif // _WIN32
