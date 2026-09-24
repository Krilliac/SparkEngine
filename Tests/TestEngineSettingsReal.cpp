/**
 * @file TestEngineSettingsReal.cpp
 * @brief Real-class tests for EngineSettings singleton
 *
 * Tests the actual production EngineSettings header — default values,
 * typed accessors, generic key access, change callbacks, and reset.
 */

#include "TestFramework.h"
#include "Core/EngineSettings.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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

TEST(EngineSettingsReal_LoadAndSaveFailuresAreTruthfulAndTransactional)
{
    namespace fs = std::filesystem;
    const fs::path testRoot = fs::temp_directory_path() / "spark_engine_settings_contract";
    std::error_code error;
    fs::remove_all(testRoot, error);
    EXPECT_TRUE(fs::create_directories(testRoot));

    const fs::path settingsPath = testRoot / "settings.ini";
    auto& settings = EngineSettings::GetInstance();
    settings.Graphics().windowWidth = 999;
    EXPECT_TRUE(settings.Load(settingsPath.string()));
    EXPECT_TRUE(fs::exists(settingsPath));
    EXPECT_EQ(settings.Graphics().windowWidth, 1280);

    {
        std::ofstream file(settingsPath);
        file << "[Graphics]\nWindowWidth = 1600\n";
    }

    EXPECT_TRUE(settings.Load(settingsPath.string()));
    EXPECT_EQ(settings.Graphics().windowWidth, 1600);
    EXPECT_EQ(settings.GetFilePath(), settingsPath.string());

    {
        std::ofstream file(settingsPath, std::ios::trunc);
        file << "[Graphics\nWindowWidth = 1920\n";
    }
    EXPECT_FALSE(settings.Load(settingsPath.string()));
    EXPECT_EQ(settings.Graphics().windowWidth, 1600);
    EXPECT_EQ(settings.GetFilePath(), settingsPath.string());

    const fs::path localPath = testRoot / "settings.local.ini";
    {
        std::ofstream mainFile(settingsPath, std::ios::trunc);
        mainFile << "[Graphics]\nWindowWidth = 1920\n";
        std::ofstream localFile(localPath);
        localFile << "not valid ini\n";
    }
    settings.Graphics().windowWidth = 1700;
    EXPECT_FALSE(settings.Load(settingsPath.string()));
    EXPECT_EQ(settings.Graphics().windowWidth, 1700);
    EXPECT_EQ(settings.GetFilePath(), settingsPath.string());

    const fs::path missingParentPath = testRoot / "missing" / "settings.ini";
    EXPECT_FALSE(settings.Load(missingParentPath.string()));
    EXPECT_EQ(settings.Graphics().windowWidth, 1700);
    EXPECT_EQ(settings.GetFilePath(), settingsPath.string());

    const fs::path directoryTarget = testRoot / "directory-target";
    EXPECT_TRUE(fs::create_directory(directoryTarget));
    EXPECT_FALSE(settings.SaveAs(directoryTarget.string()));
    EXPECT_EQ(settings.Graphics().windowWidth, 1700);

    fs::remove_all(testRoot, error);
    settings.ResetToDefaults();
}

// ---------------------------------------------------------------------------
// OPS-100: retired crash-transport credentials must not survive a load/save
// ---------------------------------------------------------------------------

TEST(CrashSettings_RetiredTransportCredentialsAreDroppedOnLoadAndNeverWritten)
{
    namespace fs = std::filesystem;
    const fs::path testRoot = fs::temp_directory_path() / "spark_engine_settings_ops100_credentials";
    std::error_code error;
    fs::remove_all(testRoot, error);
    EXPECT_TRUE(fs::create_directories(testRoot));

    // An install that predates the removal of the in-process uploader: both
    // spellings the engine ever shipped, plus a secret only in the gitignored
    // local override file (which is merged into the main config on load).
    const fs::path settingsPath = testRoot / "settings.ini";
    {
        std::ofstream file(settingsPath);
        file << "[CrashReporting]\n"
                "Enabled = true\n"
                "CaptureScreenshot = false\n"
                "UploadURL = https://user:upload-secret@crash.example.invalid/capability\n"
                "ProxyURL = https://relay.example.invalid/proxy-capability\n"
                "GitHubRepo = owner/repo\n"
                "GitHubToken = ghp_legacyTokenValue0123456789\n"
                "GithubToken = ghp_reflectedTokenValue0123456789\n"
                "GitHubLabels = crash-report\n"
                "AttachDump = true\n"
                "TimeoutSeconds = 5\n"
                "SmtpUser = someone@example.invalid\n"
                "SmtpPass = smtp-main-password\n"
                "EmailTo = someone@example.invalid\n"
                "EmailFrom = crashreporter@sparkengine.dev\n";
    }
    {
        std::ofstream localFile(testRoot / "settings.local.ini");
        localFile << "[CrashReporting]\nSMTPPASS = smtp-local-password\n";
    }

    auto& settings = EngineSettings::GetInstance();
    EXPECT_TRUE(settings.Load(settingsPath.string()));

    // Non-credential settings in the same section still apply.
    EXPECT_TRUE(settings.CrashReporting().enabled);
    EXPECT_FALSE(settings.CrashReporting().captureScreenshot);

    // The retired keys are gone from the live config, whatever their casing.
    for (const char* key : {"UploadURL", "ProxyURL", "GitHubRepo", "GitHubToken", "GithubToken", "GitHubLabels",
                            "AttachDump", "TimeoutSeconds", "SmtpUser", "SmtpPass", "SMTPPASS", "EmailTo", "EmailFrom"})
    {
        EXPECT_TRUE(settings.GetValue("CrashReporting", key).empty());
    }

    // A later save must not write any of them back to disk.
    EXPECT_TRUE(settings.Save());
    std::string written;
    {
        std::ifstream file(settingsPath, std::ios::binary);
        written.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    EXPECT_FALSE(written.empty());
    for (const char* secret : {"ghp_", "upload-secret", "proxy-capability", "smtp-main-password", "smtp-local-password",
                               "someone@example.invalid"})
    {
        EXPECT_TRUE(written.find(secret) == std::string::npos);
    }
    for (const char* retiredKey :
         {"UploadURL", "ProxyURL", "GitHubToken", "GithubToken", "SmtpPass", "SmtpUser", "EmailTo", "TimeoutSeconds"})
    {
        EXPECT_TRUE(written.find(retiredKey) == std::string::npos);
    }
    EXPECT_TRUE(written.find("CaptureScreenshot") != std::string::npos);

    fs::remove_all(testRoot, error);
    settings.ResetToDefaults();
}
