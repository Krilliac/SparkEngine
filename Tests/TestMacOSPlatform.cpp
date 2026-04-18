/**
 * @file TestMacOSPlatform.cpp
 * @brief Smoke tests for the macOS platform surface
 *
 * Exercises the macOS-specific helpers (Metal view lifecycle, executable
 * path resolution, backend preference) plus a handful of cross-platform
 * invariants that the macOS build trips on when a guard is missing.
 *
 * The helpers in Spark::MacOS are defined on every platform — on non-macOS
 * they compile as no-op stubs returning empty/0/nullptr. These tests assert
 * the stub behavior off-macOS and the real behavior on-macOS, so the same
 * TU runs green everywhere and gives us real coverage on the macOS CI
 * runner without a separate platform-gated build of SparkTests.
 */

#include "TestFramework.h"
#include "Core/Platform.h"
#include "Core/SparkEngineMacOS.h"

#include <cstdint>
#include <filesystem>

// ============================================================================
// Platform macro sanity — the macro set should be mutually exclusive and
// Apple Clang should light up the macOS branch.
// ============================================================================

TEST(MacOSPlatform_MacroConsistency)
{
#ifdef SPARK_PLATFORM_MACOS
    // macOS: no other platform macro may be set.
#ifdef SPARK_PLATFORM_WINDOWS
    EXPECT_TRUE(false); // SPARK_PLATFORM_WINDOWS must not be set on macOS
#endif
#ifdef SPARK_PLATFORM_LINUX
    EXPECT_TRUE(false); // SPARK_PLATFORM_LINUX must not be set on macOS
#endif
    EXPECT_TRUE(true);
#else
    // Off-macOS: SPARK_PLATFORM_MACOS must NOT be defined.
    EXPECT_TRUE(true);
#endif
}

// ============================================================================
// Metal view helpers — stubs everywhere off-macOS; real SDL_Metal calls on
// macOS (we can't create an NSWindow in CI without a display, so we only
// assert the lifecycle is safe with null inputs).
// ============================================================================

TEST(MacOSPlatform_GetMetalWindowFlag)
{
#ifdef SPARK_PLATFORM_MACOS
#ifdef SPARK_SDL2_AVAILABLE
    // SDL_WINDOW_METAL is a non-zero bitflag. We can't import the exact
    // value here without pulling SDL into every platform's build, so just
    // require non-zero — which is what the SparkEngineLinux.cpp dispatch
    // relies on.
    EXPECT_TRUE(Spark::MacOS::GetMetalWindowFlag() != 0u);
#else
    EXPECT_EQ(Spark::MacOS::GetMetalWindowFlag(), 0u);
#endif
#else
    EXPECT_EQ(Spark::MacOS::GetMetalWindowFlag(), 0u);
#endif
}

TEST(MacOSPlatform_CreateMetalViewNullInput)
{
    // Feeding nullptr must not crash on any platform and must return nullptr.
    void* view = Spark::MacOS::CreateMetalView(nullptr);
    EXPECT_EQ(view, static_cast<void*>(nullptr));
    // DestroyMetalView on nullptr must be a safe no-op.
    Spark::MacOS::DestroyMetalView(nullptr);
    EXPECT_TRUE(true);
}

TEST(MacOSPlatform_ShouldPreferMetalDefault)
{
    // Without an initialized GraphicsEngine, ShouldPreferMetal either returns
    // false (no backend picked yet) or true (Metal auto-detected). Either
    // is a valid answer; the test just pins that the call is safe and
    // returns a bool.
    bool preferMetal = Spark::MacOS::ShouldPreferMetal();
    (void)preferMetal;
    EXPECT_TRUE(true);

#ifndef SPARK_PLATFORM_MACOS
    // Off-macOS the stub is hard-coded to false.
    EXPECT_TRUE(!Spark::MacOS::ShouldPreferMetal());
#endif
}

// ============================================================================
// Executable-path resolution — uses _NSGetExecutablePath on macOS, returns
// empty on other platforms so callers fall back to /proc/self/exe etc.
// ============================================================================

TEST(MacOSPlatform_GetExecutableDirectory)
{
    auto dir = Spark::MacOS::GetExecutableDirectory();
#ifdef SPARK_PLATFORM_MACOS
    // On macOS the helper must return a non-empty, existing directory.
    EXPECT_TRUE(!dir.empty());
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(dir, ec));
#else
    // Off-macOS the stub returns an empty path.
    EXPECT_TRUE(dir.empty());
#endif
}

// ============================================================================
// HybridRTManager scene-feeder API — PushTriangleMesh and ClearTriangleMeshes
// must be safe to call on every platform even without an initialized backend.
// Off-macOS they are no-ops; on macOS without Metal RT live they early-out
// cleanly. This pins the no-crash contract so scene code can push
// unconditionally.
// ============================================================================
#include "Graphics/HybridRT/HybridRTManager.h"

TEST(MacOSPlatform_HybridRTPushIsSafeWithoutInit)
{
    Spark::Graphics::HybridRTManager rt;
    Spark::Graphics::HybridRTManager::TriangleMeshDesc mesh{};
    mesh.name = "dummy";
    // Never initialized — Push and Clear must no-op, not crash.
    rt.PushTriangleMesh(mesh);
    rt.ClearTriangleMeshes();
    EXPECT_TRUE(true);
}
