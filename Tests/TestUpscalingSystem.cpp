/**
 * @file TestUpscalingSystem.cpp
 * @brief Unit tests for the DLSS/FSR/XeSS upscaling system
 */

#include "../SparkEngine/Source/Graphics/UpscalingSystem.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

// ============================================================================
// Test helpers
// ============================================================================

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::cerr << "FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl;                 \
            g_testsFailed++;                                                                                           \
            return;                                                                                                    \
        }                                                                                                              \
    } while (false)

#define TEST_PASS(name)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        g_testsPassed++;                                                                                               \
        std::cout << "  PASS: " << (name) << std::endl;                                                                \
    } while (false)

// ============================================================================
// Tests
// ============================================================================

void TestUpscalingQualityRenderScale()
{
    UpscalingSettings settings;
    settings.displayWidth = 3840;
    settings.displayHeight = 2160;

    // Ultra Performance should render at roughly 33% resolution
    settings.quality = UpscalingQuality::UltraPerformance;
    float scale = settings.GetRenderScale();
    TEST_ASSERT(scale > 0.3f && scale < 0.4f, "UltraPerformance scale should be ~0.33");

    // Performance should render at roughly 50% resolution
    settings.quality = UpscalingQuality::Performance;
    scale = settings.GetRenderScale();
    TEST_ASSERT(scale > 0.45f && scale < 0.55f, "Performance scale should be ~0.50");

    // Quality should render at roughly 67% resolution
    settings.quality = UpscalingQuality::Quality;
    scale = settings.GetRenderScale();
    TEST_ASSERT(scale > 0.6f && scale < 0.7f, "Quality scale should be ~0.67");

    // Native should be 1.0
    settings.quality = UpscalingQuality::Native;
    scale = settings.GetRenderScale();
    TEST_ASSERT(std::abs(scale - 1.0f) < 0.01f, "Native scale should be 1.0");

    TEST_PASS("UpscalingQualityRenderScale");
}

void TestRenderResolutionCalculation()
{
    UpscalingSettings settings;
    settings.displayWidth = 1920;
    settings.displayHeight = 1080;
    settings.quality = UpscalingQuality::Performance;

    auto [w, h] = settings.CalculateRenderResolution();

    // At Performance quality (~50%), 1920x1080 -> ~960x540
    TEST_ASSERT(w > 900 && w < 1000, "Render width should be ~960 for Performance at 1080p");
    TEST_ASSERT(h > 500 && h < 560, "Render height should be ~540 for Performance at 1080p");

    // 4K at Balanced quality (~58%)
    settings.displayWidth = 3840;
    settings.displayHeight = 2160;
    settings.quality = UpscalingQuality::Balanced;
    auto [w2, h2] = settings.CalculateRenderResolution();
    TEST_ASSERT(w2 > 2100 && w2 < 2400, "Render width should be ~2227 for Balanced at 4K");
    TEST_ASSERT(h2 > 1100 && h2 < 1350, "Render height should be ~1253 for Balanced at 4K");

    TEST_PASS("RenderResolutionCalculation");
}

void TestUpscalingModeInputRequirements()
{
    // FSR 1.0 only needs color
    UpscalingInputRequirements fsr1Reqs = UpscalingInputRequirements::ForMode(UpscalingMode::FSR1);
    TEST_ASSERT(fsr1Reqs.requiresColor, "FSR1 should require color");
    TEST_ASSERT(!fsr1Reqs.requiresMotionVectors, "FSR1 should not require motion vectors");
    TEST_ASSERT(!fsr1Reqs.requiresDepth, "FSR1 should not require depth");

    // FSR 2.0 needs color, depth, motion vectors, and jitter
    UpscalingInputRequirements fsr2Reqs = UpscalingInputRequirements::ForMode(UpscalingMode::FSR2);
    TEST_ASSERT(fsr2Reqs.requiresColor, "FSR2 should require color");
    TEST_ASSERT(fsr2Reqs.requiresMotionVectors, "FSR2 should require motion vectors");
    TEST_ASSERT(fsr2Reqs.requiresDepth, "FSR2 should require depth");
    TEST_ASSERT(fsr2Reqs.requiresJitter, "FSR2 should require jitter");

    // DLSS needs similar to FSR 2.0 plus exposure
    UpscalingInputRequirements dlssReqs = UpscalingInputRequirements::ForMode(UpscalingMode::DLSS);
    TEST_ASSERT(dlssReqs.requiresColor, "DLSS should require color");
    TEST_ASSERT(dlssReqs.requiresMotionVectors, "DLSS should require motion vectors");
    TEST_ASSERT(dlssReqs.requiresExposure, "DLSS should require exposure");

    TEST_PASS("UpscalingModeInputRequirements");
}

void TestFSR1Constants()
{
    // Verify FSR 1.0 EASU constants can be constructed
    FSR1EASUConstants easu;
    easu.inputViewportWidth = 960.0f;
    easu.inputViewportHeight = 540.0f;
    easu.inputTextureSizeWidth = 960.0f;
    easu.inputTextureSizeHeight = 540.0f;
    easu.outputSizeWidth = 1920.0f;
    easu.outputSizeHeight = 1080.0f;

    TEST_ASSERT(easu.inputViewportWidth > 0, "EASU viewport width should be positive");
    TEST_ASSERT(easu.outputSizeWidth == 1920.0f, "EASU output width should match");

    // Verify RCAS constants
    FSR1RCASConstants rcas;
    rcas.sharpness = 0.5f;
    TEST_ASSERT(rcas.sharpness >= 0.0f && rcas.sharpness <= 2.0f, "RCAS sharpness in valid range");

    TEST_PASS("FSR1Constants");
}

void TestUpscalingSettingsDefaults()
{
    UpscalingSettings settings;

    TEST_ASSERT(settings.mode == UpscalingMode::None, "Default mode should be None");
    TEST_ASSERT(settings.quality == UpscalingQuality::Quality, "Default quality should be Quality");
    TEST_ASSERT(settings.sharpness >= 0.0f, "Sharpness should be non-negative");

    // When mode is None, render scale should be 1.0
    settings.mode = UpscalingMode::None;
    float scale = settings.GetRenderScale();
    TEST_ASSERT(std::abs(scale - 1.0f) < 0.01f, "None mode should return 1.0 scale");

    TEST_PASS("UpscalingSettingsDefaults");
}

// ============================================================================
// Main
// ============================================================================

int main()
{
    std::cout << "=== Upscaling System Tests ===" << std::endl;

    TestUpscalingQualityRenderScale();
    TestRenderResolutionCalculation();
    TestUpscalingModeInputRequirements();
    TestFSR1Constants();
    TestUpscalingSettingsDefaults();

    std::cout << "\nResults: " << g_testsPassed << " passed, " << g_testsFailed << " failed" << std::endl;
    return g_testsFailed > 0 ? 1 : 0;
}
