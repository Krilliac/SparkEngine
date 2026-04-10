// TestFoliageSystem.cpp — Tests for the CPU side of FoliageManager
//
// These tests exercise the deterministic scatter, density, slope/altitude
// filtering, volume lifecycle, species registry, distance culling, and the
// ECS path. They run headless (no GPU) — ClipmapTerrain is initialized
// with a flat heightmap so slope filtering is predictable.

#include "TestFramework.h"
#include "Graphics/FoliageSystem.h"
#include "Graphics/ClipmapTerrain.h"
#include "Engine/ECS/Components.h"

#include <cmath>
#include <vector>

using namespace Spark::Graphics;

namespace
{
    FoliageSpecies MakeSpecies(const std::string& name, float density, float cullDistance = 1000.0f)
    {
        FoliageSpecies s;
        s.name = name;
        s.meshPath = "meshes/" + name + ".mesh";
        s.materialPath = "materials/" + name + ".mat";
        s.density = density;
        s.minScale = 1.0f;
        s.maxScale = 1.0f;
        s.minSlopeAngleDeg = 0.0f;
        s.maxSlopeAngleDeg = 90.0f;
        s.minAltitude = -10000.0f;
        s.maxAltitude = 10000.0f;
        s.cullDistance = cullDistance;
        s.windInfluence = 1.0f;
        return s;
    }

    FoliageVolumeDesc MakeVolume(float halfX, float halfZ, uint32_t seed, const std::string& speciesName)
    {
        FoliageVolumeDesc d;
        d.center = {0.0f, 0.0f, 0.0f};
        d.halfExtents = {halfX, 50.0f, halfZ};
        d.seed = seed;
        d.densityScale = 1.0f;
        d.speciesNames = {speciesName};
        d.alignToTerrain = false; // keep Y at center.y for predictable tests
        return d;
    }
} // namespace

TEST(FoliageSystem_InitAndShutdown)
{
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();
    EXPECT_TRUE(fm.IsInitialized());
    EXPECT_EQ(fm.GetVolumeCount(), static_cast<uint32_t>(0));
    EXPECT_EQ(fm.GetSpeciesCount(), static_cast<uint32_t>(0));
    EXPECT_EQ(fm.GetTotalInstanceCount(), static_cast<uint32_t>(0));
    fm.Shutdown();
    EXPECT_FALSE(fm.IsInitialized());
}

TEST(FoliageSystem_RegisterAndFindSpecies)
{
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();

    fm.RegisterSpecies(MakeSpecies("oak", 0.5f));
    fm.RegisterSpecies(MakeSpecies("pine", 0.25f));
    EXPECT_EQ(fm.GetSpeciesCount(), static_cast<uint32_t>(2));

    const auto* oak = fm.FindSpecies("oak");
    EXPECT_NE(oak, nullptr);
    EXPECT_NEAR(oak->density, 0.5f, 1e-6f);

    const auto* missing = fm.FindSpecies("willow");
    EXPECT_EQ(missing, nullptr);

    // Re-registering the same name should overwrite, not duplicate.
    fm.RegisterSpecies(MakeSpecies("oak", 0.75f));
    EXPECT_EQ(fm.GetSpeciesCount(), static_cast<uint32_t>(2));
    EXPECT_NEAR(fm.FindSpecies("oak")->density, 0.75f, 1e-6f);

    fm.UnregisterSpecies("oak");
    EXPECT_EQ(fm.GetSpeciesCount(), static_cast<uint32_t>(1));
    EXPECT_EQ(fm.FindSpecies("oak"), nullptr);
    EXPECT_NE(fm.FindSpecies("pine"), nullptr);

    fm.Shutdown();
}

TEST(FoliageSystem_AddVolumeGeneratesInstances)
{
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();

    // density 0.1 over a 10x10 volume = 0.1 * 400 = 40 instances
    fm.RegisterSpecies(MakeSpecies("grass", 0.1f));

    const uint32_t volId = fm.AddVolume(MakeVolume(10.0f, 10.0f, 42, "grass"));
    EXPECT_GT(volId, static_cast<uint32_t>(0));
    EXPECT_EQ(fm.GetVolumeCount(), static_cast<uint32_t>(1));

    const auto& instances = fm.GetInstances(volId);
    EXPECT_EQ(instances.size(), static_cast<size_t>(40));
    EXPECT_EQ(fm.GetTotalInstanceCount(), static_cast<uint32_t>(40));

    // All instances should fall inside the volume AABB.
    for (const auto& inst : instances)
    {
        EXPECT_GE(inst.position.x, -10.0f);
        EXPECT_LE(inst.position.x, 10.0f);
        EXPECT_GE(inst.position.z, -10.0f);
        EXPECT_LE(inst.position.z, 10.0f);
    }
    fm.Shutdown();
}

TEST(FoliageSystem_DeterministicScatter)
{
    // Same seed must produce byte-identical instance lists.
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();
    fm.RegisterSpecies(MakeSpecies("tree", 0.2f));

    const uint32_t id1 = fm.AddVolume(MakeVolume(5.0f, 5.0f, 1337, "tree"));
    std::vector<FoliageInstance> first = fm.GetInstances(id1);
    fm.RemoveVolume(id1);

    const uint32_t id2 = fm.AddVolume(MakeVolume(5.0f, 5.0f, 1337, "tree"));
    std::vector<FoliageInstance> second = fm.GetInstances(id2);

    EXPECT_EQ(first.size(), second.size());
    // Note: id1 != id2 so MixSeed produces different RNG streams — that's
    // intentional (two volumes with the same seed should not overlap).
    // What we CAN guarantee: the same volume ID + same seed produces the
    // same result. Verified by restarting the manager fresh below.
    fm.Shutdown();

    fm.Initialize();
    fm.RegisterSpecies(MakeSpecies("tree", 0.2f));
    const uint32_t idA = fm.AddVolume(MakeVolume(5.0f, 5.0f, 1337, "tree"));
    std::vector<FoliageInstance> runA = fm.GetInstances(idA);
    fm.Shutdown();

    fm.Initialize();
    fm.RegisterSpecies(MakeSpecies("tree", 0.2f));
    const uint32_t idB = fm.AddVolume(MakeVolume(5.0f, 5.0f, 1337, "tree"));
    std::vector<FoliageInstance> runB = fm.GetInstances(idB);

    EXPECT_EQ(idA, idB); // Both runs start the ID counter at 1.
    EXPECT_EQ(runA.size(), runB.size());
    for (size_t i = 0; i < runA.size(); ++i)
    {
        EXPECT_NEAR(runA[i].position.x, runB[i].position.x, 1e-6f);
        EXPECT_NEAR(runA[i].position.z, runB[i].position.z, 1e-6f);
        EXPECT_NEAR(runA[i].yawRadians, runB[i].yawRadians, 1e-6f);
    }
    fm.Shutdown();
}

TEST(FoliageSystem_AltitudeFilter)
{
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();

    // Species with a narrow altitude window that excludes Y = 0.
    FoliageSpecies s = MakeSpecies("alpine", 0.1f);
    s.minAltitude = 100.0f;
    s.maxAltitude = 200.0f;
    fm.RegisterSpecies(s);

    // Volume center at Y=0 with alignToTerrain=false, so every instance
    // inherits Y=0 and fails the 100..200 altitude filter.
    FoliageVolumeDesc d = MakeVolume(10.0f, 10.0f, 7, "alpine");
    d.center = {0.0f, 0.0f, 0.0f};
    const uint32_t volId = fm.AddVolume(d);
    EXPECT_GT(volId, static_cast<uint32_t>(0));
    EXPECT_EQ(fm.GetInstances(volId).size(), static_cast<size_t>(0));

    // Lift the volume into the window and all instances should pass.
    fm.RemoveVolume(volId);
    d.center = {0.0f, 150.0f, 0.0f};
    const uint32_t volId2 = fm.AddVolume(d);
    EXPECT_EQ(fm.GetInstances(volId2).size(), static_cast<size_t>(40));

    fm.Shutdown();
}

TEST(FoliageSystem_SlopeFilterOnFlatTerrain)
{
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();

    // Init clipmap terrain with a flat heightmap — slope should be 0
    // everywhere, so a species requiring slope > 30° should place nothing.
    auto& terrain = Spark::Graphics::ClipmapTerrain::GetInstance();
    terrain.Initialize();
    std::vector<float> flat(16 * 16, 5.0f);
    terrain.SetHeightmapData(flat, 16, 16);

    FoliageSpecies s = MakeSpecies("cliffshrub", 0.1f);
    s.minSlopeAngleDeg = 30.0f;
    s.maxSlopeAngleDeg = 90.0f;
    fm.RegisterSpecies(s);

    FoliageVolumeDesc d = MakeVolume(10.0f, 10.0f, 11, "cliffshrub");
    d.alignToTerrain = true; // sample terrain for Y and normal
    const uint32_t volId = fm.AddVolume(d);
    EXPECT_GT(volId, static_cast<uint32_t>(0));
    EXPECT_EQ(fm.GetInstances(volId).size(), static_cast<size_t>(0));

    // A species that accepts 0–5° should place everything.
    FoliageSpecies s2 = MakeSpecies("flatgrass", 0.1f);
    s2.minSlopeAngleDeg = 0.0f;
    s2.maxSlopeAngleDeg = 5.0f;
    fm.RegisterSpecies(s2);

    FoliageVolumeDesc d2 = MakeVolume(10.0f, 10.0f, 13, "flatgrass");
    d2.alignToTerrain = true;
    const uint32_t volId2 = fm.AddVolume(d2);
    EXPECT_EQ(fm.GetInstances(volId2).size(), static_cast<size_t>(40));

    terrain.Shutdown();
    fm.Shutdown();
}

TEST(FoliageSystem_DistanceCullingMarksVisibility)
{
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();

    // Cull distance 5 — only instances within 5m of origin should stay visible.
    fm.RegisterSpecies(MakeSpecies("moss", 0.5f, 5.0f));
    const uint32_t volId = fm.AddVolume(MakeVolume(20.0f, 20.0f, 99, "moss"));
    const size_t total = fm.GetInstances(volId).size();
    EXPECT_GT(total, static_cast<size_t>(0));

    // Cull against the origin.
    fm.Update(1.0f / 60.0f, XMFLOAT3{0.0f, 0.0f, 0.0f});
    uint32_t visible = fm.GetVisibleInstanceCount();
    EXPECT_LE(visible, total);
    // Some must remain visible (a 5m cull on a 40x40 volume always has hits).
    EXPECT_GT(visible, static_cast<uint32_t>(0));

    // Cull against a far-away point → zero visible.
    fm.Update(1.0f / 60.0f, XMFLOAT3{10000.0f, 0.0f, 10000.0f});
    EXPECT_EQ(fm.GetVisibleInstanceCount(), static_cast<uint32_t>(0));

    fm.Shutdown();
}

TEST(FoliageSystem_RemoveVolumeClearsInstances)
{
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();
    fm.RegisterSpecies(MakeSpecies("bush", 0.1f));

    const uint32_t v1 = fm.AddVolume(MakeVolume(5.0f, 5.0f, 1, "bush"));
    const uint32_t v2 = fm.AddVolume(MakeVolume(5.0f, 5.0f, 2, "bush"));
    EXPECT_EQ(fm.GetVolumeCount(), static_cast<uint32_t>(2));
    const uint32_t totalBefore = fm.GetTotalInstanceCount();
    EXPECT_GT(totalBefore, static_cast<uint32_t>(0));

    fm.RemoveVolume(v1);
    EXPECT_FALSE(fm.HasVolume(v1));
    EXPECT_TRUE(fm.HasVolume(v2));
    EXPECT_EQ(fm.GetVolumeCount(), static_cast<uint32_t>(1));
    EXPECT_LT(fm.GetTotalInstanceCount(), totalBefore);

    // Removing an unknown ID is a safe no-op.
    fm.RemoveVolume(9999);
    EXPECT_EQ(fm.GetVolumeCount(), static_cast<uint32_t>(1));

    fm.Shutdown();
}

TEST(FoliageSystem_AddVolumeRejectsEmptySpeciesList)
{
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();
    fm.RegisterSpecies(MakeSpecies("fern", 0.1f));

    FoliageVolumeDesc desc;
    desc.center = {0.0f, 0.0f, 0.0f};
    desc.halfExtents = {10.0f, 10.0f, 10.0f};
    desc.seed = 123;
    desc.alignToTerrain = false;
    // speciesNames left empty
    EXPECT_EQ(fm.AddVolume(desc), static_cast<uint32_t>(0));
    EXPECT_EQ(fm.GetVolumeCount(), static_cast<uint32_t>(0));

    fm.Shutdown();
}

TEST(FoliageSystem_ECSIntegration)
{
    auto& fm = FoliageManager::GetInstance();
    fm.Initialize();
    fm.RegisterSpecies(MakeSpecies("clover", 0.2f));

    World world;
    auto entity = world.CreateEntity();
    Transform xform{};
    xform.position = {0.0f, 0.0f, 0.0f};
    world.AddComponent<Transform>(entity, xform);

    FoliageVolumeComponent comp{};
    comp.halfExtents = {5.0f, 50.0f, 5.0f};
    comp.seed = 77;
    comp.densityScale = 1.0f;
    comp.minSlopeAngle = 0.0f;
    comp.maxSlopeAngle = 90.0f;
    comp.minAltitude = -10000.0f;
    comp.maxAltitude = 10000.0f;
    comp.alignToSurface = false;
    comp.enabled = true;
    comp.speciesNames = {"clover"};
    world.AddComponent<FoliageVolumeComponent>(entity, comp);

    // First pass: the component gets a runtime volume ID.
    fm.UpdateFromECS(world, 1.0f / 60.0f);
    auto* stored = world.GetComponent<FoliageVolumeComponent>(entity);
    EXPECT_NE(stored, nullptr);
    EXPECT_GT(stored->runtimeVolumeId, static_cast<uint32_t>(0));
    EXPECT_EQ(fm.GetVolumeCount(), static_cast<uint32_t>(1));

    const uint32_t assignedId = stored->runtimeVolumeId;
    // Second pass: should not create a duplicate volume.
    fm.UpdateFromECS(world, 1.0f / 60.0f);
    EXPECT_EQ(fm.GetVolumeCount(), static_cast<uint32_t>(1));
    EXPECT_EQ(stored->runtimeVolumeId, assignedId);

    // Disabling the component removes the volume.
    stored->enabled = false;
    fm.UpdateFromECS(world, 1.0f / 60.0f);
    EXPECT_EQ(fm.GetVolumeCount(), static_cast<uint32_t>(0));
    EXPECT_EQ(stored->runtimeVolumeId, static_cast<uint32_t>(0));

    fm.Shutdown();
}
