// TestAccessibility.cpp - Tests for Spark::Accessibility::AccessibilitySystem
#include "TestFramework.h"
#include "Engine/Accessibility/AccessibilitySystem.h"

using namespace Spark::Accessibility;

// ============================================================================
// Default settings
// ============================================================================

TEST(Accessibility_DefaultSettings_ColorblindNone)
{
    auto& system = AccessibilitySystem::GetInstance();
    system.Initialize();

    const auto& settings = system.GetSettings();
    EXPECT_EQ(static_cast<uint8_t>(settings.colorblindMode), static_cast<uint8_t>(ColorblindMode::None));

    system.Shutdown();
}

TEST(Accessibility_DefaultSettings_TextScaleOne)
{
    auto& system = AccessibilitySystem::GetInstance();
    system.Initialize();

    const auto& settings = system.GetSettings();
    EXPECT_NEAR(settings.textScaleFactor, 1.0f, 0.001f);

    system.Shutdown();
}

TEST(Accessibility_DefaultSettings_SubtitlesOff)
{
    auto& system = AccessibilitySystem::GetInstance();
    system.Initialize();

    const auto& settings = system.GetSettings();
    EXPECT_FALSE(settings.subtitlesEnabled);

    system.Shutdown();
}

TEST(Accessibility_DefaultSettings_HighContrastOff)
{
    auto& system = AccessibilitySystem::GetInstance();
    system.Initialize();

    const auto& settings = system.GetSettings();
    EXPECT_FALSE(settings.highContrastUI);

    system.Shutdown();
}

// ============================================================================
// SetSettings / GetSettings roundtrip
// ============================================================================

TEST(Accessibility_SetSettings_GetSettings_Roundtrip)
{
    auto& system = AccessibilitySystem::GetInstance();
    system.Initialize();

    AccessibilitySettings s;
    s.colorblindMode = ColorblindMode::Deuteranopia;
    s.highContrastUI = true;
    s.largeText = true;
    s.textScaleFactor = 1.5f;
    s.subtitlesEnabled = true;
    s.reducedMotion = true;
    system.SetSettings(s);

    const auto& result = system.GetSettings();
    EXPECT_EQ(static_cast<uint8_t>(result.colorblindMode), static_cast<uint8_t>(ColorblindMode::Deuteranopia));
    EXPECT_TRUE(result.highContrastUI);
    EXPECT_TRUE(result.largeText);
    EXPECT_NEAR(result.textScaleFactor, 1.5f, 0.001f);
    EXPECT_TRUE(result.subtitlesEnabled);
    EXPECT_TRUE(result.reducedMotion);

    system.Shutdown();
}

// ============================================================================
// GetColorblindCorrectionMatrix
// ============================================================================

TEST(Accessibility_ColorMatrix_None_IsIdentity)
{
    auto mat = AccessibilitySystem::GetColorblindCorrectionMatrix(ColorblindMode::None);

    // Identity: diag = 1, off-diag = 0
    EXPECT_NEAR(mat[0], 1.0f, 0.001f);
    EXPECT_NEAR(mat[1], 0.0f, 0.001f);
    EXPECT_NEAR(mat[2], 0.0f, 0.001f);
    EXPECT_NEAR(mat[3], 0.0f, 0.001f);
    EXPECT_NEAR(mat[4], 1.0f, 0.001f);
    EXPECT_NEAR(mat[5], 0.0f, 0.001f);
    EXPECT_NEAR(mat[6], 0.0f, 0.001f);
    EXPECT_NEAR(mat[7], 0.0f, 0.001f);
    EXPECT_NEAR(mat[8], 1.0f, 0.001f);
}

TEST(Accessibility_ColorMatrix_Protanopia_NotIdentity)
{
    auto mat = AccessibilitySystem::GetColorblindCorrectionMatrix(ColorblindMode::Protanopia);
    // Protanopia matrix differs from identity
    EXPECT_TRUE(mat[0] != 1.0f || mat[4] != 1.0f || mat[8] != 1.0f);
}

// ============================================================================
// ApplyColorblindFilter
// ============================================================================

TEST(Accessibility_ApplyColorblindFilter_None_PreservesColor)
{
    auto& system = AccessibilitySystem::GetInstance();
    system.Initialize();

    // Default mode is None, so filter should pass through
    float rgbIn[3] = {0.5f, 0.7f, 0.3f};
    float rgbOut[3] = {};
    system.ApplyColorblindFilter(rgbOut, rgbIn);

    EXPECT_NEAR(rgbOut[0], 0.5f, 0.001f);
    EXPECT_NEAR(rgbOut[1], 0.7f, 0.001f);
    EXPECT_NEAR(rgbOut[2], 0.3f, 0.001f);

    system.Shutdown();
}

TEST(Accessibility_ApplyColorblindFilter_Deuteranopia_ModifiesColor)
{
    auto& system = AccessibilitySystem::GetInstance();
    system.Initialize();

    AccessibilitySettings s;
    s.colorblindMode = ColorblindMode::Deuteranopia;
    s.colorblindStrength = 1.0f;
    system.SetSettings(s);

    float rgbIn[3] = {1.0f, 0.0f, 0.0f};
    float rgbOut[3] = {};
    system.ApplyColorblindFilter(rgbOut, rgbIn);

    // With deuteranopia correction, pure red should be modified
    bool modified = (rgbOut[0] != rgbIn[0]) || (rgbOut[1] != rgbIn[1]) || (rgbOut[2] != rgbIn[2]);
    EXPECT_TRUE(modified);

    system.Shutdown();
}

// ============================================================================
// PushSubtitle
// ============================================================================

TEST(Accessibility_PushSubtitle_AddsToActiveList)
{
    auto& system = AccessibilitySystem::GetInstance();
    system.Initialize();

    AccessibilitySettings s;
    s.subtitlesEnabled = true;
    system.SetSettings(s);

    SubtitleEntry entry;
    entry.text = "Hello World";
    entry.speaker = "Narrator";
    entry.duration = 5.0f;
    system.PushSubtitle(entry);

    const auto& subs = system.GetActiveSubtitles();
    EXPECT_EQ(subs.size(), 1u);
    EXPECT_TRUE(subs[0].text == "Hello World");
    EXPECT_TRUE(subs[0].speaker == "Narrator");

    system.Shutdown();
}

TEST(Accessibility_PushSubtitle_SubtitlesDisabled_NotAdded)
{
    auto& system = AccessibilitySystem::GetInstance();
    system.Initialize();

    // Default: subtitles disabled
    SubtitleEntry entry;
    entry.text = "Should not appear";
    entry.duration = 5.0f;
    system.PushSubtitle(entry);

    const auto& subs = system.GetActiveSubtitles();
    EXPECT_TRUE(subs.empty());

    system.Shutdown();
}

TEST(Accessibility_PushSubtitle_ExpiredByUpdate)
{
    auto& system = AccessibilitySystem::GetInstance();
    system.Initialize();

    AccessibilitySettings s;
    s.subtitlesEnabled = true;
    system.SetSettings(s);

    SubtitleEntry entry;
    entry.text = "Brief";
    entry.duration = 1.0f;
    system.PushSubtitle(entry);

    EXPECT_EQ(system.GetActiveSubtitles().size(), 1u);

    // Advance time past the subtitle's duration
    system.Update(2.0f);
    EXPECT_TRUE(system.GetActiveSubtitles().empty());

    system.Shutdown();
}

// ============================================================================
// IsFeatureEnabled / SetFeatureEnabled
// ============================================================================

TEST(Accessibility_FeatureFlag_DefaultDisabled)
{
    auto& system = AccessibilitySystem::GetInstance();
    system.Initialize();

    EXPECT_FALSE(system.IsFeatureEnabled("screen_magnifier"));

    system.Shutdown();
}

TEST(Accessibility_FeatureFlag_SetAndGet)
{
    auto& system = AccessibilitySystem::GetInstance();
    system.Initialize();

    system.SetFeatureEnabled("screen_magnifier", true);
    EXPECT_TRUE(system.IsFeatureEnabled("screen_magnifier"));

    system.SetFeatureEnabled("screen_magnifier", false);
    EXPECT_FALSE(system.IsFeatureEnabled("screen_magnifier"));

    system.Shutdown();
}

// ============================================================================
// Console_GetStatus
// ============================================================================

TEST(Accessibility_Console_GetStatus_NonEmpty)
{
    auto& system = AccessibilitySystem::GetInstance();
    system.Initialize();

    auto status = system.Console_GetStatus();
    EXPECT_FALSE(status.empty());
    EXPECT_STR_CONTAINS(status, "initialized");

    system.Shutdown();
}
