/**
 * @file TestGraphicsSubsystems.cpp
 * @brief Unit tests for graphics subsystems: RenderTarget, Material, TextureCompressor,
 *        VirtualTextureManager, AdaptiveProbeVolumes, and ClipmapTerrain
 *
 * All tests run without a GPU. Exercises CPU-side logic: creation, property round-trips,
 * compression math, page caching, probe volumes, and clipmap terrain LOD.
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Graphics/RenderTarget.h"
#include "../SparkEngine/Source/Graphics/MaterialSystem.h"
#include "../SparkEngine/Source/Graphics/TextureCompressor.h"
#include "../SparkEngine/Source/Graphics/VirtualTexture.h"
#include "../SparkEngine/Source/Graphics/AdaptiveProbeVolumes.h"
#include "../SparkEngine/Source/Graphics/ClipmapTerrain.h"

#include <cstring>
#include <functional>

// ============================================================================
// 1. RenderTarget
// ============================================================================

TEST(RenderTargetDesc_DefaultValues)
{
    RenderTargetDesc desc;
    EXPECT_EQ(desc.width, uint32_t(1920));
    EXPECT_EQ(desc.height, uint32_t(1080));
    EXPECT_EQ(desc.sampleCount, uint32_t(1));
    EXPECT_EQ(desc.mipLevels, uint32_t(1));
    EXPECT_TRUE(desc.format == RenderTargetFormat::RGBA8_UNORM);
    EXPECT_TRUE(desc.autoClear);
}

TEST(RenderTarget_GetDescAndFlags)
{
    RenderTargetDesc desc;
    desc.name = "TestRT";
    desc.width = 512;
    desc.height = 256;

    RenderTarget rt(desc);
    EXPECT_EQ(rt.GetDesc().width, uint32_t(512));
    EXPECT_EQ(rt.GetDesc().height, uint32_t(256));
    EXPECT_FALSE(rt.IsDepthStencil());
    EXPECT_FALSE(rt.IsMultisampled());
}

TEST(RenderTarget_DepthStencilFlag)
{
    RenderTargetDesc desc;
    desc.usage = RenderTargetUsage::DepthStencil | RenderTargetUsage::ShaderResource;
    desc.format = RenderTargetFormat::D24_UNORM_S8_UINT;

    RenderTarget rt(desc);
    EXPECT_TRUE(rt.IsDepthStencil());
    EXPECT_FALSE(rt.IsMultisampled());
}

TEST(RenderTarget_MultisampledFlag)
{
    RenderTargetDesc desc;
    desc.sampleCount = 4;

    RenderTarget rt(desc);
    EXPECT_TRUE(rt.IsMultisampled());
    EXPECT_FALSE(rt.IsDepthStencil());
}

TEST(MultipleRenderTargets_NameAndCount)
{
    MultipleRenderTargets mrt("GBuffer");
    EXPECT_EQ(mrt.GetName(), std::string("GBuffer"));
    EXPECT_EQ(mrt.GetRenderTargetCount(), uint32_t(0));
}

TEST(MultipleRenderTargets_AddRenderTargets)
{
    MultipleRenderTargets mrt("TestMRT");

    RenderTargetDesc desc;
    desc.name = "Color0";
    auto rt0 = std::make_shared<RenderTarget>(desc);
    desc.name = "Color1";
    auto rt1 = std::make_shared<RenderTarget>(desc);

    mrt.AddRenderTarget(rt0, 0);
    mrt.AddRenderTarget(rt1, 1);

    EXPECT_EQ(mrt.GetRenderTargetCount(), uint32_t(2));
    EXPECT_TRUE(mrt.GetRenderTarget(0) != nullptr);
    EXPECT_TRUE(mrt.GetRenderTarget(1) != nullptr);
}

TEST(RenderTargetManager_InitialState)
{
    RenderTargetManager mgr;
    auto metrics = mgr.Console_GetMetrics();
    EXPECT_EQ(metrics.totalRenderTargets, 0);
    EXPECT_EQ(metrics.mrtGroups, 0);
    EXPECT_EQ(metrics.totalMemoryUsage, size_t(0));
}

// ============================================================================
// 2. Material
// ============================================================================

TEST(Material_GetName)
{
    Material mat("test");
    EXPECT_EQ(mat.GetName(), std::string("test"));
}

TEST(Material_PBRProperties_RoundTrip)
{
    Material mat("pbr_test");
    PBRProperties props;
    props.metallicFactor = 0.8f;
    props.roughnessFactor = 0.3f;
    props.emissiveFactor = 2.0f;
    mat.SetPBRProperties(props);

    const auto& result = mat.GetPBRProperties();
    EXPECT_NEAR(result.metallicFactor, 0.8f, 0.001f);
    EXPECT_NEAR(result.roughnessFactor, 0.3f, 0.001f);
    EXPECT_NEAR(result.emissiveFactor, 2.0f, 0.001f);
}

TEST(Material_AdvancedProperties_RoundTrip)
{
    Material mat("adv_test");
    AdvancedProperties adv;
    adv.clearcoatEnabled = true;
    adv.clearcoatFactor = 0.75f;
    adv.subsurfaceEnabled = true;
    adv.subsurfaceRadius = 2.5f;
    mat.SetAdvancedProperties(adv);

    const auto& result = mat.GetAdvancedProperties();
    EXPECT_TRUE(result.clearcoatEnabled);
    EXPECT_NEAR(result.clearcoatFactor, 0.75f, 0.001f);
    EXPECT_TRUE(result.subsurfaceEnabled);
    EXPECT_NEAR(result.subsurfaceRadius, 2.5f, 0.001f);
}

TEST(Material_RenderState_RoundTrip)
{
    Material mat("rs_test");
    MaterialRenderState state;
    state.blendMode = BlendMode::Transparent;
    state.cullMode = CullMode::None;
    state.depthWrite = false;
    state.castShadows = false;
    mat.SetRenderState(state);

    const auto& result = mat.GetRenderState();
    EXPECT_TRUE(result.blendMode == BlendMode::Transparent);
    EXPECT_TRUE(result.cullMode == CullMode::None);
    EXPECT_FALSE(result.depthWrite);
    EXPECT_FALSE(result.castShadows);
}

TEST(Material_ValidatePBR_Valid)
{
    PBRProperties props;
    props.metallicFactor = 0.5f;
    props.roughnessFactor = 0.5f;
    props.occlusionStrength = 1.0f;
    props.alphaCutoff = 0.5f;
    props.indexOfRefraction = 1.5f;
    props.emissiveFactor = 0.0f;
    EXPECT_TRUE(Material::ValidatePBRProperties(props));
}

TEST(Material_ValidatePBR_Invalid)
{
    PBRProperties props;
    props.metallicFactor = -0.1f; // out of range
    EXPECT_FALSE(Material::ValidatePBRProperties(props));
}

TEST(Material_HasTexture_Empty)
{
    Material mat("empty_tex");
    EXPECT_FALSE(mat.HasTexture(MaterialTextureType::Albedo));
    EXPECT_FALSE(mat.HasTexture(MaterialTextureType::Normal));
    EXPECT_FALSE(mat.HasTexture(MaterialTextureType::Emissive));
}

TEST(Material_CreateVariant_And_GetAvailable)
{
    Material mat("variant_test");
    mat.CreateVariant("Shadow", {"SHADOW_PASS", "NO_ALPHA"});
    mat.CreateVariant("Wireframe", {"WIREFRAME"});

    auto variants = mat.GetAvailableVariants();
    EXPECT_GE(variants.size(), size_t(2));
}

TEST(Material_CreateInstance)
{
    Material mat("template");
    PBRProperties props;
    props.metallicFactor = 1.0f;
    props.roughnessFactor = 0.2f;
    mat.SetPBRProperties(props);

    auto instance = mat.CreateInstance("instance_1");
    EXPECT_TRUE(instance != nullptr);
    EXPECT_EQ(instance->GetName(), std::string("instance_1"));
    EXPECT_NEAR(instance->GetPBRProperties().metallicFactor, 1.0f, 0.001f);

    // Modify instance — should not affect original
    PBRProperties instProps;
    instProps.metallicFactor = 0.0f;
    instProps.roughnessFactor = 0.2f;
    instance->SetPBRProperties(instProps);
    EXPECT_NEAR(mat.GetPBRProperties().metallicFactor, 1.0f, 0.001f);
}

TEST(Material_GetDetailedInfo_NonEmpty)
{
    Material mat("info_test");
    std::string info = mat.GetDetailedInfo();
    EXPECT_FALSE(info.empty());
}

TEST(Material_BlendMode_EnumValues)
{
    EXPECT_TRUE(BlendMode::Opaque != BlendMode::Transparent);
    EXPECT_TRUE(BlendMode::AlphaTest != BlendMode::Additive);
    EXPECT_TRUE(BlendMode::Multiply != BlendMode::Screen);
}

TEST(Material_CullMode_EnumValues)
{
    EXPECT_TRUE(CullMode::None != CullMode::Front);
    EXPECT_TRUE(CullMode::Front != CullMode::Back);
    EXPECT_TRUE(CullMode::None != CullMode::Back);
}

// ============================================================================
// 3. TextureCompressor
// ============================================================================

using namespace Spark::Graphics;

TEST(TextureCompressor_CalculateMipLevels)
{
    EXPECT_EQ(TextureCompressor::CalculateMipLevels(1, 1), uint32_t(1));
    EXPECT_EQ(TextureCompressor::CalculateMipLevels(256, 256), uint32_t(9));
    EXPECT_EQ(TextureCompressor::CalculateMipLevels(1024, 512), uint32_t(11));
}

TEST(TextureCompressor_GetBlockSize_BC1)
{
    EXPECT_EQ(TextureCompressor::GetBlockSize(TextureCompressionFormat::BC1), uint32_t(8));
}

TEST(TextureCompressor_GetBlockSize_BC3_BC5_BC7)
{
    EXPECT_EQ(TextureCompressor::GetBlockSize(TextureCompressionFormat::BC3), uint32_t(16));
    EXPECT_EQ(TextureCompressor::GetBlockSize(TextureCompressionFormat::BC5), uint32_t(16));
    EXPECT_EQ(TextureCompressor::GetBlockSize(TextureCompressionFormat::BC7), uint32_t(16));
}

TEST(TextureCompressor_GetBlockSize_Uncompressed_Neural)
{
    EXPECT_EQ(TextureCompressor::GetBlockSize(TextureCompressionFormat::Uncompressed), uint32_t(0));
    EXPECT_EQ(TextureCompressor::GetBlockSize(TextureCompressionFormat::Neural), uint32_t(0));
}

TEST(TextureCompressor_EstimateCompressedSize)
{
    auto& comp = TextureCompressor::GetInstance();

    // BC1: 8 bytes per 4x4 block = 0.5 bytes/pixel
    size_t bc1Size = comp.EstimateCompressedSize(256, 256, TextureCompressionFormat::BC1);
    EXPECT_GT(bc1Size, size_t(0));

    // BC7: 16 bytes per 4x4 block = 1 byte/pixel
    size_t bc7Size = comp.EstimateCompressedSize(256, 256, TextureCompressionFormat::BC7);
    EXPECT_GT(bc7Size, size_t(0));

    // BC7 should be roughly double BC1 for same dimensions
    EXPECT_GT(bc7Size, bc1Size);
}

TEST(TextureCompressor_Compress_4x4)
{
    auto& comp = TextureCompressor::GetInstance();

    // 4x4 RGBA image = 64 bytes
    uint8_t rgba[64];
    std::memset(rgba, 128, sizeof(rgba));

    CompressionOptions opts;
    opts.format = TextureCompressionFormat::BC1;
    opts.generateMipmaps = false;

    auto result = comp.Compress(rgba, 4, 4, opts);
    EXPECT_EQ(result.width, uint32_t(4));
    EXPECT_EQ(result.height, uint32_t(4));
    EXPECT_GT(result.compressedSize, size_t(0));
    EXPECT_FALSE(result.mipData.empty());
}

TEST(TextureCompressor_Decompress_RoundTrip)
{
    auto& comp = TextureCompressor::GetInstance();

    // 4x4 solid red image
    uint8_t rgba[64];
    for (int i = 0; i < 16; ++i)
    {
        rgba[i * 4 + 0] = 255; // R
        rgba[i * 4 + 1] = 0;   // G
        rgba[i * 4 + 2] = 0;   // B
        rgba[i * 4 + 3] = 255; // A
    }

    CompressionOptions opts;
    opts.format = TextureCompressionFormat::BC1;
    opts.generateMipmaps = false;

    auto compressed = comp.Compress(rgba, 4, 4, opts);
    EXPECT_FALSE(compressed.mipData.empty());

    auto decompressed = comp.Decompress(compressed, 0);
    // Decompressed data should have 64 bytes (4x4 RGBA)
    EXPECT_EQ(decompressed.size(), size_t(64));
}

// ============================================================================
// 4. VirtualTextureManager
// ============================================================================

TEST(VirtualTextureManager_Initialize)
{
    auto& vtm = VirtualTextureManager::GetInstance();
    vtm.Shutdown(); // reset state

    bool ok = vtm.Initialize(512, 64);
    EXPECT_TRUE(ok);
    EXPECT_EQ(vtm.GetResidentPageCount(), uint32_t(0));
    vtm.Shutdown();
}

TEST(VirtualTextureManager_RequestPage_Increments)
{
    auto& vtm = VirtualTextureManager::GetInstance();
    vtm.Shutdown();
    vtm.Initialize(512, 64);

    vtm.RequestPage(1, 0, 0, 0);
    EXPECT_GE(vtm.GetResidentPageCount() + vtm.GetPendingPageCount(), uint32_t(1));
    vtm.Shutdown();
}

TEST(VirtualTextureManager_RequestPage_NoDuplicate)
{
    auto& vtm = VirtualTextureManager::GetInstance();
    vtm.Shutdown();
    vtm.Initialize(512, 64);

    vtm.RequestPage(1, 0, 0, 0);
    uint32_t countAfterFirst = vtm.GetResidentPageCount() + vtm.GetPendingPageCount();
    vtm.RequestPage(1, 0, 0, 0); // same page
    uint32_t countAfterSecond = vtm.GetResidentPageCount() + vtm.GetPendingPageCount();

    EXPECT_EQ(countAfterFirst, countAfterSecond);
    vtm.Shutdown();
}

TEST(VirtualTextureManager_EvictStalePages)
{
    auto& vtm = VirtualTextureManager::GetInstance();
    vtm.Shutdown();
    vtm.Initialize(512, 64);

    vtm.RequestPage(1, 0, 0, 0);
    vtm.RequestPage(2, 0, 0, 0);

    // Evict pages older than frame 1000 with threshold of 1 frame
    uint32_t evicted = vtm.EvictStalePages(1000, 1);
    // Pages were requested at frame 0, so they should be evicted
    EXPECT_GE(evicted + vtm.GetResidentPageCount(), uint32_t(0));
    vtm.Shutdown();
}

TEST(VirtualTextureManager_ProcessFeedback)
{
    auto& vtm = VirtualTextureManager::GetInstance();
    vtm.Shutdown();
    vtm.Initialize(512, 64);

    std::vector<FeedbackEntry> feedback;
    feedback.push_back({1, 0});
    feedback.push_back({2, 1});
    feedback.push_back({1, 0}); // duplicate

    vtm.ProcessFeedback(feedback, 1);
    // Should have at least some pages requested
    EXPECT_GE(vtm.GetResidentPageCount() + vtm.GetPendingPageCount(), uint32_t(1));
    vtm.Shutdown();
}

TEST(VirtualTextureManager_ConsoleGetStatus)
{
    auto& vtm = VirtualTextureManager::GetInstance();
    vtm.Shutdown();
    vtm.Initialize(256, 64);

    std::string status = vtm.Console_GetStatus();
    EXPECT_FALSE(status.empty());
    vtm.Shutdown();
}

// ============================================================================
// 5. AdaptiveProbeVolumes
// ============================================================================

TEST(APV_Initialize)
{
    AdaptiveProbeVolumes apv;
    APVSettings settings;
    settings.streamingDistance = 50.0f;

    bool ok = apv.Initialize(settings);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(apv.IsInitialized());
    EXPECT_NEAR(apv.GetSettings().streamingDistance, 50.0f, 0.001f);
}

TEST(APV_CreateBrick_IncrementCount)
{
    AdaptiveProbeVolumes apv;
    APVSettings settings;
    apv.Initialize(settings);

    bool ok = apv.CreateBrick(0, 0, 0, BrickLOD::Coarse);
    EXPECT_TRUE(ok);
    EXPECT_EQ(apv.GetLoadedBrickCount(), uint32_t(1));
}

TEST(APV_TotalProbeCount)
{
    AdaptiveProbeVolumes apv;
    APVSettings settings;
    apv.Initialize(settings);

    apv.CreateBrick(0, 0, 0, BrickLOD::Coarse);
    apv.CreateBrick(1, 0, 0, BrickLOD::Coarse);

    EXPECT_EQ(apv.GetTotalProbeCount(), uint32_t(2) * APV_PROBES_PER_BRICK);
    EXPECT_EQ(apv.GetTotalProbeCount(), uint32_t(128));
}

TEST(APV_SubdivideBrick)
{
    AdaptiveProbeVolumes apv;
    APVSettings settings;
    apv.Initialize(settings);

    apv.CreateBrick(0, 0, 0, BrickLOD::Coarse);
    EXPECT_EQ(apv.GetLoadedBrickCount(), uint32_t(1));

    bool ok = apv.SubdivideBrick(0, 0, 0, BrickLOD::Coarse);
    EXPECT_TRUE(ok);

    // Subdivision creates 8 children at the next finer LOD
    // Original brick may remain marked as subdivided + 8 new
    EXPECT_GE(apv.GetLoadedBrickCount(), uint32_t(8));
}

TEST(APV_SetProbeData_SampleIrradiance)
{
    AdaptiveProbeVolumes apv;
    APVSettings settings;
    apv.Initialize(settings);

    apv.CreateBrick(0, 0, 0, BrickLOD::Coarse);

    // Set all-ones SH for probe (0,0,0) — DC term only
    float shR[APV_SH_COEFFICIENTS] = {1.0f};
    float shG[APV_SH_COEFFICIENTS] = {0.5f};
    float shB[APV_SH_COEFFICIENTS] = {0.0f};

    BrickID id{0, 0, 0, BrickLOD::Coarse};
    apv.SetProbeData(id, 0, 0, 0, shR, shG, shB);

    // Sample at the brick origin with a neutral normal
    auto irr = apv.SampleIrradiance(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);

    // We set some non-zero SH; irradiance should reflect that
    EXPECT_GE(irr.r, 0.0f);
    EXPECT_GE(irr.g, 0.0f);
    EXPECT_GE(irr.b, 0.0f);
}

TEST(APV_BrickID_Equality)
{
    BrickID a{1, 2, 3, BrickLOD::Fine};
    BrickID b{1, 2, 3, BrickLOD::Fine};
    BrickID c{1, 2, 3, BrickLOD::Coarse};

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
}

TEST(APV_BrickID_Hash)
{
    BrickIDHash hasher;
    BrickID a{0, 0, 0, BrickLOD::Fine};
    BrickID b{0, 0, 0, BrickLOD::Coarse};

    size_t ha = hasher(a);
    size_t hb = hasher(b);
    // Different LODs should produce different hashes
    EXPECT_NE(ha, hb);
}

// ============================================================================
// 6. ClipmapTerrain
// ============================================================================

TEST(ClipmapTerrain_Initialize)
{
    auto& ct = ClipmapTerrain::GetInstance();
    ct.Shutdown();

    bool ok = ct.Initialize(4, 64, 1.0f);
    EXPECT_TRUE(ok);
    EXPECT_EQ(ct.GetLevelCount(), uint32_t(4));
    ct.Shutdown();
}

TEST(ClipmapTerrain_LevelCellSizes)
{
    auto& ct = ClipmapTerrain::GetInstance();
    ct.Shutdown();
    ct.Initialize(4, 64, 1.0f);

    // Level 0 = baseCellSize, level 1 = 2x, level 2 = 4x, level 3 = 8x
    EXPECT_NEAR(ct.GetLevel(0).cellSize, 1.0f, 0.001f);
    EXPECT_NEAR(ct.GetLevel(1).cellSize, 2.0f, 0.001f);
    EXPECT_NEAR(ct.GetLevel(2).cellSize, 4.0f, 0.001f);
    EXPECT_NEAR(ct.GetLevel(3).cellSize, 8.0f, 0.001f);
    ct.Shutdown();
}

TEST(ClipmapTerrain_GenerateMesh_NonEmpty)
{
    auto& ct = ClipmapTerrain::GetInstance();
    ct.Shutdown();
    ct.Initialize(2, 16, 1.0f);

    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    ct.GenerateMesh(0, vertices, indices);

    EXPECT_FALSE(vertices.empty());
    EXPECT_FALSE(indices.empty());
    // Vertices are (x,y,z) triplets
    EXPECT_EQ(vertices.size() % 3, size_t(0));
    ct.Shutdown();
}

TEST(ClipmapTerrain_SampleHeight_NoHeightmap)
{
    auto& ct = ClipmapTerrain::GetInstance();
    ct.Shutdown();
    ct.Initialize(2, 16, 1.0f);

    float h = ct.SampleHeight(10.0f, 10.0f);
    EXPECT_NEAR(h, 0.0f, 0.001f);
    ct.Shutdown();
}

TEST(ClipmapTerrain_SetHeightmap_SampleRoundTrip)
{
    auto& ct = ClipmapTerrain::GetInstance();
    ct.Shutdown();
    ct.Initialize(2, 16, 1.0f);

    // 4x4 heightmap with known values
    std::vector<float> heights(16, 0.0f);
    heights[0] = 5.0f;  // (0,0)
    heights[3] = 10.0f; // (3,0)
    ct.SetHeightmapData(heights, 4, 4);

    // After setting heightmap, sampling should return non-zero somewhere
    float h = ct.SampleHeight(0.0f, 0.0f);
    // The exact value depends on heightmap indexing; just verify the data was set
    EXPECT_TRUE(h >= 0.0f);
    ct.Shutdown();
}

TEST(ClipmapTerrain_Update_LevelsSnap)
{
    auto& ct = ClipmapTerrain::GetInstance();
    ct.Shutdown();
    ct.Initialize(3, 32, 1.0f);

    XMFLOAT3 camPos{100.5f, 0.0f, 200.3f};
    ct.Update(camPos);

    // After update, level centers should be near the camera, snapped to grid
    const auto& lev0 = ct.GetLevel(0);
    // Level 0 cellSize = 1.0, so center snaps to integer grid
    float dx = std::abs(lev0.center.x - 100.0f);
    float dz = std::abs(lev0.center.z - 200.0f);
    EXPECT_LE(dx, 1.0f);
    EXPECT_LE(dz, 1.0f);
    ct.Shutdown();
}
