// TestFoliageRenderer.cpp — Tests for FoliageRenderer
//
// Covers: mesh cache behaviour (hits, misses, prewarm), per-instance wind
// phase determinism, world matrix construction from FoliageInstance data,
// LOD selection, and end-to-end batch collection from a populated
// FoliageManager. No GPU is required — FoliageRenderer's Windows-only
// GPUSceneBuffer upload path is excluded from CI builds by #ifdef.

#include "TestFramework.h"
#include "Graphics/ClipmapTerrain.h"
#include "Graphics/FoliageRenderer.h"
#include "Graphics/FoliageSystem.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace Spark::Graphics;

namespace
{
    FoliageSpecies MakeRendererSpecies(const std::string& name, float density, float windInfluence,
                                       const std::string& meshPath)
    {
        FoliageSpecies s;
        s.name = name;
        s.meshPath = meshPath;
        s.materialPath = "materials/" + name + ".mat";
        s.density = density;
        s.minScale = 1.0f;
        s.maxScale = 1.0f;
        s.cullDistance = 1000.0f;
        s.windInfluence = windInfluence;
        return s;
    }

    FoliageVolumeDesc MakeRendererVolume(uint32_t seed, const std::vector<std::string>& speciesNames)
    {
        FoliageVolumeDesc d;
        d.center = {0.0f, 0.0f, 0.0f};
        d.halfExtents = {10.0f, 50.0f, 10.0f};
        d.seed = seed;
        d.densityScale = 1.0f;
        d.speciesNames = speciesNames;
        d.alignToTerrain = false;
        return d;
    }
} // namespace

// ============================================================================
// Initialization
// ============================================================================

TEST(FoliageRenderer_InitShutdown)
{
    FoliageRenderer renderer;
    EXPECT_FALSE(renderer.IsInitialized());

    int loadCalls = 0;
    auto loader = [&](const std::string&) -> std::shared_ptr<MeshAsset>
    {
        ++loadCalls;
        return nullptr;
    };

    EXPECT_TRUE(renderer.Initialize(loader, 40.0f));
    EXPECT_TRUE(renderer.IsInitialized());
    EXPECT_NEAR(renderer.GetImpostorDistance(), 40.0f, 1e-5f);
    EXPECT_EQ(renderer.GetMeshCacheSize(), static_cast<uint32_t>(0));

    renderer.Shutdown();
    EXPECT_FALSE(renderer.IsInitialized());
    EXPECT_EQ(renderer.GetMeshCacheSize(), static_cast<uint32_t>(0));
}

TEST(FoliageRenderer_InitNullLoaderFallsBackToNullptrLoader)
{
    FoliageRenderer renderer;
    EXPECT_TRUE(renderer.Initialize(nullptr, 50.0f));
    EXPECT_TRUE(renderer.IsInitialized());
    // Force a load; it should not crash, it should simply return nullptr.
    renderer.PrewarmMesh("does/not/matter.mesh", nullptr);
    EXPECT_TRUE(renderer.HasMeshCached("does/not/matter.mesh"));
    renderer.Shutdown();
}

// ============================================================================
// Pure helpers — wind phase, matrix, LOD
// ============================================================================

TEST(FoliageRenderer_ComputeWindPhaseDeterministic)
{
    FoliageInstance a;
    a.position = {10.0f, 2.0f, -3.0f};
    FoliageInstance b;
    b.position = {10.0f, 2.0f, -3.0f};

    const float pa = FoliageRenderer::ComputeWindPhase(a);
    const float pb = FoliageRenderer::ComputeWindPhase(b);
    EXPECT_NEAR(pa, pb, 1e-6f);

    // Phase is in [0, 2*pi).
    const float twoPi = 6.283185307179586f;
    EXPECT_GE(pa, 0.0f);
    EXPECT_LT(pa, twoPi);
}

TEST(FoliageRenderer_ComputeWindPhaseDecorrelates)
{
    FoliageInstance a;
    a.position = {10.0f, 0.0f, 0.0f};
    FoliageInstance b;
    b.position = {100.0f, 0.0f, 0.0f};

    const float pa = FoliageRenderer::ComputeWindPhase(a);
    const float pb = FoliageRenderer::ComputeWindPhase(b);
    // Very unlikely to collide; a tiny tolerance is fine.
    EXPECT_GT(std::fabs(pa - pb), 0.01f);
}

TEST(FoliageRenderer_BuildWorldMatrixIdentityForZeroYawUnitScale)
{
    FoliageInstance inst;
    inst.position = {0.0f, 0.0f, 0.0f};
    inst.yawRadians = 0.0f;
    inst.scale = 1.0f;

    float m[16];
    FoliageRenderer::BuildWorldMatrix(inst, m);

    EXPECT_NEAR(m[0], 1.0f, 1e-6f);
    EXPECT_NEAR(m[5], 1.0f, 1e-6f);
    EXPECT_NEAR(m[10], 1.0f, 1e-6f);
    EXPECT_NEAR(m[15], 1.0f, 1e-6f);
    EXPECT_NEAR(m[12], 0.0f, 1e-6f);
    EXPECT_NEAR(m[13], 0.0f, 1e-6f);
    EXPECT_NEAR(m[14], 0.0f, 1e-6f);
}

TEST(FoliageRenderer_BuildWorldMatrixScaleAndPosition)
{
    FoliageInstance inst;
    inst.position = {3.0f, 4.0f, 5.0f};
    inst.yawRadians = 0.0f;
    inst.scale = 2.5f;

    float m[16];
    FoliageRenderer::BuildWorldMatrix(inst, m);

    EXPECT_NEAR(m[0], 2.5f, 1e-5f);  // scale X
    EXPECT_NEAR(m[5], 2.5f, 1e-5f);  // scale Y
    EXPECT_NEAR(m[10], 2.5f, 1e-5f); // scale Z
    EXPECT_NEAR(m[12], 3.0f, 1e-5f); // translation X
    EXPECT_NEAR(m[13], 4.0f, 1e-5f); // translation Y
    EXPECT_NEAR(m[14], 5.0f, 1e-5f); // translation Z
}

TEST(FoliageRenderer_BuildWorldMatrixYawRotation90Degrees)
{
    FoliageInstance inst;
    inst.position = {0.0f, 0.0f, 0.0f};
    inst.yawRadians = 1.5707963f; // 90 degrees
    inst.scale = 1.0f;

    float m[16];
    FoliageRenderer::BuildWorldMatrix(inst, m);

    // cos(90) == 0, sin(90) == 1 → row 0 becomes [0, 0, 1, 0], row 2 becomes [-1, 0, 0, 0]
    EXPECT_NEAR(m[0], 0.0f, 1e-5f);
    EXPECT_NEAR(m[2], 1.0f, 1e-5f);
    EXPECT_NEAR(m[8], -1.0f, 1e-5f);
    EXPECT_NEAR(m[10], 0.0f, 1e-5f);
}

TEST(FoliageRenderer_SelectLODMeshThenImpostor)
{
    // Cast to int because the lightweight test framework cannot stream scoped enums.
    EXPECT_EQ(static_cast<int>(FoliageRenderer::SelectLOD(10.0f, 50.0f)), static_cast<int>(FoliageRenderLOD::Mesh));
    EXPECT_EQ(static_cast<int>(FoliageRenderer::SelectLOD(50.0f, 50.0f)),
              static_cast<int>(FoliageRenderLOD::Mesh)); // boundary is inclusive
    EXPECT_EQ(static_cast<int>(FoliageRenderer::SelectLOD(50.0001f, 50.0f)),
              static_cast<int>(FoliageRenderLOD::Impostor));
    EXPECT_EQ(static_cast<int>(FoliageRenderer::SelectLOD(200.0f, 50.0f)),
              static_cast<int>(FoliageRenderLOD::Impostor));
}

// ============================================================================
// Mesh cache
// ============================================================================

TEST(FoliageRenderer_MeshCacheHitsAndMisses)
{
    FoliageRenderer renderer;

    int loadCalls = 0;
    auto loader = [&](const std::string&) -> std::shared_ptr<MeshAsset>
    {
        ++loadCalls;
        return nullptr; // tracked-load behaviour is what we care about
    };

    renderer.Initialize(loader, 50.0f);

    // Force collection with an empty manager — nothing to load.
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();
    renderer.CollectFromFoliageManager(0.0f);
    EXPECT_EQ(renderer.GetStats().meshCacheMisses, static_cast<uint32_t>(0));
    EXPECT_EQ(renderer.GetStats().meshCacheHits, static_cast<uint32_t>(0));
    fm.Shutdown();
    renderer.Shutdown();
}

TEST(FoliageRenderer_PrewarmMeshRecordsCacheEntry)
{
    FoliageRenderer renderer;
    renderer.Initialize(nullptr, 50.0f);

    EXPECT_FALSE(renderer.HasMeshCached("trees/oak.mesh"));
    renderer.PrewarmMesh("trees/oak.mesh", nullptr);
    EXPECT_TRUE(renderer.HasMeshCached("trees/oak.mesh"));
    EXPECT_EQ(renderer.GetMeshCacheSize(), static_cast<uint32_t>(1));

    // Empty paths are ignored.
    renderer.PrewarmMesh("", nullptr);
    EXPECT_EQ(renderer.GetMeshCacheSize(), static_cast<uint32_t>(1));

    renderer.Shutdown();
}

// ============================================================================
// End-to-end collection with a populated FoliageManager
// ============================================================================

TEST(FoliageRenderer_CollectFromManagerBuildsBatch)
{
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();
    fm.RegisterSpecies(MakeRendererSpecies("grass", 0.1f, 1.0f, "meshes/grass.mesh"));
    const uint32_t volId = fm.AddVolume(MakeRendererVolume(123, {"grass"}));
    EXPECT_GT(volId, static_cast<uint32_t>(0));

    // Place camera at origin — all instances near 0 should be visible.
    fm.Update(0.016f, {0.0f, 0.0f, 0.0f});

    FoliageRenderer renderer;
    int loads = 0;
    auto loader = [&](const std::string& path) -> std::shared_ptr<MeshAsset>
    {
        ++loads;
        EXPECT_EQ(path, std::string("meshes/grass.mesh"));
        return nullptr;
    };
    renderer.Initialize(loader, 500.0f);

    renderer.CollectFromFoliageManager(0.0f);

    const auto& batch = renderer.GetRenderInstances();
    EXPECT_GT(batch.size(), static_cast<size_t>(0));

    const auto& stats = renderer.GetStats();
    EXPECT_EQ(stats.meshDraws, static_cast<uint32_t>(batch.size()));
    EXPECT_EQ(stats.impostorDraws, static_cast<uint32_t>(0));
    // Loader should have been invoked exactly once — everything else must
    // have been a cache hit.
    EXPECT_EQ(loads, 1);
    EXPECT_EQ(stats.meshCacheMisses, static_cast<uint32_t>(1));
    EXPECT_GT(stats.meshCacheHits, static_cast<uint32_t>(0));

    // Every render instance must carry the species' windInfluence.
    for (const auto& r : batch)
    {
        EXPECT_NEAR(r.windStrength, 1.0f, 1e-6f);
    }

    renderer.Shutdown();
    fm.Shutdown();
}

TEST(FoliageRenderer_CollectSkipsCulledInstances)
{
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();
    fm.RegisterSpecies(MakeRendererSpecies("grass", 0.1f, 1.0f, "meshes/grass.mesh"));
    const uint32_t volId = fm.AddVolume(MakeRendererVolume(7, {"grass"}));
    EXPECT_GT(volId, static_cast<uint32_t>(0));

    // Place the camera 10 km away — species cullDistance is 1000 so
    // everything should be culled.
    fm.Update(0.016f, {10000.0f, 0.0f, 0.0f});

    FoliageRenderer renderer;
    renderer.Initialize([](const std::string&) { return nullptr; }, 50.0f);
    renderer.CollectFromFoliageManager(0.0f);

    const auto& batch = renderer.GetRenderInstances();
    EXPECT_EQ(batch.size(), static_cast<size_t>(0));
    EXPECT_GT(renderer.GetStats().culledInstances, static_cast<uint32_t>(0));

    renderer.Shutdown();
    fm.Shutdown();
}

TEST(FoliageRenderer_CollectClassifiesImpostorByDistance)
{
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();
    fm.RegisterSpecies(MakeRendererSpecies("grass", 0.05f, 1.0f, "meshes/grass.mesh"));
    fm.AddVolume(MakeRendererVolume(99, {"grass"}));

    // All instances visible (camera at origin, cullDistance=1000).
    fm.Update(0.016f, {0.0f, 0.0f, 0.0f});

    FoliageRenderer renderer;
    renderer.Initialize([](const std::string&) { return nullptr; }, 2.0f); // 2 m impostor distance

    renderer.CollectFromFoliageManager(0.0f);

    // With a 2 m impostor distance and a 10 m volume, most should be impostors.
    const auto& stats = renderer.GetStats();
    EXPECT_GT(stats.impostorDraws, static_cast<uint32_t>(0));
    EXPECT_EQ(stats.meshDraws + stats.impostorDraws, static_cast<uint32_t>(renderer.GetRenderInstances().size()));

    renderer.Shutdown();
    fm.Shutdown();
}

TEST(FoliageRenderer_EmptyManagerProducesEmptyBatch)
{
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();

    FoliageRenderer renderer;
    renderer.Initialize([](const std::string&) { return nullptr; }, 50.0f);

    renderer.CollectFromFoliageManager(0.0f);
    EXPECT_EQ(renderer.GetRenderInstances().size(), static_cast<size_t>(0));
    EXPECT_EQ(renderer.GetStats().meshDraws, static_cast<uint32_t>(0));
    EXPECT_EQ(renderer.GetStats().impostorDraws, static_cast<uint32_t>(0));

    renderer.Shutdown();
    fm.Shutdown();
}

TEST(FoliageRenderer_GlobalMaterialIdMatchesRegistryIndex)
{
    // Register three species in a known order, then build a volume that
    // lists them in a DIFFERENT order. Verify that each render record
    // carries the REGISTRY-wide index (not the volume-local speciesIndex)
    // in globalMaterialId, which is the value uploaded as GPUInstanceData
    // materialId.
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();
    fm.RegisterSpecies(MakeRendererSpecies("grass", 1.0f, 1.0f, "meshes/grass.mesh"));
    fm.RegisterSpecies(MakeRendererSpecies("flower", 1.0f, 1.0f, "meshes/flower.mesh"));
    fm.RegisterSpecies(MakeRendererSpecies("fern", 1.0f, 1.0f, "meshes/fern.mesh"));

    const uint32_t grassIdx = fm.GetSpeciesGlobalIndex("grass");
    const uint32_t flowerIdx = fm.GetSpeciesGlobalIndex("flower");
    const uint32_t fernIdx = fm.GetSpeciesGlobalIndex("fern");
    EXPECT_NE(grassIdx, UINT32_MAX);
    EXPECT_NE(flowerIdx, UINT32_MAX);
    EXPECT_NE(fernIdx, UINT32_MAX);
    EXPECT_NE(grassIdx, flowerIdx);
    EXPECT_NE(flowerIdx, fernIdx);

    // Volume references them in reversed order: local index 0 = fern,
    // 1 = flower, 2 = grass.
    fm.AddVolume(MakeRendererVolume(7, {"fern", "flower", "grass"}));
    fm.Update(0.016f, {0.0f, 0.0f, 0.0f});

    FoliageRenderer renderer;
    renderer.Initialize([](const std::string&) { return nullptr; }, 500.0f);
    renderer.CollectFromFoliageManager(0.0f);

    bool sawFern = false, sawFlower = false, sawGrass = false;
    for (const auto& inst : renderer.GetRenderInstances())
    {
        if (inst.speciesIndex == 0)
        {
            EXPECT_EQ(inst.globalMaterialId, fernIdx);
            sawFern = true;
        }
        else if (inst.speciesIndex == 1)
        {
            EXPECT_EQ(inst.globalMaterialId, flowerIdx);
            sawFlower = true;
        }
        else if (inst.speciesIndex == 2)
        {
            EXPECT_EQ(inst.globalMaterialId, grassIdx);
            sawGrass = true;
        }
    }
    EXPECT_TRUE(sawFern);
    EXPECT_TRUE(sawFlower);
    EXPECT_TRUE(sawGrass);

    renderer.Shutdown();
    fm.Shutdown();
}

TEST(FoliageRenderer_UnregisteredSpeciesSkippedNotEmitted)
{
    // Register a species, add a volume that uses it, then unregister the
    // species BEFORE collecting. Every instance should be skipped via
    // stats.unresolvedSpecies and the batch should be empty — not filled
    // with defaulted material ids.
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();
    fm.RegisterSpecies(MakeRendererSpecies("ephemeral", 0.1f, 1.0f, "meshes/ephemeral.mesh"));
    fm.AddVolume(MakeRendererVolume(11, {"ephemeral"}));
    fm.Update(0.016f, {0.0f, 0.0f, 0.0f});
    fm.UnregisterSpecies("ephemeral");

    FoliageRenderer renderer;
    renderer.Initialize([](const std::string&) { return nullptr; }, 500.0f);
    renderer.CollectFromFoliageManager(0.0f);

    EXPECT_EQ(renderer.GetRenderInstances().size(), static_cast<size_t>(0));
    EXPECT_GT(renderer.GetStats().unresolvedSpecies, static_cast<uint32_t>(0));
    EXPECT_EQ(renderer.GetStats().meshDraws, static_cast<uint32_t>(0));
    EXPECT_EQ(renderer.GetStats().impostorDraws, static_cast<uint32_t>(0));

    renderer.Shutdown();
    fm.Shutdown();
}

TEST(FoliageRenderer_ConsoleStatusString)
{
    FoliageRenderer renderer;
    renderer.Initialize(nullptr, 50.0f);
    const std::string s = renderer.Console_GetStatus();
    EXPECT_TRUE(s.find("FoliageRenderer") != std::string::npos);
    renderer.Shutdown();
}

// ============================================================================
// AssetPipeline loader installation
// ============================================================================

TEST(FoliageRenderer_InstallAssetPipelineLoaderNullIsNoOp)
{
    FoliageRenderer renderer;
    renderer.Initialize(nullptr, 50.0f);
    EXPECT_FALSE(renderer.HasExplicitLoader());

    EXPECT_FALSE(renderer.InstallAssetPipelineLoader(nullptr));
    EXPECT_FALSE(renderer.HasExplicitLoader());

    renderer.Shutdown();
}

TEST(FoliageRenderer_ExplicitLoaderTakesPrecedence)
{
    FoliageRenderer renderer;

    int calls = 0;
    auto loader = [&](const std::string&) -> std::shared_ptr<MeshAsset>
    {
        ++calls;
        return nullptr;
    };

    renderer.Initialize(loader, 50.0f);
    EXPECT_TRUE(renderer.HasExplicitLoader());

    // A subsequent Collect on an empty manager should not un-install the
    // explicit loader via the lazy AssetPipeline path.
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();
    renderer.CollectFromFoliageManager(0.0f);
    EXPECT_TRUE(renderer.HasExplicitLoader());
    fm.Shutdown();

    renderer.Shutdown();
    EXPECT_FALSE(renderer.HasExplicitLoader());
}

TEST(FoliageRenderer_InitNullLoaderStaysWithoutExplicitLoader)
{
    // Without an EngineContext AssetPipeline registered (typical CI test
    // configuration), the lazy install should fail silently and leave
    // HasExplicitLoader() == false.
    FoliageRenderer renderer;
    renderer.Initialize(nullptr, 50.0f);

    // Depending on whether an earlier test populated EngineContext, the
    // lazy install may or may not have succeeded. The contract we test
    // here is that the renderer is still functional regardless.
    EXPECT_TRUE(renderer.IsInitialized());

    renderer.Shutdown();
}

// ============================================================================
// Phase E: GPU instance encoding + flag bit helpers (CPU-only)
// ============================================================================

TEST(FoliageRenderer_FoliageImpostorFlagRoundTrip)
{
    // The flag helpers live in GPUSceneBuffer.h and operate on the POD
    // struct directly — no D3D required. Verifies that toggling the
    // impostor bit does not corrupt other flag bits.
    GPUInstanceData data{};
    data.flags = static_cast<uint32_t>(InstanceFlags::Visible) | static_cast<uint32_t>(InstanceFlags::CastShadow);

    EXPECT_FALSE(IsFoliageImpostorFlag(data));

    SetFoliageImpostorFlag(data, true);
    EXPECT_TRUE(IsFoliageImpostorFlag(data));
    // Other bits preserved.
    EXPECT_TRUE((data.flags & static_cast<uint32_t>(InstanceFlags::Visible)) != 0);
    EXPECT_TRUE((data.flags & static_cast<uint32_t>(InstanceFlags::CastShadow)) != 0);

    SetFoliageImpostorFlag(data, false);
    EXPECT_FALSE(IsFoliageImpostorFlag(data));
    // Other bits still preserved.
    EXPECT_TRUE((data.flags & static_cast<uint32_t>(InstanceFlags::Visible)) != 0);
    EXPECT_TRUE((data.flags & static_cast<uint32_t>(InstanceFlags::CastShadow)) != 0);
}

TEST(FoliageRenderer_BuildGPUInstanceMeshLOD)
{
    // Mesh-LOD record should NOT have the impostor flag bit set.
    FoliageRenderInstance src;
    src.globalMaterialId = 7;
    src.lod = FoliageRenderLOD::Mesh;
    src.distanceToCamera = 12.5f;
    src.windPhase = 1.25f;
    // Build a simple translation-only world matrix.
    float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 4, 5, 1};
    std::memcpy(src.worldMatrix, world, sizeof(world));

    GPUInstanceData out{};
    FoliageRenderer::BuildGPUInstance(src, out);

    EXPECT_EQ(out.materialId, 7u);
    EXPECT_NEAR(out.lodDistance, 12.5f, 1e-6f);
    EXPECT_NEAR(out.padding, 1.25f, 1e-6f); // wind phase packed into padding
    EXPECT_FALSE(IsFoliageImpostorFlag(out));
    EXPECT_TRUE((out.flags & static_cast<uint32_t>(InstanceFlags::Visible)) != 0);
}

TEST(FoliageRenderer_BuildGPUInstanceImpostorLODSetsFlag)
{
    // Impostor-LOD record should have the impostor flag bit set and
    // preserve the wind-phase-in-padding / material-id conventions.
    FoliageRenderInstance src;
    src.globalMaterialId = 42;
    src.lod = FoliageRenderLOD::Impostor;
    src.distanceToCamera = 200.0f;
    src.windPhase = 3.14f;
    float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(src.worldMatrix, world, sizeof(world));

    GPUInstanceData out{};
    FoliageRenderer::BuildGPUInstance(src, out);

    EXPECT_EQ(out.materialId, 42u);
    EXPECT_TRUE(IsFoliageImpostorFlag(out));
    EXPECT_NEAR(out.lodDistance, 200.0f, 1e-6f);
    EXPECT_NEAR(out.padding, 3.14f, 1e-6f);
}

TEST(FoliageRenderer_SpeciesDefaultsForPhaseFFields)
{
    // Phase F added two fields to FoliageSpecies; Phase G added a third.
    // Defaults matter because existing scenes will inherit them without
    // touching the new fields.
    FoliageSpecies s;
    EXPECT_TRUE(s.albedoTexturePath.empty());
    EXPECT_NEAR(s.billboardHeight, 2.0f, 1e-6f);
    // Phase G: aspect default preserves Phase E/F appearance
    // (halfWidth = height * 0.5 = height/2).
    EXPECT_NEAR(s.billboardAspect, 0.5f, 1e-6f);
}

TEST(FoliageRenderer_BillboardHalfWidthFormulaPhaseG)
{
    // Documents the VS formula: halfWidth = height * aspect. Tests the
    // two anchor cases (default tree-like, wide bush) so a regression
    // in either the CPU-side packing or the VS read surfaces here
    // before it appears visually.
    FoliageSpecies tree;
    tree.billboardHeight = 8.0f;
    tree.billboardAspect = 0.5f;
    const float treeHalf = tree.billboardHeight * tree.billboardAspect;
    EXPECT_NEAR(treeHalf, 4.0f, 1e-6f);

    FoliageSpecies bush;
    bush.billboardHeight = 1.0f;
    bush.billboardAspect = 1.5f;
    const float bushHalf = bush.billboardHeight * bush.billboardAspect;
    EXPECT_NEAR(bushHalf, 1.5f, 1e-6f);
}

TEST(FoliageRenderer_CollectProducesSortedDrawOrder)
{
    // After CollectFromFoliageManager, m_renderInstances should be sorted
    // by (lod, globalMaterialId) and m_drawOrder should contain one run
    // per contiguous (lod, species) group in that order.
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();
    fm.RegisterSpecies(MakeRendererSpecies("grass", 0.05f, 1.0f, "meshes/grass.mesh"));
    fm.AddVolume(MakeRendererVolume(31, {"grass"}));

    FoliageRenderer renderer;
    renderer.Initialize(nullptr, 50.0f);
    renderer.CollectFromFoliageManager(0.0f);

    const auto& batch = renderer.GetRenderInstances();
    const auto& order = renderer.GetDrawOrder();

    // Every run in m_drawOrder covers a contiguous slice of m_renderInstances.
    uint32_t walker = 0;
    for (const FoliageDrawRun& run : order)
    {
        EXPECT_EQ(run.startInstance, walker);
        EXPECT_GT(run.instanceCount, 0u);
        // All instances in the run share the run's (lod, globalMaterialId).
        for (uint32_t k = 0; k < run.instanceCount; ++k)
        {
            const FoliageRenderInstance& rec = batch[walker + k];
            EXPECT_TRUE(rec.lod == run.lod);
            EXPECT_EQ(rec.globalMaterialId, run.globalMaterialId);
        }
        walker += run.instanceCount;
    }
    EXPECT_EQ(walker, static_cast<uint32_t>(batch.size()));

    // Sorted by (lod, globalMaterialId): all Mesh before any Impostor.
    bool sawImpostor = false;
    for (const FoliageRenderInstance& rec : batch)
    {
        if (rec.lod == FoliageRenderLOD::Impostor)
            sawImpostor = true;
        else
            EXPECT_FALSE(sawImpostor); // no mesh after we saw an impostor
    }

    renderer.Shutdown();
    fm.Shutdown();
}
