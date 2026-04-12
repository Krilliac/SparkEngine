/**
 * @file TestEngineSettingsReal.cpp
 * @brief Real-class tests for EngineSettings singleton
 *
 * Tests the actual production EngineSettings header — default values,
 * typed accessors, generic key access, change callbacks, and reset.
 */

#include "TestFramework.h"
#include "Core/EngineSettings.h"

// ---------------------------------------------------------------------------
// Default values
// ---------------------------------------------------------------------------

TEST(EngineSettingsReal_GraphicsDefaults)
{
    auto& settings = EngineSettings::GetInstance();
    settings.ResetToDefaults();

    EXPECT_EQ(settings.Graphics().windowWidth, 1280);
    EXPECT_EQ(settings.Graphics().windowHeight, 720);
    EXPECT_FALSE(settings.Graphics().fullscreen);
    EXPECT_TRUE(settings.Graphics().vsync);
    EXPECT_EQ(settings.Graphics().antiAliasing, 4);
    EXPECT_EQ(settings.Graphics().shadowQuality, 2);
    EXPECT_NEAR(settings.Graphics().renderScale, 1.0f, 0.001f);
    EXPECT_FALSE(settings.Graphics().hdr);
}

TEST(EngineSettingsReal_AudioDefaults)
{
    auto& settings = EngineSettings::GetInstance();
    settings.ResetToDefaults();

    EXPECT_NEAR(settings.Audio().masterVolume, 1.0f, 0.001f);
    EXPECT_NEAR(settings.Audio().sfxVolume, 0.8f, 0.001f);
    EXPECT_NEAR(settings.Audio().musicVolume, 0.6f, 0.001f);
    EXPECT_NEAR(settings.Audio().voiceVolume, 1.0f, 0.001f);
    EXPECT_TRUE(settings.Audio().muteOnFocusLoss);
    EXPECT_FALSE(settings.Audio().muteAll);
}

TEST(EngineSettingsReal_PhysicsDefaults)
{
    auto& settings = EngineSettings::GetInstance();
    settings.ResetToDefaults();

    EXPECT_NEAR(settings.Physics().gravityX, 0.0f, 0.001f);
    EXPECT_NEAR(settings.Physics().gravityY, -20.0f, 0.001f);
    EXPECT_NEAR(settings.Physics().gravityZ, 0.0f, 0.001f);
    EXPECT_NEAR(settings.Physics().fixedTimestep, 0.016667f, 0.0001f);
    EXPECT_EQ(settings.Physics().maxSubSteps, 4);
    EXPECT_FALSE(settings.Physics().debugDraw);
}

TEST(EngineSettingsReal_NetworkDefaults)
{
    auto& settings = EngineSettings::GetInstance();
    settings.ResetToDefaults();

    EXPECT_EQ(settings.Network().serverPort, 27015);
    EXPECT_EQ(settings.Network().maxClients, 32);
    EXPECT_NEAR(settings.Network().connectionTimeout, 10.0f, 0.001f);
    EXPECT_FALSE(settings.Network().enableCompression);
    EXPECT_FALSE(settings.Network().enableEncryption);
}

TEST(EngineSettingsReal_PlayerDefaults)
{
    auto& settings = EngineSettings::GetInstance();
    settings.ResetToDefaults();

    EXPECT_NEAR(settings.Player().maxHealth, 100.0f, 0.001f);
    EXPECT_NEAR(settings.Player().moveSpeed, 5.0f, 0.001f);
    EXPECT_NEAR(settings.Player().jumpHeight, 3.0f, 0.001f);
    EXPECT_NEAR(settings.Player().sprintMultiplier, 2.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// Typed accessor mutability
// ---------------------------------------------------------------------------

TEST(EngineSettingsReal_MutableAccessors)
{
    auto& settings = EngineSettings::GetInstance();
    settings.ResetToDefaults();

    settings.Graphics().windowWidth = 1920;
    settings.Graphics().windowHeight = 1080;
    EXPECT_EQ(settings.Graphics().windowWidth, 1920);
    EXPECT_EQ(settings.Graphics().windowHeight, 1080);

    settings.Audio().masterVolume = 0.5f;
    EXPECT_NEAR(settings.Audio().masterVolume, 0.5f, 0.001f);

    settings.Physics().gravityY = -9.81f;
    EXPECT_NEAR(settings.Physics().gravityY, -9.81f, 0.001f);
}

// ---------------------------------------------------------------------------
// ResetToDefaults restores all values
// ---------------------------------------------------------------------------

TEST(EngineSettingsReal_ResetToDefaultsRestoresValues)
{
    auto& settings = EngineSettings::GetInstance();

    settings.Graphics().windowWidth = 3840;
    settings.Audio().masterVolume = 0.0f;
    settings.Physics().gravityY = -100.0f;

    settings.ResetToDefaults();

    EXPECT_EQ(settings.Graphics().windowWidth, 1280);
    EXPECT_NEAR(settings.Audio().masterVolume, 1.0f, 0.001f);
    EXPECT_NEAR(settings.Physics().gravityY, -20.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// Const accessor
// ---------------------------------------------------------------------------

TEST(EngineSettingsReal_ConstAccessors)
{
    auto& settings = EngineSettings::GetInstance();
    settings.ResetToDefaults();

    const EngineSettings& constRef = settings;
    EXPECT_EQ(constRef.Graphics().windowWidth, 1280);
    EXPECT_NEAR(constRef.Audio().masterVolume, 1.0f, 0.001f);
    EXPECT_NEAR(constRef.Physics().gravityY, -20.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// All domain sections have valid accessors
// ---------------------------------------------------------------------------

TEST(EngineSettingsReal_AllDomainAccessors)
{
    auto& s = EngineSettings::GetInstance();
    s.ResetToDefaults();

    // Verify each accessor doesn't crash and returns reasonable defaults
    EXPECT_NEAR(s.Controls().mouseSensitivity, 1.0f, 0.001f);
    EXPECT_TRUE(s.Game().showFPS);
    EXPECT_EQ(s.Rendering().renderPath, 1); // Deferred
    EXPECT_TRUE(s.PostProcess().bloomEnabled);
    EXPECT_FALSE(s.SSAO().enabled);
    EXPECT_FALSE(s.SSR().enabled);
    EXPECT_FALSE(s.Volumetric().enabled);
    EXPECT_TRUE(s.TAA().enabled);
    EXPECT_FALSE(s.MotionBlur().enabled);
    EXPECT_TRUE(s.DynamicQuality().enabled);
    EXPECT_TRUE(s.AudioExtended().enable3D);
    EXPECT_NEAR(s.AI().detectionRange, 30.0f, 0.001f);
    EXPECT_NEAR(s.Camera().defaultFov, 90.0f, 0.001f);
    EXPECT_TRUE(s.Editor().snapToGrid);
    EXPECT_TRUE(s.Scripting().hotReloadEnabled);
    EXPECT_TRUE(s.Animation().enableRootMotion);
    EXPECT_TRUE(s.CrashReporting().enabled);
    EXPECT_FALSE(s.Debug().suppressFatalAsserts);
    EXPECT_EQ(s.Weather().weatherType, 0);
    EXPECT_NEAR(s.TimeOfDay().startHour, 12.0f, 0.001f);
    EXPECT_TRUE(s.Streaming().enableStreaming);
    EXPECT_TRUE(s.Performance().enableJobSystem);
    EXPECT_TRUE(s.WorldConfig().enableOriginRebasing);
    EXPECT_TRUE(s.UI().showHUD);
    EXPECT_EQ(s.Accessibility().colorblindMode, 0);
    EXPECT_FALSE(s.VR().enabled);
    EXPECT_NEAR(s.Destruction().debrisLifetime, 10.0f, 0.001f);
    EXPECT_NEAR(s.Dialogue().defaultCooldown, 5.0f, 0.001f);
    EXPECT_FALSE(s.Modding().enableModding);
    EXPECT_TRUE(s.Localization().autoDetectLanguage);
    EXPECT_EQ(s.SaveSystem().maxAutoSaveSlots, 3);
    EXPECT_FALSE(s.Replay().autoRecord);
    EXPECT_TRUE(s.Persistence().enableWAL);
    EXPECT_EQ(s.Particles().maxParticles, 10000);
    EXPECT_TRUE(s.Decals().enableDecals);
    EXPECT_EQ(s.Memory().textureStreamingBudgetMB, 512);
    EXPECT_FALSE(s.OnlineServices().enableOnlineServices);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(EngineSettingsReal_GameModeDefaults)
{
    auto& settings = EngineSettings::GetInstance();
    settings.ResetToDefaults();

    EXPECT_EQ(settings.GameMode().scoreLimit, 50);
    EXPECT_TRUE(settings.GameMode().headshots);
    EXPECT_NEAR(settings.GameMode().headshotMultiplier, 2.0f, 0.001f);
    EXPECT_FALSE(settings.GameMode().friendlyFire);
    EXPECT_EQ(settings.GameMode().killPoints, 100);
}

TEST(EngineSettingsReal_SettingsFlagsEnum)
{
    // Verify enum class values exist and are distinct
    EXPECT_NE(static_cast<uint8_t>(SettingsFlags::Runtime), static_cast<uint8_t>(SettingsFlags::RequiresRestart));
    EXPECT_NE(static_cast<uint8_t>(SettingsFlags::ReadOnly), static_cast<uint8_t>(SettingsFlags::DevOnly));
    EXPECT_EQ(static_cast<uint8_t>(SettingsFlags::Runtime), 0);
    EXPECT_EQ(static_cast<uint8_t>(SettingsFlags::RequiresRestart), 1);
}

TEST(EngineSettingsReal_LoggingDefaults)
{
    auto& settings = EngineSettings::GetInstance();
    settings.ResetToDefaults();

    EXPECT_EQ(settings.Logging().categoryMask, 0xFFFFFFFF);
    // Per-category overrides should be empty by default
    EXPECT_TRUE(settings.Logging().coreLevel.empty());
    EXPECT_TRUE(settings.Logging().graphicsLevel.empty());
}
