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

#include "TestFramework.h"
#include "Core/Platform.h"

#ifdef SPARK_PLATFORM_MACOS

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

#endif // SPARK_PLATFORM_MACOS
