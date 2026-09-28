/**
 * @file TestRHI210D3D11DeviceLossReal.cpp
 * @brief RHI-210: D3D11 device loss recovers and resized frames keep rendering.
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
 */

#include "TestFramework.h"

#ifdef _WIN32

#include "RHI210D3D11EngineFixture.h"
#include "Utils/GoldenImageTest.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

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

#endif // _WIN32
