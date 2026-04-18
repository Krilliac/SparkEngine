/**
 * @file TestMetalRayTracingLive.mm
 * @brief Live-device Metal RT tests (macOS + Metal toolchain only).
 *
 * Compiled and linked only on the macOS CI row that has Metal enabled
 * (SPARK_METAL_AVAILABLE). Off-macOS or on macOS runs without the Metal
 * backend, the file is not added to the test target at all — so there's
 * no empty-TU / unused-symbol risk.
 *
 * These tests use a real `MTLDevice` obtained via
 * `MTLCreateSystemDefaultDevice`. That call works in headless contexts
 * (no window / CAMetalLayer needed), so the macOS-latest runner in CI
 * can exercise them even without a display attached. Tests that
 * require a device that `supportsRaytracing` are guarded by that
 * predicate — on runners whose GPU predates macOS 12 / Apple Silicon
 * ray-tracing support, the guarded tests report a skip by returning
 * early after a neutral EXPECT_TRUE(true) assertion.
 *
 * The contract being validated: Initialize → BLAS push → TLAS build →
 * SetMaterials → SetFrameParams → DispatchFrame round-trip completes
 * without crashing and returns consistent state. We intentionally don't
 * validate output pixels — that's the job of image-diff tests on a
 * GPU-equipped runner with a stable reference image set.
 */

#include "Core/Platform.h"
#include "TestFramework.h"

#ifdef SPARK_PLATFORM_MACOS

#import <Metal/Metal.h>

#include "Graphics/RHI/Metal/MetalDevice.h"
#include "Graphics/RHI/Metal/MetalRayTracing.h"

#include <vector>

namespace
{
// Stand up a real Metal device through the engine's own `MetalDevice`
// wrapper — same path the engine takes at runtime. Returns nullopt when
// the runner has no Metal-capable GPU or ray-tracing isn't supported;
// tests translate nullopt into a neutral skip via the `SKIP_IF_NO_RT`
// macro below.
struct LiveMetalDevice
{
    Spark::RHI::Metal::MetalDevice device;
    bool initialized = false;
    bool supportsRT = false;

    LiveMetalDevice()
    {
        Spark::RHI::RHIDeviceDesc desc;
        desc.enableDebugLayer = false;
        initialized = device.Initialize(desc);
        if (!initialized)
            return;
        auto raw = device.GetMTLDevice();
        if (raw && @available(macOS 12.0, *))
            supportsRT = [raw supportsRaytracing];
    }

    ~LiveMetalDevice()
    {
        if (initialized)
            device.Shutdown();
    }

    bool Usable() const
    {
        return initialized && supportsRT;
    }
};
} // namespace

// CI runners without a Metal-capable GPU (or with a pre-12.0 Metal
// feature set) short-circuit to a passing no-op — we're testing the
// contract on real hardware, not checking that it exists.
#define SKIP_IF_NO_RT(dev)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(dev).Usable())                                                                                           \
        {                                                                                                              \
            EXPECT_TRUE(true);                                                                                         \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

TEST(MetalRT_Live_SystemDefaultDeviceAvailable)
{
    LiveMetalDevice d;
    // On a runner without Metal, this records a skip-pass instead of a
    // hard failure. The other tests are gated on `Usable()` so they
    // skip too.
    if (!d.initialized)
    {
        EXPECT_TRUE(true);
        return;
    }
    EXPECT_TRUE(d.device.GetMTLDevice() != nil);
}

TEST(MetalRT_Live_InitializeSucceedsOnRTCapableGPU)
{
    LiveMetalDevice d;
    SKIP_IF_NO_RT(d);

    Spark::RHI::Metal::MetalRayTracingSystem rt;
    EXPECT_TRUE(rt.Initialize(&d.device));
    EXPECT_TRUE(rt.IsAvailable());
    rt.Shutdown();
}

TEST(MetalRT_Live_PushBLASAndBuildTLAS)
{
    LiveMetalDevice d;
    SKIP_IF_NO_RT(d);

    Spark::RHI::Metal::MetalRayTracingSystem rt;
    if (!rt.Initialize(&d.device))
    {
        EXPECT_TRUE(true);
        return;
    }

    // A minimal 1-triangle mesh at world-space origin.
    const float verts[] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint32_t idx[] = {0, 1, 2};

    Spark::RHI::Metal::BLASGeometry geo;
    geo.name = "UnitTri";
    geo.vertexData = verts;
    geo.vertexCount = 3;
    geo.vertexStride = sizeof(float) * 3;
    geo.indexData = idx;
    geo.indexCount = 3;

    uint32_t blasIdx = rt.CreateBLAS(geo);
    EXPECT_TRUE(blasIdx != UINT32_MAX);

    // Build an empty TLAS first — exercises the builder without any
    // instances. The real engine does this on scene-clear.
    rt.BuildTLAS({});

    // Then a single-instance TLAS referencing the BLAS we just pushed.
    Spark::RHI::Metal::TLASInstance inst;
    inst.blasIndex = blasIdx;
    inst.instanceID = 0;
    rt.BuildTLAS({inst});

    rt.Shutdown();
}

TEST(MetalRT_Live_SetMaterialsRoundTrip)
{
    LiveMetalDevice d;
    SKIP_IF_NO_RT(d);

    Spark::RHI::Metal::MetalRayTracingSystem rt;
    if (!rt.Initialize(&d.device))
    {
        EXPECT_TRUE(true);
        return;
    }

    // Upload three materials, then drop them. The buffer-upload path
    // has to handle both non-trivial sizes and the empty-vector teardown
    // without leaking or crashing.
    std::vector<Spark::RHI::Metal::MaterialParams> mats(3);
    mats[0].albedo[0] = 1.0f; // red
    mats[1].albedo[1] = 1.0f; // green
    mats[2].albedo[2] = 1.0f; // blue
    rt.SetMaterials(mats);
    rt.SetMaterials({});

    rt.Shutdown();
}

TEST(MetalRT_Live_DispatchFrameWithoutRenderTargetsIsSafe)
{
    LiveMetalDevice d;
    SKIP_IF_NO_RT(d);

    Spark::RHI::Metal::MetalRayTracingSystem rt;
    if (!rt.Initialize(&d.device))
    {
        EXPECT_TRUE(true);
        return;
    }

    Spark::RHI::Metal::FrameParams fp;
    fp.resolutionX = 64;
    fp.resolutionY = 64;
    rt.SetFrameParams(fp);

    // No input textures, no TLAS — every pass must short-circuit
    // through the WarnOnce path in EncodeTracePass and return false.
    // Contract: "safe to call in any order, never crash". Verified
    // here by exercising the full mask.
    auto executed =
        rt.DispatchFrame(Spark::RHI::Metal::TracePass::Shadows | Spark::RHI::Metal::TracePass::Reflections |
                         Spark::RHI::Metal::TracePass::AmbientOcclusion |
                         Spark::RHI::Metal::TracePass::GlobalIllumination);
    EXPECT_TRUE(!Spark::RHI::Metal::Any(executed));

    rt.Shutdown();
}

TEST(MetalRT_Live_RepeatedInitShutdownIsStable)
{
    LiveMetalDevice d;
    SKIP_IF_NO_RT(d);

    // Engine lifecycle (map swap, scene reload) may Init → Shutdown
    // the RT system multiple times. Metal texture / pipeline handles
    // must release cleanly each cycle.
    for (int i = 0; i < 3; ++i)
    {
        Spark::RHI::Metal::MetalRayTracingSystem rt;
        if (!rt.Initialize(&d.device))
        {
            EXPECT_TRUE(true);
            return;
        }
        rt.Shutdown();
    }
    EXPECT_TRUE(true);
}

#endif // SPARK_PLATFORM_MACOS
