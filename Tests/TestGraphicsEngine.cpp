/**
 * @file TestGraphicsEngine.cpp
 * @brief Unit tests for GraphicsEngine state management, draw list, and statistics
 *
 * Tests configuration, quality presets, draw list operations, and statistics
 * without requiring a GPU or D3D11 device.
 */

#include "TestFramework.h"
#include "Graphics/GraphicsEngineTypes.h"

#include <algorithm>

// ============================================================================
// GraphicsSettings Defaults
// ============================================================================

TEST(GraphicsSettings_DefaultValues)
{
    GraphicsSettings settings;

    EXPECT_TRUE(settings.renderPath == RenderPath::Deferred);
    EXPECT_TRUE(settings.qualityPreset == QualityPreset::High);
    EXPECT_TRUE(settings.vsync);
    EXPECT_TRUE(settings.hdr);
    EXPECT_EQ(settings.msaaSamples, uint32_t(4));
    EXPECT_EQ(settings.maxTextureSize, uint32_t(2048));
    EXPECT_TRUE(settings.anisotropicFiltering);
    EXPECT_EQ(settings.anisotropyLevel, uint32_t(16));
    EXPECT_TRUE(settings.shadows);
    EXPECT_EQ(settings.shadowMapSize, uint32_t(2048));
    EXPECT_EQ(settings.cascadeCount, uint32_t(3));
    EXPECT_TRUE(settings.bloom);
    EXPECT_FALSE(settings.ssao);
    EXPECT_FALSE(settings.taa);
    EXPECT_FALSE(settings.motionBlur);
    EXPECT_TRUE(settings.frustumCulling);
    EXPECT_FALSE(settings.occlusionCulling);
    EXPECT_TRUE(settings.levelOfDetail);
    EXPECT_EQ(settings.maxDrawCalls, uint32_t(1000));
    EXPECT_FALSE(settings.wireframeMode);
    EXPECT_FALSE(settings.debugMode);
    EXPECT_FALSE(settings.showFPS);
    EXPECT_NEAR(settings.renderScale, 1.0f, 0.001f);
    EXPECT_FALSE(settings.enableGPUTiming);
}

TEST(GraphicsSettings_CustomValues)
{
    GraphicsSettings settings;
    settings.renderPath = RenderPath::Forward;
    settings.qualityPreset = QualityPreset::Low;
    settings.vsync = false;
    settings.hdr = false;
    settings.msaaSamples = 1;
    settings.shadowMapSize = 512;
    settings.bloom = false;
    settings.renderScale = 0.5f;

    EXPECT_TRUE(settings.renderPath == RenderPath::Forward);
    EXPECT_TRUE(settings.qualityPreset == QualityPreset::Low);
    EXPECT_FALSE(settings.vsync);
    EXPECT_FALSE(settings.hdr);
    EXPECT_EQ(settings.msaaSamples, uint32_t(1));
    EXPECT_EQ(settings.shadowMapSize, uint32_t(512));
    EXPECT_FALSE(settings.bloom);
    EXPECT_NEAR(settings.renderScale, 0.5f, 0.001f);
}

// ============================================================================
// RenderStatistics Defaults
// ============================================================================

TEST(RenderStatistics_DefaultZero)
{
    RenderStatistics stats;

    EXPECT_NEAR(stats.frameTime, 0.0f, 0.001f);
    EXPECT_NEAR(stats.cpuTime, 0.0f, 0.001f);
    EXPECT_NEAR(stats.gpuTime, 0.0f, 0.001f);
    EXPECT_EQ(stats.fps, uint32_t(0));
    EXPECT_EQ(stats.drawCalls, uint32_t(0));
    EXPECT_EQ(stats.triangles, uint32_t(0));
    EXPECT_EQ(stats.vertices, uint32_t(0));
    EXPECT_EQ(stats.textureBinds, uint32_t(0));
    EXPECT_EQ(stats.materialSwitches, uint32_t(0));
    EXPECT_EQ(stats.totalObjects, uint32_t(0));
    EXPECT_EQ(stats.visibleObjects, uint32_t(0));
    EXPECT_EQ(stats.culledObjects, uint32_t(0));
    EXPECT_EQ(stats.textureMemory, size_t(0));
    EXPECT_EQ(stats.meshMemory, size_t(0));
    EXPECT_EQ(stats.totalGPUMemory, size_t(0));
    EXPECT_EQ(stats.activeLights, uint32_t(0));
    EXPECT_EQ(stats.shadowUpdates, uint32_t(0));
}

TEST(RenderStatistics_CanSetFields)
{
    RenderStatistics stats;
    stats.drawCalls = 150;
    stats.triangles = 250000;
    stats.fps = 60;
    stats.frameTime = 16.67f;
    stats.totalObjects = 500;
    stats.visibleObjects = 200;
    stats.culledObjects = 300;

    EXPECT_EQ(stats.drawCalls, uint32_t(150));
    EXPECT_EQ(stats.triangles, uint32_t(250000));
    EXPECT_EQ(stats.fps, uint32_t(60));
    EXPECT_NEAR(stats.frameTime, 16.67f, 0.01f);
    EXPECT_EQ(stats.totalObjects, uint32_t(500));
    EXPECT_EQ(stats.visibleObjects, uint32_t(200));
    EXPECT_EQ(stats.culledObjects, uint32_t(300));
}

// ============================================================================
// SSAOSettings Defaults and Ranges
// ============================================================================

TEST(SSAOSettings_Defaults)
{
    SSAOSettings ssao;
    EXPECT_FALSE(ssao.enabled);
    EXPECT_NEAR(ssao.radius, 0.5f, 0.001f);
    EXPECT_NEAR(ssao.intensity, 1.0f, 0.001f);
    EXPECT_EQ(ssao.sampleCount, 16);
    EXPECT_NEAR(ssao.bias, 0.025f, 0.001f);
    EXPECT_TRUE(ssao.blur);
}

// ============================================================================
// SSRSettings Defaults
// ============================================================================

TEST(SSRSettings_Defaults)
{
    SSRSettings ssr;
    EXPECT_FALSE(ssr.enabled);
    EXPECT_NEAR(ssr.maxDistance, 100.0f, 0.01f);
    EXPECT_EQ(ssr.maxSteps, 32);
    EXPECT_NEAR(ssr.thickness, 0.5f, 0.001f);
    EXPECT_NEAR(ssr.fadeStart, 80.0f, 0.01f);
    EXPECT_NEAR(ssr.fadeEnd, 100.0f, 0.01f);
}

// ============================================================================
// VolumetricSettings Defaults
// ============================================================================

TEST(VolumetricSettings_Defaults)
{
    VolumetricSettings vol;
    EXPECT_FALSE(vol.enabled);
    EXPECT_EQ(vol.sampleCount, 32);
    EXPECT_NEAR(vol.scattering, 0.1f, 0.001f);
    EXPECT_NEAR(vol.extinction, 0.01f, 0.001f);
    EXPECT_NEAR(vol.anisotropy, 0.3f, 0.001f);
}

// ============================================================================
// QualityPreset Enum Values
// ============================================================================

TEST(QualityPreset_EnumValues)
{
    // Ensure distinct enum values
    EXPECT_NE(static_cast<int>(QualityPreset::Low), static_cast<int>(QualityPreset::Medium));
    EXPECT_NE(static_cast<int>(QualityPreset::Medium), static_cast<int>(QualityPreset::High));
    EXPECT_NE(static_cast<int>(QualityPreset::High), static_cast<int>(QualityPreset::Ultra));
    EXPECT_NE(static_cast<int>(QualityPreset::Ultra), static_cast<int>(QualityPreset::Custom));
}

TEST(MSAALevel_EnumValues)
{
    EXPECT_EQ(static_cast<int>(MSAALevel::None), 1);
    EXPECT_EQ(static_cast<int>(MSAALevel::MSAA2x), 2);
    EXPECT_EQ(static_cast<int>(MSAALevel::MSAA4x), 4);
    EXPECT_EQ(static_cast<int>(MSAALevel::MSAA8x), 8);
}

// ============================================================================
// GraphicsSettings Copy Semantics
// ============================================================================

TEST(GraphicsSettings_CopySemantic)
{
    GraphicsSettings original;
    original.renderPath = RenderPath::ForwardPlus;
    original.qualityPreset = QualityPreset::Ultra;
    original.vsync = false;
    original.shadowMapSize = 4096;
    original.bloom = false;

    GraphicsSettings copy = original;
    EXPECT_TRUE(copy.renderPath == RenderPath::ForwardPlus);
    EXPECT_TRUE(copy.qualityPreset == QualityPreset::Ultra);
    EXPECT_FALSE(copy.vsync);
    EXPECT_EQ(copy.shadowMapSize, uint32_t(4096));
    EXPECT_FALSE(copy.bloom);

    // Modify copy — original unchanged
    copy.vsync = true;
    EXPECT_FALSE(original.vsync);
    EXPECT_TRUE(copy.vsync);
}

// ============================================================================
// Render State Defaults
// ============================================================================

namespace TestGE
{

    enum class CullMode
    {
        None,
        Front,
        Back
    };

    struct RenderState
    {
        bool blendEnabled = false;
        bool depthTestEnabled = true;
        bool depthWriteEnabled = true;
        CullMode cullMode = CullMode::Back;
        bool scissorTestEnabled = false;
    };

    struct Viewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;

        float AspectRatio() const { return (height != 0.0f) ? width / height : 0.0f; }
    };

    struct ShaderPermutationKey
    {
        bool normalMapping = false;
        bool shadows = false;
        bool skinning = false;
        bool fog = false;

        uint32_t Hash() const
        {
            uint32_t h = 0;
            if (normalMapping)
                h |= (1u << 0);
            if (shadows)
                h |= (1u << 1);
            if (skinning)
                h |= (1u << 2);
            if (fog)
                h |= (1u << 3);
            return h;
        }

        bool operator==(const ShaderPermutationKey& other) const { return Hash() == other.Hash(); }
        bool operator!=(const ShaderPermutationKey& other) const { return !(*this == other); }
    };

    struct DrawCallEntry
    {
        uint32_t materialId = 0;
        float depth = 0.0f;
        uint32_t meshId = 0;

        bool operator<(const DrawCallEntry& other) const
        {
            if (materialId != other.materialId)
                return materialId < other.materialId;
            return depth < other.depth;
        }
    };

} // namespace TestGE

TEST(GraphicsEngine_RenderStateDefaults)
{
    TestGE::RenderState state;

    EXPECT_FALSE(state.blendEnabled);
    EXPECT_TRUE(state.depthTestEnabled);
    EXPECT_TRUE(state.depthWriteEnabled);
    EXPECT_TRUE(state.cullMode == TestGE::CullMode::Back);
    EXPECT_FALSE(state.scissorTestEnabled);
}

// ============================================================================
// Viewport Calculation
// ============================================================================

TEST(GraphicsEngine_ViewportCalculation)
{
    // 16:9 aspect ratio
    TestGE::Viewport vp16x9;
    vp16x9.width = 1920.0f;
    vp16x9.height = 1080.0f;
    EXPECT_NEAR(vp16x9.AspectRatio(), 16.0f / 9.0f, 0.01f);

    // 4:3 aspect ratio
    TestGE::Viewport vp4x3;
    vp4x3.width = 1024.0f;
    vp4x3.height = 768.0f;
    EXPECT_NEAR(vp4x3.AspectRatio(), 4.0f / 3.0f, 0.01f);

    // 21:9 ultrawide
    TestGE::Viewport vp21x9;
    vp21x9.width = 2560.0f;
    vp21x9.height = 1080.0f;
    EXPECT_NEAR(vp21x9.AspectRatio(), 2560.0f / 1080.0f, 0.01f);

    // Zero height returns 0 (no division by zero)
    TestGE::Viewport vpZero;
    vpZero.width = 1920.0f;
    vpZero.height = 0.0f;
    EXPECT_NEAR(vpZero.AspectRatio(), 0.0f, 0.001f);
}

// ============================================================================
// Shader Permutation Key
// ============================================================================

TEST(GraphicsEngine_ShaderPermutationKey)
{
    TestGE::ShaderPermutationKey keyA;
    keyA.normalMapping = true;
    keyA.shadows = false;

    TestGE::ShaderPermutationKey keyB;
    keyB.normalMapping = false;
    keyB.shadows = true;

    TestGE::ShaderPermutationKey keyC;
    keyC.normalMapping = true;
    keyC.shadows = true;

    TestGE::ShaderPermutationKey keyD;
    keyD.normalMapping = true;
    keyD.shadows = false;

    // Different combos produce different keys
    EXPECT_TRUE(keyA != keyB);
    EXPECT_TRUE(keyA != keyC);
    EXPECT_TRUE(keyB != keyC);

    // Same combo produces same key
    EXPECT_TRUE(keyA == keyD);

    // All hashes are unique for distinct combos
    EXPECT_NE(keyA.Hash(), keyB.Hash());
    EXPECT_NE(keyA.Hash(), keyC.Hash());
    EXPECT_NE(keyB.Hash(), keyC.Hash());
}

// ============================================================================
// Draw Call Sorting
// ============================================================================

TEST(GraphicsEngine_DrawCallSorting)
{
    std::vector<TestGE::DrawCallEntry> drawCalls = {
        {2, 10.0f, 100}, {1, 5.0f, 101}, {1, 2.0f, 102}, {3, 1.0f, 103}, {2, 3.0f, 104},
    };

    std::sort(drawCalls.begin(), drawCalls.end());

    // Sorted by materialId first
    EXPECT_EQ(drawCalls[0].materialId, 1u);
    EXPECT_EQ(drawCalls[1].materialId, 1u);
    EXPECT_EQ(drawCalls[2].materialId, 2u);
    EXPECT_EQ(drawCalls[3].materialId, 2u);
    EXPECT_EQ(drawCalls[4].materialId, 3u);

    // Within same material, sorted by depth
    EXPECT_NEAR(drawCalls[0].depth, 2.0f, 0.001f);
    EXPECT_NEAR(drawCalls[1].depth, 5.0f, 0.001f);
    EXPECT_NEAR(drawCalls[2].depth, 3.0f, 0.001f);
    EXPECT_NEAR(drawCalls[3].depth, 10.0f, 0.001f);
}
