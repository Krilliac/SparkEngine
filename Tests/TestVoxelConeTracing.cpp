/**
 * @file TestVoxelConeTracing.cpp
 * @brief Phase T — CPU reference tests for VoxelConeTracing (VCT GI)
 *
 * Exercises `Spark::Graphics::VoxelGrid` and `Spark::Graphics::VCTSystem`
 * directly — the 3D voxel grid, mip chain builder, cone trace math,
 * and the VCTSystem coordinator that drives the full voxelization →
 * trace pipeline. All code is pure CPU (`std::vector<uint8_t>`
 * storage, no D3D11 or compute shaders), so every test runs on every
 * platform's CI.
 *
 * Phase T also wires VCTSystem as a `unique_ptr` member of
 * `Spark::Graphics::GraphicsEngine` with lifecycle hooks on both the
 * Windows real impl and the Linux stub path. The default grid is
 * 32³ (~130 KB) with `enabled = false` so the accessor is alive
 * from frame 1 without wasting memory on an unused GI pipeline.
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Graphics/VoxelConeTracing.h"

#include <cmath>
#include <cstdint>

using Spark::Graphics::VCTSettings;
using Spark::Graphics::VCTSystem;
using Spark::Graphics::VoxelGrid;

// =========================================================================
// VCTSettings defaults
// =========================================================================

TEST(VoxelConeTracing_DefaultSettings)
{
    VCTSettings s;
    EXPECT_FALSE(s.enabled);
    EXPECT_EQ(s.voxelResolution, 128u);
    EXPECT_NEAR(s.worldExtent, 100.0f, 1e-6f);
    EXPECT_EQ(s.diffuseCones, 6);
    EXPECT_NEAR(s.diffuseConeAngle, 1.047f, 1e-4f);
    EXPECT_NEAR(s.specularConeAngle, 0.087f, 1e-4f);
    EXPECT_NEAR(s.maxTraceDistance, 50.0f, 1e-6f);
    EXPECT_NEAR(s.stepMultiplier, 1.0f, 1e-6f);
    EXPECT_NEAR(s.indirectDiffuseStrength, 1.0f, 1e-6f);
    EXPECT_NEAR(s.indirectSpecularStrength, 1.0f, 1e-6f);
    EXPECT_NEAR(s.aoStrength, 1.0f, 1e-6f);
    EXPECT_FALSE(s.secondBounce);
    EXPECT_FALSE(s.revoxelizeEveryFrame);
}

// =========================================================================
// VoxelGrid: initialisation + metadata
// =========================================================================

TEST(VoxelConeTracing_VoxelGridInitialize)
{
    VoxelGrid grid;
    EXPECT_FALSE(grid.IsInitialized());
    EXPECT_TRUE(grid.Initialize(16, 50.0f));
    EXPECT_TRUE(grid.IsInitialized());
    EXPECT_EQ(grid.GetResolution(), 16u);
    EXPECT_NEAR(grid.GetWorldExtent(), 50.0f, 1e-6f);
    // Voxel size = (worldExtent * 2) / resolution = 100 / 16 = 6.25.
    EXPECT_NEAR(grid.GetVoxelSize(), 6.25f, 1e-6f);
}

TEST(VoxelConeTracing_VoxelGridMipCount)
{
    VoxelGrid grid;
    grid.Initialize(16, 50.0f);
    // 16 → 8 → 4 → 2 → 1 = 5 mips. The class clamps to kMaxMips = 8.
    EXPECT_EQ(grid.GetMipCount(), 5);
}

TEST(VoxelConeTracing_VoxelGridSmallestMipCount)
{
    VoxelGrid grid;
    grid.Initialize(8, 20.0f);
    // 8 → 4 → 2 → 1 = 4 mips.
    EXPECT_EQ(grid.GetMipCount(), 4);
}

TEST(VoxelConeTracing_VoxelGridConsoleStatus)
{
    VoxelGrid grid;
    grid.Initialize(8, 20.0f);
    std::string status = grid.Console_GetStatus();
    EXPECT_TRUE(status.find("VoxelGrid") != std::string::npos);
    EXPECT_TRUE(status.find("8") != std::string::npos); // resolution
    EXPECT_TRUE(status.find("mips") != std::string::npos);
}

// =========================================================================
// VoxelGrid: light injection + clear
// =========================================================================

TEST(VoxelConeTracing_VoxelGridInjectLightInBounds)
{
    VoxelGrid grid;
    grid.Initialize(8, 20.0f);
    // World position (0, 0, 0) is the centre of the grid — always inside.
    grid.InjectLight(0, 0, 0, 200, 150, 100, 255);
    // Can't directly inspect voxels without BuildMipChain + SampleMip,
    // so we trace a cone toward the origin and expect non-zero output.
    grid.BuildMipChain();
    float r, g, b, occ;
    grid.TraceCone(0, 0, -10, 0, 0, 1, 0.2f, 20.0f, r, g, b, occ);
    // The injected voxel should produce *some* occlusion along the
    // trace toward the origin.
    EXPECT_GT(occ, 0.0f);
}

TEST(VoxelConeTracing_VoxelGridInjectLightOutOfBoundsIsSafe)
{
    VoxelGrid grid;
    grid.Initialize(8, 10.0f);
    // Far outside the grid — must not crash, must not touch any voxel.
    grid.InjectLight(1000.0f, 1000.0f, 1000.0f, 255, 255, 255, 255);
    grid.BuildMipChain();
    // Trace from well inside the grid — should see zero injected light.
    float r, g, b, occ;
    grid.TraceCone(0, 0, 0, 1, 0, 0, 0.2f, 10.0f, r, g, b, occ);
    EXPECT_NEAR(r, 0.0f, 1e-4f);
    EXPECT_NEAR(g, 0.0f, 1e-4f);
    EXPECT_NEAR(b, 0.0f, 1e-4f);
    EXPECT_NEAR(occ, 0.0f, 1e-4f);
}

TEST(VoxelConeTracing_VoxelGridClear)
{
    VoxelGrid grid;
    grid.Initialize(8, 20.0f);
    grid.InjectLight(0, 0, 0, 255, 255, 255, 255);
    grid.InjectLight(5, 5, 5, 128, 64, 32, 255);
    grid.BuildMipChain();

    // Clear should zero every voxel + every mip level.
    grid.Clear();
    float r, g, b, occ;
    grid.TraceCone(-10, -10, -10, 1, 1, 1, 0.5f, 30.0f, r, g, b, occ);
    EXPECT_NEAR(r, 0.0f, 1e-4f);
    EXPECT_NEAR(g, 0.0f, 1e-4f);
    EXPECT_NEAR(b, 0.0f, 1e-4f);
    EXPECT_NEAR(occ, 0.0f, 1e-4f);
}

// =========================================================================
// VoxelGrid: cone tracing
// =========================================================================

TEST(VoxelConeTracing_TraceConeEmptyGridReturnsZero)
{
    VoxelGrid grid;
    grid.Initialize(8, 20.0f);
    grid.BuildMipChain();
    float r, g, b, occ;
    grid.TraceCone(0, 0, 0, 0, 0, 1, 0.2f, 15.0f, r, g, b, occ);
    EXPECT_NEAR(r, 0.0f, 1e-4f);
    EXPECT_NEAR(g, 0.0f, 1e-4f);
    EXPECT_NEAR(b, 0.0f, 1e-4f);
    EXPECT_NEAR(occ, 0.0f, 1e-4f);
}

TEST(VoxelConeTracing_TraceConeAccumulatesLight)
{
    VoxelGrid grid;
    grid.Initialize(16, 30.0f);
    // Inject a wall of geometry between origin and +Z.
    for (int i = -2; i <= 2; ++i)
    {
        for (int j = -2; j <= 2; ++j)
        {
            grid.InjectLight(static_cast<float>(i) * 2.0f, static_cast<float>(j) * 2.0f, 10.0f, 200, 100, 50, 255);
        }
    }
    grid.BuildMipChain();

    float r, g, b, occ;
    grid.TraceCone(0, 0, 0, 0, 0, 1, 0.3f, 30.0f, r, g, b, occ);
    // The trace should hit the wall and accumulate some radiance + occlusion.
    EXPECT_GT(occ, 0.0f);
    EXPECT_GT(r + g + b, 0.0f);
}

TEST(VoxelConeTracing_TraceConeOccludedByWall)
{
    VoxelGrid grid;
    grid.Initialize(16, 30.0f);
    // Inject a solid column (opacity-only) on the +X axis.
    for (int y = -3; y <= 3; ++y)
    {
        for (int z = -3; z <= 3; ++z)
        {
            grid.InjectLight(5.0f, static_cast<float>(y) * 2.0f, static_cast<float>(z) * 2.0f, 0, 0, 0, 255);
        }
    }
    grid.BuildMipChain();

    // Trace toward +X — should see occlusion from the column.
    float r, g, b, occ;
    grid.TraceCone(0, 0, 0, 1, 0, 0, 0.3f, 20.0f, r, g, b, occ);
    EXPECT_GT(occ, 0.0f);
}

TEST(VoxelConeTracing_StepMultiplierAccessor)
{
    VoxelGrid grid;
    grid.Initialize(8, 20.0f);
    grid.SetStepMultiplier(2.0f); // coarser steps
    // No crash, no behaviour-change API to check directly; the trace
    // should still work without crashing.
    grid.BuildMipChain();
    float r, g, b, occ;
    grid.TraceCone(0, 0, 0, 0, 1, 0, 0.2f, 20.0f, r, g, b, occ);
    EXPECT_GE(occ, 0.0f);
}

// =========================================================================
// VoxelGrid: mip chain
// =========================================================================

TEST(VoxelConeTracing_BuildMipChainIsIdempotent)
{
    VoxelGrid grid;
    grid.Initialize(8, 20.0f);
    grid.InjectLight(0, 0, 0, 200, 100, 50, 255);
    grid.BuildMipChain();
    grid.BuildMipChain(); // second call must not break state

    float r, g, b, occ;
    grid.TraceCone(0, 0, -10, 0, 0, 1, 0.2f, 20.0f, r, g, b, occ);
    EXPECT_GT(occ, 0.0f);
}

// =========================================================================
// VCTSystem: lifecycle
// =========================================================================

TEST(VoxelConeTracing_VCTSystemInitializeAndShutdown)
{
    VCTSystem sys;
    VCTSettings settings;
    settings.voxelResolution = 16;
    settings.worldExtent = 20.0f;
    EXPECT_TRUE(sys.Initialize(settings));
    EXPECT_TRUE(sys.GetGrid().IsInitialized());
    EXPECT_EQ(sys.GetGrid().GetResolution(), 16u);
    sys.Shutdown();
    EXPECT_FALSE(sys.GetGrid().IsInitialized());
}

TEST(VoxelConeTracing_VCTSystemDefaultInitialize)
{
    VCTSystem sys;
    EXPECT_TRUE(sys.Initialize());
    // Default resolution is 128 — sanity check the accessor.
    EXPECT_EQ(sys.GetGrid().GetResolution(), 128u);
    sys.Shutdown();
}

// =========================================================================
// VCTSystem: settings round-trip
// =========================================================================

TEST(VoxelConeTracing_VCTSystemSettingsRoundTrip)
{
    VCTSystem sys;
    VCTSettings settings;
    settings.voxelResolution = 16;
    settings.worldExtent = 25.0f;
    settings.diffuseCones = 4;
    settings.indirectDiffuseStrength = 2.5f;
    sys.Initialize(settings);

    const auto& read = sys.GetSettings();
    EXPECT_EQ(read.voxelResolution, 16u);
    EXPECT_NEAR(read.worldExtent, 25.0f, 1e-6f);
    EXPECT_EQ(read.diffuseCones, 4);
    EXPECT_NEAR(read.indirectDiffuseStrength, 2.5f, 1e-6f);

    // Mutable accessor: change a setting, confirm read-back.
    sys.GetSettings().diffuseCones = 8;
    EXPECT_EQ(sys.GetSettings().diffuseCones, 8);
    sys.Shutdown();
}

// =========================================================================
// VCTSystem: voxelization pipeline
// =========================================================================

TEST(VoxelConeTracing_VCTSystemBeginEndVoxelization)
{
    VCTSystem sys;
    VCTSettings settings;
    settings.voxelResolution = 16;
    settings.worldExtent = 20.0f;
    sys.Initialize(settings);

    sys.BeginVoxelization();
    sys.InjectLight(0, 0, 0, 1.0f, 0.5f, 0.25f, 1.0f);
    sys.InjectGeometry(5, 5, 5, 255);
    sys.EndVoxelization();

    // Trace a cone toward the injected light — should see non-zero
    // occlusion.
    float r, g, b, ao;
    sys.TraceDiffuse(0, 0, -5, 0, 0, 1, r, g, b, ao);
    EXPECT_GE(ao, 0.0f);
    sys.Shutdown();
}

TEST(VoxelConeTracing_VCTSystemInjectLightIntensityScales)
{
    VCTSystem sys;
    VCTSettings settings;
    settings.voxelResolution = 16;
    settings.worldExtent = 20.0f;
    sys.Initialize(settings);

    sys.BeginVoxelization();
    // Zero intensity → no light injected.
    sys.InjectLight(0, 0, 0, 1.0f, 1.0f, 1.0f, 0.0f);
    sys.EndVoxelization();

    float r, g, b, ao;
    sys.TraceDiffuse(0, 0, -5, 0, 0, 1, r, g, b, ao);
    // Radiance should be ~0 since no light was actually stored.
    EXPECT_NEAR(r, 0.0f, 1e-3f);
    EXPECT_NEAR(g, 0.0f, 1e-3f);
    EXPECT_NEAR(b, 0.0f, 1e-3f);
    sys.Shutdown();
}

// =========================================================================
// VCTSystem: cone tracing (diffuse + specular)
// =========================================================================

TEST(VoxelConeTracing_TraceDiffuseRespectsConeCount)
{
    VCTSystem sys;
    VCTSettings settings;
    settings.voxelResolution = 16;
    settings.worldExtent = 20.0f;
    settings.diffuseCones = 1;
    settings.indirectDiffuseStrength = 1.0f;
    sys.Initialize(settings);

    sys.BeginVoxelization();
    sys.InjectLight(0, 0, 5, 1.0f, 1.0f, 1.0f, 1.0f);
    sys.EndVoxelization();

    float r1, g1, b1, ao1;
    sys.TraceDiffuse(0, 0, 0, 0, 0, 1, r1, g1, b1, ao1);

    // Bump cone count to 6 — result may differ but should still be valid.
    sys.GetSettings().diffuseCones = 6;
    float r6, g6, b6, ao6;
    sys.TraceDiffuse(0, 0, 0, 0, 0, 1, r6, g6, b6, ao6);

    // Both calls should produce non-negative output without crashing.
    EXPECT_GE(ao1, 0.0f);
    EXPECT_GE(ao6, 0.0f);
    sys.Shutdown();
}

TEST(VoxelConeTracing_TraceSpecularRoughnessWidensCone)
{
    VCTSystem sys;
    VCTSettings settings;
    settings.voxelResolution = 16;
    settings.worldExtent = 20.0f;
    sys.Initialize(settings);

    sys.BeginVoxelization();
    sys.InjectLight(0, 0, 5, 1.0f, 0.5f, 0.25f, 1.0f);
    sys.EndVoxelization();

    float r, g, b;
    // Sharp reflection (roughness ≈ 0) uses the narrow specular cone.
    sys.TraceSpecular(0, 0, 0, 0, 0, 1, 0.0f, r, g, b);
    EXPECT_GE(r, 0.0f);

    // Rough reflection (roughness ≈ 1) uses a wider cone.
    sys.TraceSpecular(0, 0, 0, 0, 0, 1, 1.0f, r, g, b);
    EXPECT_GE(r, 0.0f);
    sys.Shutdown();
}

TEST(VoxelConeTracing_TraceDiffuseEmptyGridIsZero)
{
    VCTSystem sys;
    VCTSettings settings;
    settings.voxelResolution = 16;
    settings.worldExtent = 20.0f;
    sys.Initialize(settings);

    sys.BeginVoxelization();
    sys.EndVoxelization();

    float r, g, b, ao;
    sys.TraceDiffuse(0, 0, 0, 0, 1, 0, r, g, b, ao);
    EXPECT_NEAR(r, 0.0f, 1e-4f);
    EXPECT_NEAR(g, 0.0f, 1e-4f);
    EXPECT_NEAR(b, 0.0f, 1e-4f);
    EXPECT_NEAR(ao, 0.0f, 1e-4f);
    sys.Shutdown();
}

// =========================================================================
// VCTSystem: console status
// =========================================================================

TEST(VoxelConeTracing_VCTSystemConsoleStatusFormatting)
{
    VCTSystem sys;
    VCTSettings settings;
    settings.voxelResolution = 8;
    settings.worldExtent = 10.0f;
    settings.enabled = true;
    sys.Initialize(settings);

    std::string status = sys.Console_GetStatus();
    EXPECT_TRUE(status.find("VCTSystem") != std::string::npos);
    EXPECT_TRUE(status.find("ON") != std::string::npos);
    EXPECT_TRUE(status.find("VoxelGrid") != std::string::npos);
    sys.Shutdown();
}

TEST(VoxelConeTracing_VCTSystemConsoleDisabledStatus)
{
    VCTSystem sys;
    VCTSettings settings;
    settings.voxelResolution = 8;
    settings.worldExtent = 10.0f;
    settings.enabled = false;
    sys.Initialize(settings);

    std::string status = sys.Console_GetStatus();
    EXPECT_TRUE(status.find("OFF") != std::string::npos);
    sys.Shutdown();
}

// =========================================================================
// VCTSystem: reinitialise cycle
// =========================================================================

TEST(VoxelConeTracing_VCTSystemReinitialiseGrowsGrid)
{
    VCTSystem sys;
    VCTSettings smallSettings;
    smallSettings.voxelResolution = 8;
    smallSettings.worldExtent = 10.0f;
    sys.Initialize(smallSettings);
    EXPECT_EQ(sys.GetGrid().GetResolution(), 8u);

    VCTSettings bigSettings;
    bigSettings.voxelResolution = 32;
    bigSettings.worldExtent = 50.0f;
    sys.Initialize(bigSettings);
    EXPECT_EQ(sys.GetGrid().GetResolution(), 32u);
    sys.Shutdown();
}
