/**
 * @file TestShadowPassReal.cpp
 * @brief Production-linked coverage for the D3D11 shadow-caster depth pass,
 *        the cached shadow atlas wiring, real render statistics, and honest
 *        upscaling availability reporting.
 *
 * Every test drives the real production classes (GraphicsEngine, LightingSystem,
 * UpscalingSystem) — there are no local reimplementations. The D3D11 tests fall
 * back to the WARP software rasterizer and skip cleanly when no device can be
 * created.
 */
#include "TestFramework.h"

#include "Graphics/UpscalingSystem.h"

#include <cstdint>
#include <string>

#ifdef _WIN32

#include "Game/GameObject.h"
#include "Graphics/AssetPipeline.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/GraphicsRenderPipelinesShadowPass.h"
#include "Graphics/LightingSystem.h"
#include "Graphics/Mesh.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    bool CreateWarpDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context)
    {
        D3D_FEATURE_LEVEL featureLevel{};
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                           device.GetAddressOf(), &featureLevel, context.GetAddressOf()));
    }

    /// Minimal concrete GameObject whose mesh is a sphere — deliberately not a
    /// cube, so its 12-triangle count cannot be confused with the constant the
    /// render pipelines used to report.
    class SphereObject final : public GameObject
    {
      public:
        void OnHit(GameObject* /*target*/) override {}
        void OnHitWorld(const XMFLOAT3& /*hitPoint*/, const XMFLOAT3& /*normal*/) override {}

      protected:
        void CreateMesh() override
        {
            if (m_mesh)
            {
                m_mesh->CreateSphere(1.0f, 12, 12);
            }
        }
    };
} // namespace

// ============================================================================
// Shadow-caster depth pass
// ============================================================================

TEST(ShadowPass_NoDeviceIssuesNoDrawCalls)
{
    // A default-constructed GraphicsEngine owns an AssetPipeline but has no
    // D3D11 context: the depth pass must issue nothing rather than dereference.
    GraphicsEngine engine;

    const std::string meshPath = "test://shadow_caster";
    const std::string materialPath = "test://material";
    engine.SubmitMeshForRendering(meshPath, materialPath, XMMatrixIdentity(), /*castShadows*/ true);

    const std::vector<GraphicsEngine::MeshDrawCommand> drawList = engine.GetDrawList();
    EXPECT_EQ(drawList.size(), static_cast<size_t>(1));

    const uint32_t draws = Spark::Graphics::RenderShadowCasterDepth(engine, drawList, XMMatrixIdentity(),
                                                                    XMMatrixIdentity());
    EXPECT_EQ(draws, 0u);
}

TEST(ShadowPass_OnlyShadowCastersAreDrawn)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateWarpDevice(device, context))
    {
        SKIP_TEST("No D3D11 device available (hardware or WARP)");
    }

    GraphicsEngine engine;
    ASSERT_TRUE(SUCCEEDED(engine.InitializeFromDevice(device.Get(), context.Get())));

    const std::string casterA = "test://caster_a";
    const std::string casterB = "test://caster_b";
    const std::string nonCaster = "test://non_caster";
    const std::string materialPath = "test://material";

    // Each path must resolve to a real, GPU-resident mesh or the depth pass
    // (correctly) refuses to draw it. MeshAsset::Load falls back to a unit cube
    // when the path names no file on disk, so these become real bindable meshes.
    AssetPipeline* assetPipeline = engine.GetAssetPipeline();
    ASSERT_TRUE(assetPipeline != nullptr);
    // InitializeFromDevice attaches the renderer only; the asset pipeline needs the
    // device/context of its own before it can build mesh GPU buffers.
    ASSERT_TRUE(SUCCEEDED(assetPipeline->Initialize(device.Get(), context.Get())));
    ASSERT_TRUE(assetPipeline->LoadMesh(casterA) != nullptr);
    ASSERT_TRUE(assetPipeline->LoadMesh(casterB) != nullptr);
    ASSERT_TRUE(assetPipeline->LoadMesh(nonCaster) != nullptr);

    engine.SubmitMeshForRendering(casterA, materialPath, XMMatrixIdentity(), /*castShadows*/ true);
    engine.SubmitMeshForRendering(nonCaster, materialPath, XMMatrixIdentity(), /*castShadows*/ false);
    engine.SubmitMeshForRendering(casterB, materialPath, XMMatrixIdentity(), /*castShadows*/ true);

    const std::vector<GraphicsEngine::MeshDrawCommand> drawList = engine.GetDrawList();
    ASSERT_EQ(drawList.size(), static_cast<size_t>(3));

    // castShadows had no reader anywhere in the engine before the depth pass
    // existed; the pass must honour it. Two of the three commands cast shadows
    // and all three are bindable, so exactly two draws must be submitted.
    const uint32_t draws = Spark::Graphics::RenderShadowCasterDepth(engine, drawList, XMMatrixIdentity(),
                                                                    XMMatrixIdentity());
    EXPECT_EQ(draws, 2u);

    engine.Shutdown();
}

TEST(ShadowPass_UnbindableCastersAreNotCountedAsDraws)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateWarpDevice(device, context))
    {
        SKIP_TEST("No D3D11 device available (hardware or WARP)");
    }

    GraphicsEngine engine;
    ASSERT_TRUE(SUCCEEDED(engine.InitializeFromDevice(device.Get(), context.Get())));

    // Deliberately never loaded: AssetPipeline::BindMesh cannot resolve these, so
    // no geometry reaches the depth target. The returned count used to be the
    // loop counter (2), which LightingPass added to RenderStatistics::drawCalls —
    // a draw statistic that reported work the GPU never saw.
    const std::string materialPath = "test://material";
    engine.SubmitMeshForRendering("test://never_loaded_a", materialPath, XMMatrixIdentity(), /*castShadows*/ true);
    engine.SubmitMeshForRendering("test://never_loaded_b", materialPath, XMMatrixIdentity(), /*castShadows*/ true);

    const std::vector<GraphicsEngine::MeshDrawCommand> drawList = engine.GetDrawList();
    ASSERT_EQ(drawList.size(), static_cast<size_t>(2));

    const uint32_t draws =
        Spark::Graphics::RenderShadowCasterDepth(engine, drawList, XMMatrixIdentity(), XMMatrixIdentity());
    EXPECT_EQ(draws, 0u);

    engine.Shutdown();
}

TEST(ShadowPass_DrawListSurvivesTheDepthPass)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateWarpDevice(device, context))
    {
        SKIP_TEST("No D3D11 device available (hardware or WARP)");
    }

    GraphicsEngine engine;
    ASSERT_TRUE(SUCCEEDED(engine.InitializeFromDevice(device.Get(), context.Get())));

    const std::string meshPath = "test://caster";
    const std::string materialPath = "test://material";
    engine.SubmitMeshForRendering(meshPath, materialPath, XMMatrixIdentity(), /*castShadows*/ true);
    engine.SubmitMeshForRendering(meshPath, materialPath, XMMatrixIdentity(), /*castShadows*/ true);

    // The shadow pass runs before the geometry pass drains the list, so it must
    // read the commands without consuming them.
    const std::vector<GraphicsEngine::MeshDrawCommand> drawList = engine.GetDrawList();
    Spark::Graphics::RenderShadowCasterDepth(engine, drawList, XMMatrixIdentity(), XMMatrixIdentity());

    EXPECT_EQ(engine.GetDrawList().size(), static_cast<size_t>(2));

    engine.Shutdown();
}

TEST(ShadowPass_LightingSystemInvokesTheCallbackPerShadowCastingLight)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateWarpDevice(device, context))
    {
        SKIP_TEST("No D3D11 device available (hardware or WARP)");
    }

    LightingSystem lighting;
    ASSERT_TRUE(SUCCEEDED(lighting.Initialize(device.Get(), context.Get())));

    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.0f, 5.0f, -10.0f, 1.0f), XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f),
                                           XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 16.0f / 9.0f, 0.1f, 100.0f);
    lighting.Update(0.016f, view, proj);

    uint32_t callbackInvocations = 0;
    bool depthTargetBound = true;
    lighting.RenderShadowMaps(
        [&](const XMMATRIX& /*lightView*/, const XMMATRIX& /*lightProj*/)
        {
            ++callbackInvocations;

            // The callback must run with the light's depth-stencil view bound —
            // that is what makes a depth-only draw land in the shadow map.
            ComPtr<ID3D11RenderTargetView> boundRTV;
            ComPtr<ID3D11DepthStencilView> boundDSV;
            context->OMGetRenderTargets(1, boundRTV.GetAddressOf(), boundDSV.GetAddressOf());
            if (!boundDSV)
            {
                depthTargetBound = false;
            }
        });

    EXPECT_GE(callbackInvocations, 1u);
    EXPECT_TRUE(depthTargetBound);
    EXPECT_GE(lighting.Console_GetMetrics().shadowMapUpdates, 1u);

    lighting.Shutdown();
}

TEST(ShadowPass_DirectionalLightCallbackProjectionIsIdentity)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateWarpDevice(device, context))
    {
        SKIP_TEST("No D3D11 device available (hardware or WARP)");
    }

    LightingSystem lighting;
    ASSERT_TRUE(SUCCEEDED(lighting.Initialize(device.Get(), context.Get())));

    // The default LightingSystem owns one directional sun on the standard (PCF)
    // shadow path, so the non-CSM branch of RenderShadowMaps is what runs here.
    ASSERT_EQ(lighting.GetLights().size(), static_cast<size_t>(1));
    ASSERT_TRUE(lighting.GetLights()[0]->GetType() == LightType::Directional);

    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.0f, 5.0f, -10.0f, 1.0f), XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f),
                                           XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 16.0f / 9.0f, 0.1f, 100.0f);
    lighting.Update(0.016f, view, proj);

    // CalculateLightMatrix returns a combined light view * projection for a
    // directional light. RenderShadowCasterDepth multiplies world * view * proj,
    // so the projection handed to the callback has to be the identity; anything
    // else (the light's own 20x20 orthographic matrix, for instance) projects the
    // shadow map twice and every directional shadow is rendered with a garbage
    // transform. This test fails if that second projection ever comes back.
    uint32_t invocations = 0;
    XMMATRIX capturedView = XMMatrixIdentity();
    // Seeded with a matrix that is NOT the identity, so a callback that never runs
    // cannot make the assertions below pass vacuously.
    XMMATRIX capturedProj = XMMatrixOrthographicLH(20.0f, 20.0f, 0.1f, 100.0f);
    lighting.RenderShadowMaps(
        [&](const XMMATRIX& lightView, const XMMATRIX& lightProj)
        {
            ++invocations;
            capturedView = lightView;
            capturedProj = lightProj;
        });

    ASSERT_TRUE(invocations >= 1u);

    const XMMATRIX identity = XMMatrixIdentity();
    for (int row = 0; row < 4; ++row)
    {
        XMFLOAT4 projRow;
        XMFLOAT4 identityRow;
        XMStoreFloat4(&projRow, capturedProj.r[row]);
        XMStoreFloat4(&identityRow, identity.r[row]);
        EXPECT_NEAR(projRow.x, identityRow.x, 1e-5f);
        EXPECT_NEAR(projRow.y, identityRow.y, 1e-5f);
        EXPECT_NEAR(projRow.z, identityRow.z, 1e-5f);
        EXPECT_NEAR(projRow.w, identityRow.w, 1e-5f);
    }

    // ...and the view slot must actually carry the combined light view-projection,
    // not the identity a "just pass identity for both" regression would leave behind.
    XMFLOAT4X4 viewValues;
    XMStoreFloat4x4(&viewValues, capturedView);
    EXPECT_FALSE(XMMatrixIsIdentity(capturedView));
    EXPECT_NEAR(viewValues.m[3][3], 1.0f, 1e-3f);

    lighting.Shutdown();
}

// ============================================================================
// Cached shadow atlas wiring
// ============================================================================

TEST(ShadowAtlas_LightingSystemRequestsATileForEachShadowCaster)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateWarpDevice(device, context))
    {
        SKIP_TEST("No D3D11 device available (hardware or WARP)");
    }

    LightingSystem lighting;
    ASSERT_TRUE(SUCCEEDED(lighting.Initialize(device.Get(), context.Get())));
    ASSERT_TRUE(lighting.GetCachedShadowAtlas().IsInitialized());

    // The default LightingSystem owns exactly one (shadow-casting) sun.
    ASSERT_EQ(lighting.GetLights().size(), static_cast<size_t>(1));

    const XMMATRIX view = XMMatrixIdentity();
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 16.0f / 9.0f, 0.1f, 100.0f);
    lighting.Update(0.016f, view, proj);

    // Nothing called RequestShadow before this lane's change, so the atlas
    // allocator saw zero lights and handed out zero tiles every frame.
    const auto& shadowCache = lighting.GetCachedShadowAtlas();
    EXPECT_EQ(shadowCache.GetShadowsToRender().size(), static_cast<size_t>(1));
    EXPECT_TRUE(shadowCache.GetTile(0) != nullptr);
    EXPECT_EQ(lighting.Console_GetMetrics().shadowCastingLights, 1u);

    lighting.Shutdown();
}

// ============================================================================
// Render statistics
// ============================================================================

TEST(RenderStatistics_ReportTheRealMeshCountsNotAConstant)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateWarpDevice(device, context))
    {
        SKIP_TEST("No D3D11 device available (hardware or WARP)");
    }

    GraphicsEngine engine;
    ASSERT_TRUE(SUCCEEDED(engine.InitializeFromDevice(device.Get(), context.Get())));

    SphereObject sphere;
    ASSERT_TRUE(SUCCEEDED(sphere.Initialize(device.Get(), context.Get())));

    const Mesh* mesh = sphere.GetMesh();
    ASSERT_TRUE(mesh != nullptr);
    ASSERT_TRUE(mesh->GetIndexCount() > 36);

    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.0f, 5.0f, -10.0f, 1.0f), XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f),
                                           XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 16.0f / 9.0f, 0.1f, 100.0f);

    std::vector<GameObject*> objects{&sphere};
    engine.RenderScene(view, proj, objects);

    // Every object used to be counted as 12 triangles / 36 vertices regardless
    // of its mesh, which made the budget metrics unable to detect a regression.
    const RenderStatistics& statistics = engine.GetStatistics();
    EXPECT_EQ(statistics.drawCalls, 1u);
    EXPECT_EQ(statistics.triangles, mesh->GetIndexCount() / 3);
    EXPECT_EQ(statistics.vertices, mesh->GetVertexCount());
    EXPECT_NE(statistics.triangles, 12u);

    sphere.Shutdown();
    engine.Shutdown();
}

#endif // _WIN32

// ============================================================================
// Upscaling availability reporting (platform independent)
// ============================================================================

TEST(Upscaling_VendorBackendsReportUnavailableWithoutAnSDK)
{
    UpscalingSystem upscaling;

    // Initialize runs DetectFeatures() before it needs a device, so the
    // availability flags are populated even though GPU resource creation fails.
    upscaling.Initialize(nullptr, nullptr, 1920, 1080);

    // m_fsr2Available used to be set to true unconditionally, and DLSS/XeSS
    // mirrored a "is the runtime DLL on this machine" probe — neither of which
    // makes a vendor upscaler runnable while no vendor SDK is linked.
    EXPECT_FALSE(upscaling.IsFSR2Available());
    EXPECT_FALSE(upscaling.GetDLSSFeatureInfo().isAvailable);
    EXPECT_FALSE(upscaling.GetXeSSFeatureInfo().isAvailable);
}

TEST(Upscaling_StatusLineReportsVendorAvailability)
{
    UpscalingSystem upscaling;
    upscaling.Initialize(nullptr, nullptr, 1920, 1080);

    const std::string status = upscaling.Console_GetStatus();
    EXPECT_STR_CONTAINS(status, "FSR 2.0 available: NO");
    EXPECT_STR_CONTAINS(status, "DLSS available: NO");
    EXPECT_STR_CONTAINS(status, "XeSS available: NO");
}

TEST(Upscaling_StatusLineNamesTheUpscalerThatActuallyRuns)
{
    UpscalingSystem upscaling;
    upscaling.Initialize(nullptr, nullptr, 1920, 1080);

    // ExecuteDLSS / ExecuteXeSS / ExecuteFSR2 all forward to ExecuteSparkSR while no
    // vendor SDK is linked. Reporting "Mode: DLSS" and nothing else would name an
    // upscaler that never runs, so the status line has to admit the substitution.
    upscaling.SetMode(UpscalingMode::DLSS);
    EXPECT_TRUE(upscaling.GetEffectiveMode() == UpscalingMode::SparkSR);
    EXPECT_STR_CONTAINS(upscaling.Console_GetStatus(), "Mode: DLSS (running SparkSR");

    upscaling.SetMode(UpscalingMode::XeSS);
    EXPECT_TRUE(upscaling.GetEffectiveMode() == UpscalingMode::SparkSR);

    // SparkSR is engine-native, so requested and effective must agree and the
    // status line must carry no substitution note.
    upscaling.SetMode(UpscalingMode::SparkSR);
    EXPECT_TRUE(upscaling.GetEffectiveMode() == UpscalingMode::SparkSR);
    EXPECT_STR_CONTAINS(upscaling.Console_GetStatus(), "Mode: SparkSR\n");
}
