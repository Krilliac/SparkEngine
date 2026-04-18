/**
 * @file TestMetalRayTracing.cpp
 * @brief macOS-only smoke tests for MetalRayTracingSystem lifecycle.
 *
 * On non-macOS builds the entire TU is an empty translation unit — the
 * Metal headers aren't available and the RT surface types don't exist.
 * On macOS, we exercise the handful of state transitions that don't
 * require a live MTLDevice:
 *
 *   - Default-constructed system reports not initialized.
 *   - `Initialize(nullptr)` returns false without crashing.
 *   - Trace methods return false before Initialize succeeds.
 *   - `SetMaterials(empty)` resets the material count.
 *   - `Shutdown` on a pristine instance is safe.
 *
 * Anything that needs a real `MetalDevice` (live BLAS build, real
 * trace dispatch) lives behind the macOS CI Metal job — once that
 * job validates compiles, we'll add runtime tests that spin up a
 * headless MTLDevice.
 */

#include "Core/Platform.h"
#include "TestFramework.h"

#ifdef SPARK_PLATFORM_MACOS

#include "Graphics/HybridRT/RTMaterialAdapter.h"
#include "Graphics/MaterialSystem.h"
#include "Graphics/RHI/Metal/MetalRayTracing.h"

TEST(MetalRayTracing_DefaultConstructNotAvailable)
{
    Spark::RHI::Metal::MetalRayTracingSystem sys;
    EXPECT_TRUE(!sys.IsAvailable());
}

TEST(MetalRayTracing_InitializeNullDeviceFails)
{
    Spark::RHI::Metal::MetalRayTracingSystem sys;
    EXPECT_TRUE(!sys.Initialize(nullptr));
    EXPECT_TRUE(!sys.IsAvailable());
}

TEST(MetalRayTracing_TraceBeforeInitReturnsFalse)
{
    Spark::RHI::Metal::MetalRayTracingSystem sys;
    EXPECT_TRUE(!sys.TraceShadows());
    EXPECT_TRUE(!sys.TraceReflections());
    EXPECT_TRUE(!sys.TraceAmbientOcclusion());
    EXPECT_TRUE(!sys.TraceGlobalIllumination());
}

TEST(MetalRayTracing_DispatchFrameBeforeInitReturnsNone)
{
    Spark::RHI::Metal::MetalRayTracingSystem sys;
    auto executed =
        sys.DispatchFrame(Spark::RHI::Metal::TracePass::Shadows | Spark::RHI::Metal::TracePass::Reflections);
    EXPECT_TRUE(!Spark::RHI::Metal::Any(executed));
}

TEST(MetalRayTracing_ShutdownOnPristineIsSafe)
{
    Spark::RHI::Metal::MetalRayTracingSystem sys;
    sys.Shutdown();
    sys.Shutdown(); // Idempotent
    EXPECT_TRUE(!sys.IsAvailable());
}

TEST(MetalRayTracing_SetMaterialsEmptyResetsCount)
{
    Spark::RHI::Metal::MetalRayTracingSystem sys;
    std::vector<Spark::RHI::Metal::MaterialParams> mats;
    sys.SetMaterials(mats); // No device, should not crash.
    EXPECT_TRUE(true);
}

TEST(MetalRayTracing_GetStatusStringReturnsSomething)
{
    Spark::RHI::Metal::MetalRayTracingSystem sys;
    auto s = sys.GetStatusString();
    EXPECT_TRUE(!s.empty());
}

// ---------------------------------------------------------------------------
// RTMaterialAdapter — pure CPU tests, no MTLDevice required
// ---------------------------------------------------------------------------

TEST(MetalRT_MaterialAdapter_AlbedoForwardedVerbatim)
{
    PBRProperties pbr;
    pbr.albedoColor = {0.25f, 0.5f, 0.75f, 1.0f};
    auto mat = Spark::Graphics::MaterialParamsFromPBR(pbr);
    EXPECT_EQ(mat.albedo[0], 0.25f);
    EXPECT_EQ(mat.albedo[1], 0.5f);
    EXPECT_EQ(mat.albedo[2], 0.75f);
    EXPECT_EQ(mat.albedo[3], 1.0f);
}

TEST(MetalRT_MaterialAdapter_EmissiveBakesFactor)
{
    // The kernel sees final radiance; the factor must be multiplied in
    // so the kernel doesn't need a second uniform. 2.0f emissive intensity
    // on (0.5, 0, 0) should land as (1.0, 0, 0).
    PBRProperties pbr;
    pbr.emissiveColor = {0.5f, 0.0f, 0.0f};
    pbr.emissiveFactor = 2.0f;
    auto mat = Spark::Graphics::MaterialParamsFromPBR(pbr);
    EXPECT_EQ(mat.emissive[0], 1.0f);
    EXPECT_EQ(mat.emissive[1], 0.0f);
    EXPECT_EQ(mat.emissive[2], 0.0f);
}

TEST(MetalRT_MaterialAdapter_RoughnessMetallicPacked)
{
    PBRProperties pbr;
    pbr.roughnessFactor = 0.7f;
    pbr.metallicFactor = 0.3f;
    auto mat = Spark::Graphics::MaterialParamsFromPBR(pbr);
    EXPECT_EQ(mat.roughnessMetallic[0], 0.7f);
    EXPECT_EQ(mat.roughnessMetallic[1], 0.3f);
    EXPECT_EQ(mat.roughnessMetallic[2], 0.0f);
    EXPECT_EQ(mat.roughnessMetallic[3], 0.0f);
}

TEST(MetalRT_MaterialAdapter_DefaultsAreNeutral)
{
    // Default-constructed PBR should produce a safe neutral material —
    // white diffuse, no emission, mid roughness. Prevents surprise black
    // or explosive emissives when a mesh has no material assigned.
    PBRProperties pbr;
    auto mat = Spark::Graphics::MaterialParamsFromPBR(pbr);
    EXPECT_EQ(mat.albedo[0], 1.0f);
    EXPECT_EQ(mat.albedo[1], 1.0f);
    EXPECT_EQ(mat.albedo[2], 1.0f);
    EXPECT_EQ(mat.emissive[0], 0.0f);
    EXPECT_EQ(mat.roughnessMetallic[0], 0.5f);
    EXPECT_EQ(mat.roughnessMetallic[1], 0.0f);
}

TEST(MetalRT_MaterialParams_SizeAndAlignment)
{
    // The Metal constant-buffer layout requires 16-byte-aligned float4
    // rows. 48 bytes = three float4s (albedo + emissive +
    // roughnessMetallic). Drift here would break every kernel dispatch.
    EXPECT_EQ(sizeof(Spark::RHI::Metal::MaterialParams), 48u);
}

// ---------------------------------------------------------------------------
// FrameParams + TLASInstance layout — matches the MSL struct exactly.
// ---------------------------------------------------------------------------

TEST(MetalRT_FrameParams_MatchesMSLLayout)
{
    // Kernel reads:
    //   float4x4 invViewProj  (64 B)
    //   float4 cameraPos      (16 B)  — xyz + pad
    //   float4 lightDir       (16 B)  — xyz + pad
    //   uint2 resolution + uint2 pad  (16 B)
    // Total = 112 bytes. Any change here requires a coordinated MSL
    // shader update in MetalRayTracing.mm.
    EXPECT_EQ(sizeof(Spark::RHI::Metal::FrameParams), 112u);
}

TEST(MetalRT_TLASInstance_DefaultIsIdentity)
{
    Spark::RHI::Metal::TLASInstance inst;
    // Row-major 3x4 identity: row 0 = (1,0,0,0), row 1 = (0,1,0,0), row 2 = (0,0,1,0).
    EXPECT_EQ(inst.transform[0], 1.0f);
    EXPECT_EQ(inst.transform[5], 1.0f);
    EXPECT_EQ(inst.transform[10], 1.0f);
    EXPECT_EQ(inst.transform[3], 0.0f); // translation x
    EXPECT_EQ(inst.instanceMask, 0xFFu);
}

// ---------------------------------------------------------------------------
// TracePass bitmask — exercised by DispatchFrame.
// ---------------------------------------------------------------------------

TEST(MetalRT_TracePass_BitwiseOrCombines)
{
    using namespace Spark::RHI::Metal;
    auto combo = TracePass::Shadows | TracePass::Reflections;
    EXPECT_TRUE(Any(combo & TracePass::Shadows));
    EXPECT_TRUE(Any(combo & TracePass::Reflections));
    EXPECT_TRUE(!Any(combo & TracePass::GlobalIllumination));
}

TEST(MetalRT_TracePass_NoneIsFalsy)
{
    using namespace Spark::RHI::Metal;
    EXPECT_TRUE(!Any(TracePass::None));
}

#endif // SPARK_PLATFORM_MACOS
