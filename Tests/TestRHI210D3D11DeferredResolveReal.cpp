/** @file TestRHI210D3D11DeferredResolveReal.cpp
 * @brief WARP/debug-layer regressions for production deferred lighting, without golden baselines.
 */
#include "TestFramework.h"
#ifdef _WIN32
#include "RHI210D3D11EngineFixture.h"
#include "Game/CubeObject.h"
#include "Graphics/LightingSystem.h"

using namespace DirectX;

struct RHI210DeferredResolveAccess
{
    static void Geometry(GraphicsEngine& engine, GameObject& cube, const XMMATRIX& view, const XMMATRIX& projection)
    {
        ASSERT_TRUE(engine.CanResolveDeferredLighting()); // Fail closed if staged shaders are missing.
        const float clear[4]{};
        for (UINT i = 0; i < 3; ++i)
            engine.GetContext()->ClearRenderTargetView(engine.m_gBufferRTVs[i].Get(), clear);
        engine.FillGBuffer({&cube}, view, projection);
        engine.GetContext()->OMSetRenderTargets(0, nullptr, nullptr);
    }

    static void Lighting(GraphicsEngine& engine, const XMMATRIX& view, const XMMATRIX& projection)
    {
        const auto before = engine.GetStatistics().drawCalls;
        engine.LightingPass(view, projection);
        EXPECT_EQ(engine.GetStatistics().drawCalls, before + 1);
    }

    static void Material(GraphicsEngine& engine, float emissive)
    {
        // A constant material fixture in the real attachment isolates resolve
        // consumption from material loading. Geometry, depth and normal stay fixed.
        const float material[4] = {0.0f, 1.0f, emissive, 1.0f};
        engine.GetContext()->ClearRenderTargetView(engine.m_gBufferRTVs[2].Get(), material);
    }
};

namespace
{
    constexpr uint32_t kWidth = 160;
    constexpr uint32_t kHeight = 120;

    struct ResolveScene
    {
        RHI210::WarpGraphicsEngine warp{kWidth, kHeight};
        CubeObject cube{1.0f};
        XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(1.4f, 1.2f, -2.2f, 1), XMVectorZero(), XMVectorSet(0, 1, 0, 0));
        XMMATRIX projection =
            XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), float(kWidth) / kHeight, 0.1f, 100.0f);

        ResolveScene()
        {
            ASSERT_TRUE(warp.Ready());
            auto& engine = warp.Engine();
            engine.SetRenderingPipeline(RenderingPipeline::Deferred);
            ASSERT_TRUE(SUCCEEDED(cube.Initialize(engine.GetDevice(), engine.GetContext())));
            ASSERT_TRUE(engine.GetLightingSystem() != nullptr);
            // RemoveAllLights recreates the default sun; these tests need a
            // genuinely empty light set before adding their own current light.
            auto* lighting = engine.GetLightingSystem();
            const auto initialLights = lighting->GetLights();
            for (const auto& light : initialLights)
                lighting->RemoveLight(light);
            ASSERT_TRUE(lighting->GetLights().empty());
            engine.GetLightingSystem()->GetEnvironmentLighting().skyIntensity = 0.0f;
            engine.BeginFrame();
            engine.UpdateFrameConstants(view, projection, XMFLOAT3(1.4f, 1.2f, -2.2f));
            RHI210DeferredResolveAccess::Geometry(engine, cube, view, projection);
            RHI210DeferredResolveAccess::Material(engine, 0.0f);
        }

        ~ResolveScene() { warp.Engine().EndFrame(); }

        RHI210::Frame Capture()
        {
            auto& engine = warp.Engine();
            RHI210DeferredResolveAccess::Lighting(engine, view, projection);
            auto frame = RHI210::ReadBackBuffer(engine);
            ASSERT_EQ(frame.width, kWidth);
            ASSERT_EQ(frame.height, kHeight);
            ASSERT_EQ(frame.rgba.size(), size_t(kWidth) * kHeight * 4);
            const auto validation = engine.GetValidationCounts();
            ASSERT_TRUE(validation.has_value());
            EXPECT_EQ(validation->corruption, uint64_t(0));
            EXPECT_EQ(validation->errors, uint64_t(0));
            EXPECT_EQ(validation->warnings, uint64_t(0));
            Microsoft::WRL::ComPtr<ID3D11InfoQueue> queue;
            ASSERT_TRUE(SUCCEEDED(engine.GetDevice()->QueryInterface(IID_PPV_ARGS(&queue))));
            EXPECT_EQ(queue->GetNumMessagesDiscardedByMessageCountLimit(), uint64_t(0));
            return frame;
        }
    };

    size_t DominantPixels(const RHI210::Frame& frame, size_t channel, size_t other)
    {
        size_t count = 0;
        for (size_t i = 0; i < frame.rgba.size(); i += 4)
            count += int(frame.rgba[i + channel]) > int(frame.rgba[i + other]) + 10 ? 1 : 0;
        return count;
    }
} // namespace

TEST(D3D11DeferredResolve_CurrentEnabledLightChangesPixels)
{
    ResolveScene scene;
    auto* lighting = scene.warp.Engine().GetLightingSystem();
    auto light = lighting->CreateLight(LightType::Directional);
    ASSERT_TRUE(light != nullptr);
    light->SetDirection({-1.4f, -1.2f, 2.2f});
    light->SetColor({1.0f, 0.0f, 0.0f});
    light->SetIntensity(1.0f);
    light->SetCastShadows(false);
    light->SetEnabled(false);
    const auto disabled = scene.Capture();
    EXPECT_EQ(scene.warp.Engine().GetStatistics().activeLights, 0u);
    light->SetEnabled(true);
    const auto red = scene.Capture();
    EXPECT_EQ(scene.warp.Engine().GetStatistics().activeLights, 1u);
    light->SetColor({0.0f, 0.0f, 1.0f});
    const auto blue = scene.Capture();
    EXPECT_GT(RHI210::MeanAbsoluteDifference(disabled, red), 0.5);
    EXPECT_GT(RHI210::MeanAbsoluteDifference(red, blue), 0.5);
    EXPECT_GT(DominantPixels(red, 0, 2), size_t(100));
    EXPECT_GT(DominantPixels(blue, 2, 0), size_t(100));
    light->SetEnabled(false);
    const auto disabledAgain = scene.Capture();
    EXPECT_LE(RHI210::MeanAbsoluteDifference(disabled, disabledAgain), 0.01);
    EXPECT_EQ(scene.warp.Engine().GetStatistics().activeLights, 0u);
}

TEST(D3D11DeferredResolve_ZeroLightsUsesCurrentSky)
{
    ResolveScene scene;
    const auto dark = scene.Capture();
    auto& sky = scene.warp.Engine().GetLightingSystem()->GetEnvironmentLighting();
    sky.skyColor = {0.0f, 0.25f, 0.0f};
    sky.skyIntensity = 1.0f;
    const auto ambient = scene.Capture();
    EXPECT_GT(RHI210::MeanAbsoluteDifference(dark, ambient), 0.5);
    EXPECT_GT(DominantPixels(ambient, 1, 0), size_t(100));
    EXPECT_EQ(scene.warp.Engine().GetStatistics().activeLights, 0u);
    // Clear-depth background is not rewritten by the fullscreen triangle.
    for (size_t channel = 0; channel < 4; ++channel)
        EXPECT_EQ(dark.At(2, 2)[channel], ambient.At(2, 2)[channel]);
    sky.skyIntensity = 0.0f;
    EXPECT_LE(RHI210::MeanAbsoluteDifference(dark, scene.Capture()), 0.01);
}

TEST(D3D11DeferredResolve_MaterialEmissiveChangesUnlitPixels)
{
    ResolveScene scene;
    const auto dark = scene.Capture();
    RHI210DeferredResolveAccess::Material(scene.warp.Engine(), 0.25f);
    const auto emissive = scene.Capture();
    EXPECT_GT(RHI210::MeanAbsoluteDifference(dark, emissive), 0.5);
    EXPECT_EQ(scene.warp.Engine().GetStatistics().activeLights, 0u);
    RHI210DeferredResolveAccess::Material(scene.warp.Engine(), 0.0f);
    EXPECT_LE(RHI210::MeanAbsoluteDifference(dark, scene.Capture()), 0.01);
}
#endif
