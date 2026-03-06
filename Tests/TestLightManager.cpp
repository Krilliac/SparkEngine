// TestLightManager.cpp - Tests for light culling, tile binning, and shadow atlas
// Uses the actual LightManager.h header

#include "TestFramework.h"
#include "Graphics/LightManager.h"

// =============================================================================
// Tests
// =============================================================================

TEST(LightManager_Initialize) {
    LightManager mgr;
    EXPECT_TRUE(mgr.Initialize(1920, 1080, 16));
    EXPECT_EQ(mgr.GetTilesX(), 120);
    EXPECT_EQ(mgr.GetTilesY(), 68);
    mgr.Shutdown();
}

TEST(LightManager_SubmitLight) {
    LightManager mgr;
    mgr.Initialize(800, 600, 16);

    XMFLOAT4X4 identity = {};
    for (int i = 0; i < 4; ++i) identity.m[i][i] = 1.0f;
    mgr.BeginFrame(identity);

    RuntimeLight light;
    light.id = 1;
    light.type = RuntimeLightType::Point;
    light.position = {0, 0, 0};
    light.intensity = 2.0f;
    light.range = 10.0f;
    light.enabled = true;

    mgr.SubmitLight(light);
    EXPECT_EQ((int)mgr.GetLights().size(), 1);
    EXPECT_EQ(mgr.GetMetrics().totalSubmitted, 1);
    mgr.Shutdown();
}

TEST(LightManager_DisabledLightNotSubmitted) {
    LightManager mgr;
    mgr.Initialize();

    XMFLOAT4X4 identity = {};
    for (int i = 0; i < 4; ++i) identity.m[i][i] = 1.0f;
    mgr.BeginFrame(identity);

    RuntimeLight light;
    light.enabled = false;
    mgr.SubmitLight(light);

    EXPECT_EQ((int)mgr.GetLights().size(), 0);
    mgr.Shutdown();
}

TEST(LightManager_CullAndBin) {
    LightManager mgr;
    mgr.Initialize(800, 600, 16);

    // Use identity as view-proj (simple case)
    XMFLOAT4X4 identity = {};
    for (int i = 0; i < 4; ++i) identity.m[i][i] = 1.0f;
    mgr.BeginFrame(identity);

    RuntimeLight dirLight;
    dirLight.id = 1;
    dirLight.type = RuntimeLightType::Directional;
    dirLight.enabled = true;
    mgr.SubmitLight(dirLight);

    RuntimeLight pointLight;
    pointLight.id = 2;
    pointLight.type = RuntimeLightType::Point;
    pointLight.position = {0, 0, 0};
    pointLight.range = 100.0f;
    pointLight.enabled = true;
    mgr.SubmitLight(pointLight);

    mgr.CullAndBinLights();

    // Directional lights are always visible
    auto& metrics = mgr.GetMetrics();
    EXPECT_GE(metrics.visibleAfterCull, 1);
    EXPECT_EQ(metrics.totalSubmitted, 2);
    mgr.Shutdown();
}

TEST(LightManager_GetVisibleLights) {
    LightManager mgr;
    mgr.Initialize(800, 600, 16);

    XMFLOAT4X4 identity = {};
    for (int i = 0; i < 4; ++i) identity.m[i][i] = 1.0f;
    mgr.BeginFrame(identity);

    RuntimeLight light;
    light.id = 1;
    light.type = RuntimeLightType::Directional;
    light.enabled = true;
    mgr.SubmitLight(light);

    mgr.CullAndBinLights();

    auto visible = mgr.GetVisibleLights();
    EXPECT_GE((int)visible.size(), 1);
    EXPECT_EQ(visible[0]->id, (uint32_t)1);
    mgr.Shutdown();
}

TEST(LightManager_ShadowCasters) {
    LightManager mgr;
    mgr.Initialize(800, 600, 16);

    XMFLOAT4X4 identity = {};
    for (int i = 0; i < 4; ++i) identity.m[i][i] = 1.0f;
    mgr.BeginFrame(identity);

    RuntimeLight light;
    light.id = 1;
    light.type = RuntimeLightType::Directional;
    light.castsShadows = true;
    light.enabled = true;
    mgr.SubmitLight(light);

    RuntimeLight noShadow;
    noShadow.id = 2;
    noShadow.type = RuntimeLightType::Directional;
    noShadow.castsShadows = false;
    noShadow.enabled = true;
    mgr.SubmitLight(noShadow);

    mgr.CullAndBinLights();

    auto casters = mgr.GetShadowCasters();
    EXPECT_EQ((int)casters.size(), 1);
    EXPECT_EQ(casters[0]->id, (uint32_t)1);
    mgr.Shutdown();
}

TEST(LightManager_ShadowAtlasAllocation) {
    LightManager mgr;
    mgr.Initialize();

    int slot = mgr.AllocateShadowSlot(1, 1024);
    EXPECT_GE(slot, 0);

    // Same light should return same slot
    int slot2 = mgr.AllocateShadowSlot(1, 1024);
    EXPECT_EQ(slot, slot2);

    // Different light gets different slot
    int slot3 = mgr.AllocateShadowSlot(2, 512);
    EXPECT_GE(slot3, 0);
    EXPECT_NE(slot, slot3);

    mgr.Shutdown();
}

TEST(LightManager_ShadowAtlasFree) {
    LightManager mgr;
    mgr.Initialize();

    int slot = mgr.AllocateShadowSlot(1, 1024);
    EXPECT_GE(slot, 0);

    mgr.FreeShadowSlot(1);

    // After freeing, a new light can get a slot (possibly the same one)
    int newSlot = mgr.AllocateShadowSlot(99, 512);
    EXPECT_GE(newSlot, 0);

    mgr.Shutdown();
}

TEST(LightManager_Resize) {
    LightManager mgr;
    mgr.Initialize(1920, 1080, 16);
    EXPECT_EQ(mgr.GetTilesX(), 120);
    EXPECT_EQ(mgr.GetTilesY(), 68);

    mgr.Resize(1280, 720);
    EXPECT_EQ(mgr.GetTilesX(), 80);
    EXPECT_EQ(mgr.GetTilesY(), 45);

    mgr.Shutdown();
}

TEST(LightManager_TileLightList) {
    LightManager mgr;
    mgr.Initialize(320, 240, 16);

    XMFLOAT4X4 identity = {};
    for (int i = 0; i < 4; ++i) identity.m[i][i] = 1.0f;
    mgr.BeginFrame(identity);

    RuntimeLight dirLight;
    dirLight.id = 1;
    dirLight.type = RuntimeLightType::Directional;
    dirLight.enabled = true;
    mgr.SubmitLight(dirLight);

    mgr.CullAndBinLights();

    // Directional lights go into all tiles
    auto& tile = mgr.GetTileLightList(0, 0);
    EXPECT_GT(tile.lightCount, 0);

    mgr.Shutdown();
}

TEST(LightManager_ConsoleStatus) {
    LightManager mgr;
    mgr.Initialize(800, 600, 16);
    std::string status = mgr.Console_GetStatus();
    EXPECT_TRUE(status.find("Light Manager") != std::string::npos);
    mgr.Shutdown();
}

TEST(LightManager_BeginFrameClears) {
    LightManager mgr;
    mgr.Initialize(800, 600, 16);

    XMFLOAT4X4 identity = {};
    for (int i = 0; i < 4; ++i) identity.m[i][i] = 1.0f;

    mgr.BeginFrame(identity);
    RuntimeLight light;
    light.id = 1;
    light.enabled = true;
    mgr.SubmitLight(light);
    EXPECT_EQ((int)mgr.GetLights().size(), 1);

    // New frame should clear
    mgr.BeginFrame(identity);
    EXPECT_EQ((int)mgr.GetLights().size(), 0);

    mgr.Shutdown();
}

TEST(LightManager_FrustumTestSphere) {
    // Test the ViewFrustum directly
    ViewFrustum f;
    // Simple frustum: just one plane facing +Z at distance 10
    f.planes[0] = {0, 0, 0, 1}; // always passes
    f.planes[1] = {0, 0, 0, 1};
    f.planes[2] = {0, 0, 0, 1};
    f.planes[3] = {0, 0, 0, 1};
    f.planes[4] = {0, 0, 0, 1};
    f.planes[5] = {0, 0, 1, -10}; // z > 10

    XMFLOAT3 center = {0, 0, 15};
    EXPECT_TRUE(f.TestSphere(center, 1.0f)); // fully inside

    XMFLOAT3 behind = {0, 0, 5};
    EXPECT_FALSE(f.TestSphere(behind, 1.0f)); // behind the plane

    XMFLOAT3 edge = {0, 0, 9.5f};
    EXPECT_TRUE(f.TestSphere(edge, 1.0f)); // sphere overlaps plane
}
