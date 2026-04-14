// TestDXRSupport.cpp — Tests for DXR support layer
//
// Covers:
//   - RTFeature flag operators (or, and, !, HasFeature)
//   - DXRManager singleton lifecycle without a real D3D12 device
//   - Trace*() methods are safe no-ops when uninitialized
//   - Settings round-trip
//   - Console_GetStatus reports the expected disabled state
//
// The full DXR pipeline only runs on Windows MSVC with a DXR-capable
// GPU and pre-compiled .cso shader blobs. None of those are available
// in CI, so this file deliberately covers only the public API contract
// that DXRManager exposes regardless of platform — the bits that the
// rest of the engine depends on.
//
// On MinGW the engine build excludes DXRSupport.cpp entirely (see the
// SPARK_NO_D3D12 block in CMakeLists.txt — MinGW's d3d12.h headers are
// too old for ID3D12Device5 etc.), so the DXRManager symbols this file
// references don't exist in the link image. Compile the whole file out
// when SPARK_NO_D3D12 is defined.

#include "TestFramework.h"

#ifndef SPARK_NO_D3D12

#include "Graphics/RHI/DXRSupport.h"

#include <cstdint>

using namespace Spark::Graphics;

// ============================================================================
// RTFeature flag arithmetic
// ============================================================================

TEST(DXR_RTFeature_OrOperator)
{
    RTFeature combined = RTFeature::Reflections | RTFeature::Shadows;
    EXPECT_TRUE(HasFeature(combined, RTFeature::Reflections));
    EXPECT_TRUE(HasFeature(combined, RTFeature::Shadows));
    EXPECT_FALSE(HasFeature(combined, RTFeature::AmbientOcclusion));
    EXPECT_FALSE(HasFeature(combined, RTFeature::GlobalIllumination));
}

TEST(DXR_RTFeature_AndOperator)
{
    RTFeature both = RTFeature::Reflections | RTFeature::AmbientOcclusion;
    RTFeature mask = both & RTFeature::Reflections;
    EXPECT_TRUE(HasFeature(mask, RTFeature::Reflections));
    EXPECT_FALSE(HasFeature(mask, RTFeature::AmbientOcclusion));
}

TEST(DXR_RTFeature_NoneIsFalsy)
{
    RTFeature empty = RTFeature::None;
    EXPECT_TRUE(!empty);
    EXPECT_FALSE(HasFeature(empty, RTFeature::Reflections));
    EXPECT_FALSE(HasFeature(empty, RTFeature::Shadows));
    EXPECT_FALSE(HasFeature(empty, RTFeature::AmbientOcclusion));
    EXPECT_FALSE(HasFeature(empty, RTFeature::GlobalIllumination));
}

TEST(DXR_RTFeature_AllIncludesEverything)
{
    EXPECT_TRUE(HasFeature(RTFeature::All, RTFeature::Reflections));
    EXPECT_TRUE(HasFeature(RTFeature::All, RTFeature::Shadows));
    EXPECT_TRUE(HasFeature(RTFeature::All, RTFeature::AmbientOcclusion));
    EXPECT_TRUE(HasFeature(RTFeature::All, RTFeature::GlobalIllumination));
}

// ============================================================================
// DXRManager — uninitialized state
// ============================================================================

TEST(DXR_Manager_NotAvailableInHeadless)
{
    auto& mgr = DXRManager::GetInstance();
    // No real D3D12 device — Initialize was never called with one.
    // The headless test runner has no DXR-capable GPU; IsAvailable
    // must return false here so callers fall back to SDFGI.
    EXPECT_FALSE(mgr.IsAvailable());
}

TEST(DXR_Manager_BackendDisabledWhenUnavailable)
{
    auto& mgr = DXRManager::GetInstance();
    EXPECT_FALSE(mgr.IsAvailable());
    EXPECT_TRUE(mgr.GetBackend() == Spark::RHI::RayTracingBackend::Disabled);
}

TEST(DXR_Manager_InitializeWithNullDeviceFails)
{
    auto& mgr = DXRManager::GetInstance();
    EXPECT_FALSE(mgr.Initialize(nullptr));
    EXPECT_FALSE(mgr.IsAvailable());
}

// ============================================================================
// DXRManager — Trace methods are safe no-ops
// ============================================================================

TEST(DXR_Manager_TraceMethodsNoOpWhenUninitialized)
{
    auto& mgr = DXRManager::GetInstance();
    EXPECT_FALSE(mgr.IsAvailable());

    // Build a settings object that requests every feature so the early
    // out path inside Trace*() can't skip on the feature flag alone —
    // we want to exercise the "not initialized" branch.
    DXRSettings settings;
    settings.enabledFeatures = RTFeature::All;
    settings.reflections.enabled = true;
    settings.shadows.enabled = true;
    settings.ambientOcclusion.enabled = true;
    settings.globalIllumination.enabled = true;
    mgr.SetSettings(settings);

    // Calling these on a non-initialized manager must not crash. The
    // identity matrix and zero vectors are deliberately trivial — the
    // point is to exercise the guard in the trace methods, not the math.
    DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();
    DirectX::XMFLOAT3 origin{0.0f, 0.0f, 0.0f};
    mgr.TraceReflections(identity, origin);
    mgr.TraceShadows(origin);
    mgr.TraceAmbientOcclusion(identity, origin);
    mgr.TraceGlobalIllumination(identity, origin);

    // Still not available after the no-op calls.
    EXPECT_FALSE(mgr.IsAvailable());
}

// ============================================================================
// DXRManager — settings round-trip
// ============================================================================

TEST(DXR_Manager_SettingsRoundTrip)
{
    auto& mgr = DXRManager::GetInstance();

    DXRSettings settings;
    settings.enabledFeatures = RTFeature::Reflections | RTFeature::AmbientOcclusion;
    settings.reflections.enabled = true;
    settings.reflections.maxBounces = 3;
    settings.reflections.samplesPerPixel = 4;
    settings.reflections.maxDistance = 250.0f;
    settings.reflections.roughnessThreshold = 0.7f;
    settings.ambientOcclusion.enabled = true;
    settings.ambientOcclusion.radius = 5.0f;
    settings.ambientOcclusion.samplesPerPixel = 8;
    settings.renderScale = 0.85f;

    mgr.SetSettings(settings);
    const DXRSettings& got = mgr.GetSettings();

    EXPECT_TRUE(HasFeature(got.enabledFeatures, RTFeature::Reflections));
    EXPECT_TRUE(HasFeature(got.enabledFeatures, RTFeature::AmbientOcclusion));
    EXPECT_FALSE(HasFeature(got.enabledFeatures, RTFeature::Shadows));
    EXPECT_EQ(got.reflections.maxBounces, 3);
    EXPECT_EQ(got.reflections.samplesPerPixel, 4);
    EXPECT_NEAR(got.reflections.maxDistance, 250.0f, 1e-5f);
    EXPECT_NEAR(got.reflections.roughnessThreshold, 0.7f, 1e-5f);
    EXPECT_NEAR(got.ambientOcclusion.radius, 5.0f, 1e-5f);
    EXPECT_EQ(got.ambientOcclusion.samplesPerPixel, 8);
    EXPECT_NEAR(got.renderScale, 0.85f, 1e-5f);
}

// ============================================================================
// DXRManager — console feature toggles
// ============================================================================

TEST(DXR_Manager_ConsoleEnableFeature)
{
    auto& mgr = DXRManager::GetInstance();

    DXRSettings clean;
    clean.enabledFeatures = RTFeature::None;
    mgr.SetSettings(clean);
    EXPECT_FALSE(HasFeature(mgr.GetSettings().enabledFeatures, RTFeature::Reflections));

    mgr.Console_EnableFeature("reflections", true);
    EXPECT_TRUE(HasFeature(mgr.GetSettings().enabledFeatures, RTFeature::Reflections));

    mgr.Console_EnableFeature("shadows", true);
    EXPECT_TRUE(HasFeature(mgr.GetSettings().enabledFeatures, RTFeature::Shadows));
    EXPECT_TRUE(HasFeature(mgr.GetSettings().enabledFeatures, RTFeature::Reflections));

    mgr.Console_EnableFeature("reflections", false);
    EXPECT_FALSE(HasFeature(mgr.GetSettings().enabledFeatures, RTFeature::Reflections));
    EXPECT_TRUE(HasFeature(mgr.GetSettings().enabledFeatures, RTFeature::Shadows));

    // Unknown feature names are silently ignored.
    mgr.Console_EnableFeature("not-a-real-feature", true);
    EXPECT_TRUE(HasFeature(mgr.GetSettings().enabledFeatures, RTFeature::Shadows));
}

TEST(DXR_Manager_ConsoleSetQualityPresets)
{
    auto& mgr = DXRManager::GetInstance();

    mgr.Console_SetQuality("low");
    EXPECT_NEAR(mgr.GetSettings().renderScale, 0.5f, 1e-5f);

    mgr.Console_SetQuality("medium");
    EXPECT_NEAR(mgr.GetSettings().renderScale, 0.75f, 1e-5f);

    mgr.Console_SetQuality("high");
    EXPECT_NEAR(mgr.GetSettings().renderScale, 1.0f, 1e-5f);

    mgr.Console_SetQuality("ultra");
    EXPECT_NEAR(mgr.GetSettings().renderScale, 1.0f, 1e-5f);
    EXPECT_TRUE(mgr.GetSettings().globalIllumination.enabled);
}

// ============================================================================
// DXRManager — stats and status
// ============================================================================

TEST(DXR_Manager_StatsZeroWhenUninitialized)
{
    auto& mgr = DXRManager::GetInstance();
    auto stats = mgr.GetStats();
    // BLAS / TLAS counts default to zero on a manager that has not been
    // through a successful Initialize() — the AS arrays are empty.
    EXPECT_EQ(stats.tlasInstanceCount, static_cast<uint32_t>(0));
}

TEST(DXR_Manager_ConsoleStatusFormat)
{
    auto& mgr = DXRManager::GetInstance();
    std::string status = mgr.Console_GetStatus();
    // Free-form text but must always include the "DXR Status" header
    // and the Available/Initialized fields the editor displays.
    EXPECT_TRUE(status.find("DXR Status") != std::string::npos);
    EXPECT_TRUE(status.find("Available") != std::string::npos);
    EXPECT_TRUE(status.find("Initialized") != std::string::npos);
}

#endif // !SPARK_NO_D3D12
