/**
 * @file EngineSettings.cpp
 * @brief Implementation of centralized engine settings
 */

#include "EngineSettings.h"
#include "Utils/Assert.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"
#include "Utils/Validate.h"
#include <filesystem>
#include <sstream>

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif

// =============================================================================
// Singleton
// =============================================================================
EngineSettings& EngineSettings::GetInstance()
{
    static EngineSettings instance;
    return instance;
}

// =============================================================================
// Find settings path relative to executable
// =============================================================================
std::string EngineSettings::FindSettingsPath() const
{
#ifdef SPARK_PLATFORM_WINDOWS
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
#else
    auto dir = std::filesystem::current_path();
#endif
    // Check Resources/Config/ first, then fall back to exe directory
    auto configDir = dir / "Resources" / "Config";
    if (std::filesystem::exists(configDir / "settings.ini"))
        return (configDir / "settings.ini").string();

    // Check one level up (common in development builds)
    auto parentConfig = dir.parent_path() / "Resources" / "Config";
    if (std::filesystem::exists(parentConfig / "settings.ini"))
        return (parentConfig / "settings.ini").string();

    // Default: create in Resources/Config
    std::filesystem::create_directories(configDir);
    return (configDir / "settings.ini").string();
}

// =============================================================================
// Load
// =============================================================================
bool EngineSettings::Load(const std::string& path)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);

    m_filePath = path.empty() ? FindSettingsPath() : path;
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Loading engine settings from: %s", m_filePath.c_str());

    if (m_config.Load(m_filePath))
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Engine settings loaded successfully");

        // Load local override file (gitignored — safe for secrets)
        std::string localPath = m_filePath;
        auto dotPos = localPath.rfind('.');
        if (dotPos != std::string::npos)
            localPath = localPath.substr(0, dotPos) + ".local" + localPath.substr(dotPos);
        else
            localPath += ".local";

        if (std::filesystem::exists(localPath))
        {
            Spark::ConfigParser localConfig;
            if (localConfig.Load(localPath))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Core, "Loaded local settings override: %s", localPath.c_str());
                // Merge local values into the main config (local takes precedence)
                for (const auto& section : localConfig.GetSections())
                {
                    for (const auto& key : localConfig.GetKeys(section))
                    {
                        std::string val = localConfig.GetString(section, key, "");
                        if (!val.empty())
                            m_config.SetString(section, key, val);
                    }
                }
            }
        }

        ReadFromConfig();
        ApplyDebugSettings();
        return true;
    }

    // File doesn't exist or failed to parse - use defaults
    SPARK_LOG_WARN(Spark::LogCategory::Core, "Settings file not found or failed to parse, using defaults: %s",
                   m_filePath.c_str());
    PopulateDefaults();
    ReadFromConfig();
    ApplyDebugSettings();

    // Save defaults so the file exists for next time
    Save();
    return true;
}

// =============================================================================
// Save
// =============================================================================
bool EngineSettings::Save() const
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);

    SPARK_LOG_INFO(Spark::LogCategory::Core, "Saving engine settings to: %s", m_filePath.c_str());
    WriteToConfig();
    bool result = m_config.Save(m_filePath);
    if (!result)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "Failed to save engine settings to: %s", m_filePath.c_str());
    }
    return result;
}

bool EngineSettings::SaveAs(const std::string& path) const
{
    WriteToConfig();
    return m_config.Save(path);
}

// =============================================================================
// Reset
// =============================================================================
void EngineSettings::ResetToDefaults()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Resetting all engine settings to defaults");

    m_graphics = GraphicsSettings{};
    m_audio = AudioSettings{};
    m_controls = ControlsSettings{};
    m_game = GameSettings{};
    m_rendering = RenderingSettings{};
    m_postProcess = PostProcessSettings{};
    m_ssao = SSAOSettings{};
    m_ssr = SSRSettings{};
    m_volumetric = VolumetricSettings{};
    m_taa = TAASettings{};
    m_motionBlur = MotionBlurSettings{};
    m_dynamicQuality = DynamicQualitySettings{};
    m_audioExtended = AudioExtendedSettings{};
    m_physics = PhysicsSettings{};
    m_ai = AISettings{};
    m_player = PlayerSettings{};
    m_gameMode = GameModeSettings{};
    m_camera = CameraSettings{};
    m_editor = EditorSettings{};
    m_network = NetworkSettings{};
    m_scripting = ScriptingSettings{};
    m_animation = AnimationSettings{};
    m_crashReporting = CrashReportingSettings{};
    m_weather = WeatherSettings{};
    m_timeOfDay = TimeOfDaySettings{};
    m_streaming = StreamingSettings{};
    m_performance = PerformanceSettings{};
    m_world = WorldSettings{};
    m_ui = UISettings{};
    m_logging = LoggingSettings{};
    PopulateDefaults();
}

// =============================================================================
// Apply debug settings to the Assert system
// =============================================================================
void EngineSettings::ApplyDebugSettings()
{
    Assert::SetSuppressionEnabled(m_debug.suppressFatalAsserts);
    Assert::SetDebugBreakOnSuppressed(m_debug.breakOnSuppressedAsserts);
}

// =============================================================================
// Read struct fields from ConfigParser
// =============================================================================
void EngineSettings::ReadFromConfig()
{
    // Graphics
    m_graphics.windowWidth = m_config.GetInt("Graphics", "WindowWidth", 1280);
    m_graphics.windowHeight = m_config.GetInt("Graphics", "WindowHeight", 720);
    m_graphics.fullscreen = m_config.GetBool("Graphics", "Fullscreen", false);
    m_graphics.vsync = m_config.GetBool("Graphics", "VSync", true);
    m_graphics.antiAliasing = m_config.GetInt("Graphics", "AntiAliasing", 4);
    m_graphics.shadowQuality = m_config.GetInt("Graphics", "ShadowQuality", 2);
    m_graphics.renderScale = m_config.GetFloat("Graphics", "RenderScale", 1.0f);
    m_graphics.hdr = m_config.GetBool("Graphics", "HDR", false);

    // Audio
    m_audio.masterVolume = m_config.GetFloat("Audio", "MasterVolume", 1.0f);
    m_audio.sfxVolume = m_config.GetFloat("Audio", "SFXVolume", 0.8f);
    m_audio.musicVolume = m_config.GetFloat("Audio", "MusicVolume", 0.6f);
    m_audio.voiceVolume = m_config.GetFloat("Audio", "VoiceVolume", 1.0f);
    m_audio.muteOnFocusLoss = m_config.GetBool("Audio", "MuteOnFocusLoss", true);

    // Controls
    m_controls.mouseSensitivity = m_config.GetFloat("Controls", "MouseSensitivity", 1.0f);
    m_controls.invertMouseY = m_config.GetBool("Controls", "InvertMouse", false);
    m_controls.mouseDeadZone = m_config.GetFloat("Controls", "MouseDeadZone", 0.0f);
    m_controls.rawMouseInput = m_config.GetBool("Controls", "RawMouseInput", false);
    m_controls.mouseAcceleration = m_config.GetBool("Controls", "MouseAcceleration", false);

    // Game
    m_game.difficulty = m_config.GetString("Game", "Difficulty", "Normal");
    m_game.showFPS = m_config.GetBool("Game", "ShowFPS", true);
    m_game.showDebugInfo = m_config.GetBool("Game", "ShowDebugInfo", false);
    m_game.fieldOfView = m_config.GetFloat("Game", "FieldOfView", 90.0f);

    // Rendering
    m_rendering.renderPath = m_config.GetInt("Rendering", "RenderPath", 1);
    m_rendering.qualityPreset = m_config.GetInt("Rendering", "QualityPreset", 2);
    m_rendering.maxTextureSize = m_config.GetInt("Rendering", "MaxTextureSize", 2048);
    m_rendering.anisotropicFiltering = m_config.GetBool("Rendering", "AnisotropicFiltering", true);
    m_rendering.anisotropyLevel = m_config.GetInt("Rendering", "AnisotropyLevel", 16);
    m_rendering.shadows = m_config.GetBool("Rendering", "Shadows", true);
    m_rendering.shadowMapSize = m_config.GetInt("Rendering", "ShadowMapSize", 2048);
    m_rendering.cascadeCount = m_config.GetInt("Rendering", "CascadeCount", 3);
    m_rendering.bloom = m_config.GetBool("Rendering", "Bloom", true);
    m_rendering.ssao = m_config.GetBool("Rendering", "SSAO", false);
    m_rendering.taa = m_config.GetBool("Rendering", "TAA", false);
    m_rendering.motionBlur = m_config.GetBool("Rendering", "MotionBlur", false);
    m_rendering.frustumCulling = m_config.GetBool("Rendering", "FrustumCulling", true);
    m_rendering.occlusionCulling = m_config.GetBool("Rendering", "OcclusionCulling", false);
    m_rendering.portalCulling = m_config.GetBool("Rendering", "PortalCulling", false);
    m_rendering.levelOfDetail = m_config.GetBool("Rendering", "LevelOfDetail", true);
    m_rendering.maxDrawCalls = m_config.GetInt("Rendering", "MaxDrawCalls", 1000);
    m_rendering.wireframeMode = m_config.GetBool("Rendering", "WireframeMode", false);
    m_rendering.debugMode = m_config.GetBool("Rendering", "DebugMode", false);
    m_rendering.enableGPUTiming = m_config.GetBool("Rendering", "EnableGPUTiming", false);

    // PostProcess
    m_postProcess.bloomEnabled = m_config.GetBool("PostProcess", "BloomEnabled", true);
    m_postProcess.bloomThreshold = m_config.GetFloat("PostProcess", "BloomThreshold", 1.0f);
    m_postProcess.bloomIntensity = m_config.GetFloat("PostProcess", "BloomIntensity", 1.0f);
    m_postProcess.bloomRadius = m_config.GetFloat("PostProcess", "BloomRadius", 1.0f);
    m_postProcess.bloomSoftKnee = m_config.GetFloat("PostProcess", "BloomSoftKnee", 0.5f);
    m_postProcess.bloomIterations = m_config.GetInt("PostProcess", "BloomIterations", 6);
    m_postProcess.toneMappingOperator = m_config.GetInt("PostProcess", "ToneMappingOperator", 4);
    m_postProcess.exposure = m_config.GetFloat("PostProcess", "Exposure", 1.0f);
    m_postProcess.gamma = m_config.GetFloat("PostProcess", "Gamma", 2.2f);
    m_postProcess.whitePoint = m_config.GetFloat("PostProcess", "WhitePoint", 11.2f);
    m_postProcess.colorGradingEnabled = m_config.GetBool("PostProcess", "ColorGradingEnabled", false);
    m_postProcess.temperature = m_config.GetFloat("PostProcess", "Temperature", 0.0f);
    m_postProcess.tint = m_config.GetFloat("PostProcess", "Tint", 0.0f);
    m_postProcess.contrast = m_config.GetFloat("PostProcess", "Contrast", 1.0f);
    m_postProcess.brightness = m_config.GetFloat("PostProcess", "Brightness", 0.0f);
    m_postProcess.saturation = m_config.GetFloat("PostProcess", "Saturation", 1.0f);

    // SSAO
    m_ssao.enabled = m_config.GetBool("SSAO", "Enabled", false);
    m_ssao.radius = m_config.GetFloat("SSAO", "Radius", 0.5f);
    m_ssao.intensity = m_config.GetFloat("SSAO", "Intensity", 1.0f);
    m_ssao.sampleCount = m_config.GetInt("SSAO", "SampleCount", 16);
    m_ssao.bias = m_config.GetFloat("SSAO", "Bias", 0.025f);
    m_ssao.blur = m_config.GetBool("SSAO", "Blur", true);

    // SSR
    m_ssr.enabled = m_config.GetBool("SSR", "Enabled", false);
    m_ssr.maxDistance = m_config.GetFloat("SSR", "MaxDistance", 100.0f);
    m_ssr.maxSteps = m_config.GetInt("SSR", "MaxSteps", 32);
    m_ssr.thickness = m_config.GetFloat("SSR", "Thickness", 0.5f);
    m_ssr.fadeStart = m_config.GetFloat("SSR", "FadeStart", 80.0f);
    m_ssr.fadeEnd = m_config.GetFloat("SSR", "FadeEnd", 100.0f);

    // Volumetric
    m_volumetric.enabled = m_config.GetBool("Volumetric", "Enabled", false);
    m_volumetric.sampleCount = m_config.GetInt("Volumetric", "SampleCount", 32);
    m_volumetric.scattering = m_config.GetFloat("Volumetric", "Scattering", 0.1f);
    m_volumetric.extinction = m_config.GetFloat("Volumetric", "Extinction", 0.01f);
    m_volumetric.anisotropy = m_config.GetFloat("Volumetric", "Anisotropy", 0.3f);

    // TAA
    m_taa.enabled = m_config.GetBool("TAA", "Enabled", true);
    m_taa.quality = m_config.GetInt("TAA", "Quality", 2);
    m_taa.jitterPattern = m_config.GetInt("TAA", "JitterPattern", 0);
    m_taa.jitterSequenceLength = m_config.GetInt("TAA", "JitterSequenceLength", 16);
    m_taa.historyBlendFactor = m_config.GetFloat("TAA", "HistoryBlendFactor", 0.9f);
    m_taa.varianceClipGamma = m_config.GetFloat("TAA", "VarianceClipGamma", 1.0f);
    m_taa.useMotionVectors = m_config.GetBool("TAA", "UseMotionVectors", true);
    m_taa.useYCoCg = m_config.GetBool("TAA", "UseYCoCg", true);
    m_taa.sharpness = m_config.GetFloat("TAA", "Sharpness", 0.0f);
    m_taa.ghostingRejectionStrength = m_config.GetFloat("TAA", "GhostingRejectionStrength", 0.8f);
    m_taa.flickerReduction = m_config.GetFloat("TAA", "FlickerReduction", 0.5f);

    // MotionBlur
    m_motionBlur.enabled = m_config.GetBool("MotionBlur", "Enabled", false);
    m_motionBlur.type = m_config.GetInt("MotionBlur", "Type", 2);
    m_motionBlur.intensity = m_config.GetFloat("MotionBlur", "Intensity", 0.5f);
    m_motionBlur.sampleCount = m_config.GetInt("MotionBlur", "SampleCount", 8);
    m_motionBlur.maxBlurRadius = m_config.GetFloat("MotionBlur", "MaxBlurRadius", 32.0f);
    m_motionBlur.velocityScale = m_config.GetFloat("MotionBlur", "VelocityScale", 1.0f);
    m_motionBlur.minVelocityThreshold = m_config.GetFloat("MotionBlur", "MinVelocityThreshold", 0.5f);
    m_motionBlur.cameraRotationScale = m_config.GetFloat("MotionBlur", "CameraRotationScale", 0.5f);
    m_motionBlur.cameraTranslationScale = m_config.GetFloat("MotionBlur", "CameraTranslationScale", 1.0f);
    m_motionBlur.tileSize = m_config.GetInt("MotionBlur", "TileSize", 20);

    // DynamicQuality
    m_dynamicQuality.enabled = m_config.GetBool("DynamicQuality", "Enabled", true);
    m_dynamicQuality.targetFrameTimeMs = m_config.GetFloat("DynamicQuality", "TargetFrameTimeMs", 16.67f);
    m_dynamicQuality.minRenderScale = m_config.GetFloat("DynamicQuality", "MinRenderScale", 0.5f);
    m_dynamicQuality.maxRenderScale = m_config.GetFloat("DynamicQuality", "MaxRenderScale", 1.0f);
    m_dynamicQuality.renderScaleStep = m_config.GetFloat("DynamicQuality", "RenderScaleStep", 0.05f);
    m_dynamicQuality.minShadowScale = m_config.GetFloat("DynamicQuality", "MinShadowScale", 0.25f);
    m_dynamicQuality.maxShadowScale = m_config.GetFloat("DynamicQuality", "MaxShadowScale", 1.0f);
    m_dynamicQuality.shadowScaleStep = m_config.GetFloat("DynamicQuality", "ShadowScaleStep", 0.25f);
    m_dynamicQuality.minLodBias = m_config.GetFloat("DynamicQuality", "MinLodBias", 0.5f);
    m_dynamicQuality.maxLodBias = m_config.GetFloat("DynamicQuality", "MaxLodBias", 1.0f);
    m_dynamicQuality.lodBiasStep = m_config.GetFloat("DynamicQuality", "LodBiasStep", 0.1f);
    m_dynamicQuality.minTextureMipBias = m_config.GetFloat("DynamicQuality", "MinTextureMipBias", 0.0f);
    m_dynamicQuality.maxTextureMipBias = m_config.GetFloat("DynamicQuality", "MaxTextureMipBias", 4.0f);
    m_dynamicQuality.textureMipBiasStep = m_config.GetFloat("DynamicQuality", "TextureMipBiasStep", 1.0f);
    m_dynamicQuality.frameTimeWindowSize = m_config.GetInt("DynamicQuality", "FrameTimeWindowSize", 30);
    m_dynamicQuality.minChangeIntervalFrames = m_config.GetInt("DynamicQuality", "MinChangeIntervalFrames", 15);
    m_dynamicQuality.increaseThresholdMs = m_config.GetFloat("DynamicQuality", "IncreaseThresholdMs", 2.0f);
    m_dynamicQuality.pidKP = m_config.GetFloat("DynamicQuality", "PID_KP", 0.5f);
    m_dynamicQuality.pidKI = m_config.GetFloat("DynamicQuality", "PID_KI", 0.05f);
    m_dynamicQuality.pidKD = m_config.GetFloat("DynamicQuality", "PID_KD", 0.1f);

    // AudioExtended
    m_audioExtended.dopplerScale = m_config.GetFloat("AudioExtended", "DopplerScale", 1.0f);
    m_audioExtended.distanceScale = m_config.GetFloat("AudioExtended", "DistanceScale", 1.0f);
    m_audioExtended.enable3D = m_config.GetBool("AudioExtended", "Enable3D", true);
    m_audioExtended.enableReverb = m_config.GetBool("AudioExtended", "EnableReverb", false);
    m_audioExtended.enableEAX = m_config.GetBool("AudioExtended", "EnableEAX", false);
    m_audioExtended.maxSources = m_config.GetInt("AudioExtended", "MaxSources", 32);

    // Physics
    m_physics.gravityX = m_config.GetFloat("Physics", "GravityX", 0.0f);
    m_physics.gravityY = m_config.GetFloat("Physics", "GravityY", -20.0f);
    m_physics.gravityZ = m_config.GetFloat("Physics", "GravityZ", 0.0f);
    m_physics.fixedTimestep = m_config.GetFloat("Physics", "FixedTimestep", 0.016667f);
    m_physics.maxSubSteps = m_config.GetInt("Physics", "MaxSubSteps", 4);
    m_physics.defaultFriction = m_config.GetFloat("Physics", "DefaultFriction", 0.5f);
    m_physics.defaultRestitution = m_config.GetFloat("Physics", "DefaultRestitution", 0.3f);
    m_physics.defaultLinearDamping = m_config.GetFloat("Physics", "DefaultLinearDamping", 0.0f);
    m_physics.defaultAngularDamping = m_config.GetFloat("Physics", "DefaultAngularDamping", 0.05f);
    m_physics.debugDraw = m_config.GetBool("Physics", "DebugDraw", false);

    // AI
    m_ai.detectionRange = m_config.GetFloat("AI", "DetectionRange", 30.0f);
    m_ai.attackRange = m_config.GetFloat("AI", "AttackRange", 15.0f);
    m_ai.meleeRange = m_config.GetFloat("AI", "MeleeRange", 2.0f);
    m_ai.moveSpeed = m_config.GetFloat("AI", "MoveSpeed", 5.0f);
    m_ai.turnSpeed = m_config.GetFloat("AI", "TurnSpeed", 180.0f);
    m_ai.accuracy = m_config.GetFloat("AI", "Accuracy", 0.7f);
    m_ai.reactionTime = m_config.GetFloat("AI", "ReactionTime", 0.3f);
    m_ai.coverSearchRadius = m_config.GetFloat("AI", "CoverSearchRadius", 20.0f);
    m_ai.canStrafe = m_config.GetBool("AI", "CanStrafe", true);
    m_ai.canSprint = m_config.GetBool("AI", "CanSprint", true);
    m_ai.canUseCover = m_config.GetBool("AI", "CanUseCover", true);

    // Player
    m_player.maxHealth = m_config.GetFloat("Player", "MaxHealth", 100.0f);
    m_player.maxArmor = m_config.GetFloat("Player", "MaxArmor", 100.0f);
    m_player.moveSpeed = m_config.GetFloat("Player", "MoveSpeed", 5.0f);
    m_player.jumpHeight = m_config.GetFloat("Player", "JumpHeight", 3.0f);
    m_player.gravityForce = m_config.GetFloat("Player", "GravityForce", 20.0f);
    m_player.friction = m_config.GetFloat("Player", "Friction", 0.9f);
    m_player.sprintMultiplier = m_config.GetFloat("Player", "SprintMultiplier", 2.0f);
    m_player.crouchMultiplier = m_config.GetFloat("Player", "CrouchMultiplier", 0.5f);
    m_player.adsSpeedMultiplier = m_config.GetFloat("Player", "ADSSpeedMultiplier", 0.5f);
    m_player.maxShield = m_config.GetFloat("Player", "MaxShield", 50.0f);
    m_player.shieldRechargeRate = m_config.GetFloat("Player", "ShieldRechargeRate", 10.0f);
    m_player.shieldRechargeDelay = m_config.GetFloat("Player", "ShieldRechargeDelay", 6.0f);
    m_player.maxEnergy = m_config.GetFloat("Player", "MaxEnergy", 100.0f);
    m_player.energyRegenRate = m_config.GetFloat("Player", "EnergyRegenRate", 10.0f);

    // GameMode
    m_gameMode.scoreLimit = m_config.GetInt("GameMode", "ScoreLimit", 50);
    m_gameMode.roundLimit = m_config.GetInt("GameMode", "RoundLimit", 1);
    m_gameMode.timeLimit = m_config.GetFloat("GameMode", "TimeLimit", 600.0f);
    m_gameMode.respawnDelay = m_config.GetFloat("GameMode", "RespawnDelay", 3.0f);
    m_gameMode.autoRespawn = m_config.GetBool("GameMode", "AutoRespawn", true);
    m_gameMode.maxLives = m_config.GetInt("GameMode", "MaxLives", 0);
    m_gameMode.damageMultiplier = m_config.GetFloat("GameMode", "DamageMultiplier", 1.0f);
    m_gameMode.healthMultiplier = m_config.GetFloat("GameMode", "HealthMultiplier", 1.0f);
    m_gameMode.speedMultiplier = m_config.GetFloat("GameMode", "SpeedMultiplier", 1.0f);
    m_gameMode.friendlyFire = m_config.GetBool("GameMode", "FriendlyFire", false);
    m_gameMode.headshots = m_config.GetBool("GameMode", "Headshots", true);
    m_gameMode.headshotMultiplier = m_config.GetFloat("GameMode", "HeadshotMultiplier", 2.0f);
    m_gameMode.allWeaponsAvailable = m_config.GetBool("GameMode", "AllWeaponsAvailable", true);
    m_gameMode.teamsEnabled = m_config.GetBool("GameMode", "TeamsEnabled", false);
    m_gameMode.maxTeamSize = m_config.GetInt("GameMode", "MaxTeamSize", 8);
    m_gameMode.autoBalance = m_config.GetBool("GameMode", "AutoBalance", true);
    m_gameMode.killPoints = m_config.GetInt("GameMode", "KillPoints", 100);
    m_gameMode.deathPenalty = m_config.GetInt("GameMode", "DeathPenalty", 0);
    m_gameMode.assistPoints = m_config.GetInt("GameMode", "AssistPoints", 25);
    m_gameMode.objectivePoints = m_config.GetInt("GameMode", "ObjectivePoints", 200);
    m_gameMode.headshotBonus = m_config.GetInt("GameMode", "HeadshotBonus", 50);

    // Camera
    m_camera.moveSpeed = m_config.GetFloat("Camera", "MoveSpeed", 10.0f);
    m_camera.rotationSpeed = m_config.GetFloat("Camera", "RotationSpeed", 2.0f);
    m_camera.defaultFov = m_config.GetFloat("Camera", "DefaultFov", 90.0f);
    m_camera.zoomedFov = m_config.GetFloat("Camera", "ZoomedFov", 45.0f);
    m_camera.smoothMovement = m_config.GetBool("Camera", "SmoothMovement", true);
    m_camera.nearPlane = m_config.GetFloat("Camera", "NearPlane", 0.1f);
    m_camera.farPlane = m_config.GetFloat("Camera", "FarPlane", 1000.0f);

    // Editor
    m_editor.gridSize = m_config.GetFloat("Editor", "GridSize", 1.0f);
    m_editor.snapToGrid = m_config.GetBool("Editor", "SnapToGrid", true);
    m_editor.showGrid = m_config.GetBool("Editor", "ShowGrid", true);
    m_editor.gizmoScale = m_config.GetFloat("Editor", "GizmoScale", 1.0f);
    m_editor.autosaveEnabled = m_config.GetBool("Editor", "AutosaveEnabled", true);
    m_editor.autosaveIntervalSeconds = m_config.GetFloat("Editor", "AutosaveIntervalSeconds", 300.0f);
    m_editor.undoHistorySize = m_config.GetInt("Editor", "UndoHistorySize", 100);

    // Network
    m_network.serverPort = m_config.GetInt("Network", "ServerPort", 27015);
    m_network.maxClients = m_config.GetInt("Network", "MaxClients", 32);
    m_network.connectionTimeout = m_config.GetFloat("Network", "ConnectionTimeout", 10.0f);
    m_network.heartbeatInterval = m_config.GetFloat("Network", "HeartbeatInterval", 1.0f);
    m_network.replicationRate = m_config.GetFloat("Network", "ReplicationRate", 20.0f);
    m_network.reliableRetransmitBase = m_config.GetFloat("Network", "ReliableRetransmitBase", 0.5f);
    m_network.maxReliableRetries = m_config.GetInt("Network", "MaxReliableRetries", 5);
    m_network.sendBufferSize = m_config.GetInt("Network", "SendBufferSize", 65536);
    m_network.receiveBufferSize = m_config.GetInt("Network", "ReceiveBufferSize", 65536);
    m_network.enableCompression = m_config.GetBool("Network", "EnableCompression", false);
    m_network.enableEncryption = m_config.GetBool("Network", "EnableEncryption", false);
    m_network.simulatedLatencyMs = m_config.GetFloat("Network", "SimulatedLatencyMs", 0.0f);
    m_network.simulatedPacketLoss = m_config.GetFloat("Network", "SimulatedPacketLoss", 0.0f);
    m_network.simulatedJitterMs = m_config.GetFloat("Network", "SimulatedJitterMs", 0.0f);

    // Scripting
    m_scripting.hotReloadEnabled = m_config.GetBool("Scripting", "HotReloadEnabled", true);
    m_scripting.hotReloadPollInterval = m_config.GetFloat("Scripting", "HotReloadPollInterval", 1.0f);
    m_scripting.contextPoolSize = m_config.GetInt("Scripting", "ContextPoolSize", 8);
    m_scripting.executionTimeoutMs = m_config.GetFloat("Scripting", "ExecutionTimeoutMs", 100.0f);
    m_scripting.generateDebugInfo = m_config.GetBool("Scripting", "GenerateDebugInfo", true);
    m_scripting.maxCallStackDepth = m_config.GetInt("Scripting", "MaxCallStackDepth", 64);
    m_scripting.maxScriptMemoryMB = m_config.GetInt("Scripting", "MaxScriptMemoryMB", 64);
    m_scripting.enableProfiler = m_config.GetBool("Scripting", "EnableProfiler", false);

    // Animation
    m_animation.defaultBlendTime = m_config.GetFloat("Animation", "DefaultBlendTime", 0.2f);
    m_animation.ikSolverIterations = m_config.GetInt("Animation", "IKSolverIterations", 10);
    m_animation.ikTolerance = m_config.GetFloat("Animation", "IKTolerance", 0.001f);
    m_animation.maxActiveMontages = m_config.GetInt("Animation", "MaxActiveMontages", 4);
    m_animation.enableRootMotion = m_config.GetBool("Animation", "EnableRootMotion", true);
    m_animation.lodDistanceMultiplier = m_config.GetFloat("Animation", "LODDistanceMultiplier", 1.0f);
    m_animation.compressionQuality = m_config.GetInt("Animation", "CompressionQuality", 2);
    m_animation.enableAnimationEvents = m_config.GetBool("Animation", "EnableAnimationEvents", true);

    // CrashReporting
    m_crashReporting.enabled = m_config.GetBool("CrashReporting", "Enabled", true);
    m_crashReporting.requireConsent = m_config.GetBool("CrashReporting", "RequireConsent", true);
    m_crashReporting.uploadURL = m_config.GetString("CrashReporting", "UploadURL", "");
    m_crashReporting.proxyURL = m_config.GetString("CrashReporting", "ProxyURL", "");
    m_crashReporting.githubRepo = m_config.GetString("CrashReporting", "GitHubRepo", "");
    m_crashReporting.githubToken = m_config.GetString("CrashReporting", "GitHubToken", "");
    m_crashReporting.githubLabels = m_config.GetString("CrashReporting", "GitHubLabels", "crash-report");
    m_crashReporting.attachDump = m_config.GetBool("CrashReporting", "AttachDump", true);
    m_crashReporting.captureScreenshot = m_config.GetBool("CrashReporting", "CaptureScreenshot", true);
    m_crashReporting.captureSystemInfo = m_config.GetBool("CrashReporting", "CaptureSystemInfo", true);
    m_crashReporting.captureAllThreads = m_config.GetBool("CrashReporting", "CaptureAllThreads", true);
    m_crashReporting.timeoutSeconds = m_config.GetInt("CrashReporting", "TimeoutSeconds", 5);
    m_crashReporting.headlessMode = m_config.GetBool("CrashReporting", "HeadlessMode", false);
    m_crashReporting.promptUserDescription = m_config.GetBool("CrashReporting", "PromptUserDescription", true);
    m_crashReporting.allowScreenshotRefusal = m_config.GetBool("CrashReporting", "AllowScreenshotRefusal", true);
    m_crashReporting.smtpUser = m_config.GetString("CrashReporting", "SmtpUser", "");
    m_crashReporting.smtpPass = m_config.GetString("CrashReporting", "SmtpPass", "");
    m_crashReporting.emailTo = m_config.GetString("CrashReporting", "EmailTo", "");
    m_crashReporting.emailFrom = m_config.GetString("CrashReporting", "EmailFrom", "crashreporter@sparkengine.dev");

    // Debug
    m_debug.suppressFatalAsserts = m_config.GetBool("Debug", "SuppressFatalAsserts", false);
    m_debug.breakOnSuppressedAsserts = m_config.GetBool("Debug", "BreakOnSuppressedAsserts", true);

    // Weather
    m_weather.weatherType = m_config.GetInt("Weather", "WeatherType", 0);
    m_weather.intensity = m_config.GetFloat("Weather", "Intensity", 0.0f);
    m_weather.transitionTime = m_config.GetFloat("Weather", "TransitionTime", 5.0f);
    m_weather.windSpeed = m_config.GetFloat("Weather", "WindSpeed", 0.0f);
    m_weather.windDirectionX = m_config.GetFloat("Weather", "WindDirectionX", 1.0f);
    m_weather.windDirectionY = m_config.GetFloat("Weather", "WindDirectionY", 0.0f);
    m_weather.windDirectionZ = m_config.GetFloat("Weather", "WindDirectionZ", 0.0f);
    m_weather.windGustiness = m_config.GetFloat("Weather", "WindGustiness", 0.0f);
    m_weather.fogDensity = m_config.GetFloat("Weather", "FogDensity", 0.0f);
    m_weather.fogStartDistance = m_config.GetFloat("Weather", "FogStartDistance", 10.0f);
    m_weather.fogEndDistance = m_config.GetFloat("Weather", "FogEndDistance", 500.0f);
    m_weather.precipitationRate = m_config.GetFloat("Weather", "PrecipitationRate", 100.0f);
    m_weather.precipitationSize = m_config.GetFloat("Weather", "PrecipitationSize", 1.0f);
    m_weather.lightningFrequency = m_config.GetFloat("Weather", "LightningFrequency", 0.0f);
    m_weather.thunderDelay = m_config.GetFloat("Weather", "ThunderDelay", 2.0f);
    m_weather.ambientMultiplier = m_config.GetFloat("Weather", "AmbientMultiplier", 1.0f);
    m_weather.directionalMultiplier = m_config.GetFloat("Weather", "DirectionalMultiplier", 1.0f);

    // TimeOfDay
    m_timeOfDay.startHour = m_config.GetFloat("TimeOfDay", "StartHour", 12.0f);
    m_timeOfDay.timeScale = m_config.GetFloat("TimeOfDay", "TimeScale", 60.0f);
    m_timeOfDay.paused = m_config.GetBool("TimeOfDay", "Paused", false);
    m_timeOfDay.sunriseHour = m_config.GetFloat("TimeOfDay", "SunriseHour", 6.0f);
    m_timeOfDay.sunsetHour = m_config.GetFloat("TimeOfDay", "SunsetHour", 20.0f);
    m_timeOfDay.moonIntensity = m_config.GetFloat("TimeOfDay", "MoonIntensity", 0.1f);
    m_timeOfDay.ambientNightMultiplier = m_config.GetFloat("TimeOfDay", "AmbientNightMultiplier", 0.2f);

    // Streaming
    m_streaming.drawDistance = m_config.GetFloat("Streaming", "DrawDistance", 1000.0f);
    m_streaming.lodDistanceMultiplier = m_config.GetFloat("Streaming", "LODDistanceMultiplier", 1.0f);
    m_streaming.streamingDistance = m_config.GetFloat("Streaming", "StreamingDistance", 2000.0f);
    m_streaming.maxLoadedAreas = m_config.GetInt("Streaming", "MaxLoadedAreas", 4);
    m_streaming.lodBias = m_config.GetFloat("Streaming", "LODBias", 0.0f);
    m_streaming.maxConcurrentLoads = m_config.GetInt("Streaming", "MaxConcurrentLoads", 2);
    m_streaming.enableStreaming = m_config.GetBool("Streaming", "EnableStreaming", true);
    m_streaming.shadowDrawDistance = m_config.GetFloat("Streaming", "ShadowDrawDistance", 200.0f);

    // Performance
    m_performance.targetFPS = m_config.GetInt("Performance", "TargetFPS", 0);
    m_performance.workerThreadCount = m_config.GetInt("Performance", "WorkerThreadCount", 0);
    m_performance.enableJobSystem = m_config.GetBool("Performance", "EnableJobSystem", true);
    m_performance.maxParticles = m_config.GetInt("Performance", "MaxParticles", 10000);
    m_performance.maxDecals = m_config.GetInt("Performance", "MaxDecals", 256);
    m_performance.enableAsyncCompute = m_config.GetBool("Performance", "EnableAsyncCompute", false);
    m_performance.enableAsyncLoading = m_config.GetBool("Performance", "EnableAsyncLoading", true);
    m_performance.objectPoolGrowthFactor = m_config.GetFloat("Performance", "ObjectPoolGrowthFactor", 1.5f);

    // World
    m_world.originRebaseThreshold = m_config.GetFloat("World", "OriginRebaseThreshold", 5000.0f);
    m_world.defaultGravityScale = m_config.GetFloat("World", "DefaultGravityScale", 1.0f);
    m_world.enableOriginRebasing = m_config.GetBool("World", "EnableOriginRebasing", true);
    m_world.worldBoundsMin = m_config.GetFloat("World", "WorldBoundsMin", -100000.0f);
    m_world.worldBoundsMax = m_config.GetFloat("World", "WorldBoundsMax", 100000.0f);

    // UI
    m_ui.uiScale = m_config.GetFloat("UI", "UIScale", 1.0f);
    m_ui.fontSize = m_config.GetFloat("UI", "FontSize", 14.0f);
    m_ui.showCrosshair = m_config.GetBool("UI", "ShowCrosshair", true);
    m_ui.showHUD = m_config.GetBool("UI", "ShowHUD", true);
    m_ui.showMinimap = m_config.GetBool("UI", "ShowMinimap", true);
    m_ui.showDamageNumbers = m_config.GetBool("UI", "ShowDamageNumbers", true);
    m_ui.showSubtitles = m_config.GetBool("UI", "ShowSubtitles", true);
    m_ui.hudOpacity = m_config.GetFloat("UI", "HUDOpacity", 1.0f);
    m_ui.subtitleSize = m_config.GetFloat("UI", "SubtitleSize", 1.0f);
    m_ui.showInteractionPrompts = m_config.GetBool("UI", "ShowInteractionPrompts", true);

    // Logging
    m_logging.globalLevel = m_config.GetString("Logging", "GlobalLevel", "Info");
    m_logging.stackTraceLevel = m_config.GetString("Logging", "StackTraceLevel", "Error");

    // CategoryMask: supports hex (0xFFFF) or decimal
    std::string maskStr = m_config.GetString("Logging", "CategoryMask", "0xFFFFFFFF");
    try
    {
        m_logging.categoryMask = static_cast<uint32_t>(std::stoul(maskStr, nullptr, 0));
    }
    catch (...)
    {
        m_logging.categoryMask = 0xFFFFFFFF;
    }

    // Per-category level overrides (empty = use global)
    m_logging.coreLevel = m_config.GetString("Logging", "CoreLevel", "");
    m_logging.graphicsLevel = m_config.GetString("Logging", "GraphicsLevel", "");
    m_logging.physicsLevel = m_config.GetString("Logging", "PhysicsLevel", "");
    m_logging.audioLevel = m_config.GetString("Logging", "AudioLevel", "");
    m_logging.aiLevel = m_config.GetString("Logging", "AILevel", "");
    m_logging.animationLevel = m_config.GetString("Logging", "AnimationLevel", "");
    m_logging.ecsLevel = m_config.GetString("Logging", "ECSLevel", "");
    m_logging.networkLevel = m_config.GetString("Logging", "NetworkLevel", "");
    m_logging.inputLevel = m_config.GetString("Logging", "InputLevel", "");
    m_logging.scriptingLevel = m_config.GetString("Logging", "ScriptingLevel", "");
    m_logging.sceneLevel = m_config.GetString("Logging", "SceneLevel", "");
    m_logging.saveLevel = m_config.GetString("Logging", "SaveLevel", "");
    m_logging.cinematicLevel = m_config.GetString("Logging", "CinematicLevel", "");
    m_logging.proceduralLevel = m_config.GetString("Logging", "ProceduralLevel", "");
    m_logging.editorLevel = m_config.GetString("Logging", "EditorLevel", "");
    m_logging.gameLevel = m_config.GetString("Logging", "GameLevel", "");
}

// =============================================================================
// Write struct fields to ConfigParser
// =============================================================================
void EngineSettings::WriteToConfig() const
{
    // Graphics
    m_config.SetInt("Graphics", "WindowWidth", m_graphics.windowWidth);
    m_config.SetInt("Graphics", "WindowHeight", m_graphics.windowHeight);
    m_config.SetBool("Graphics", "Fullscreen", m_graphics.fullscreen);
    m_config.SetBool("Graphics", "VSync", m_graphics.vsync);
    m_config.SetInt("Graphics", "AntiAliasing", m_graphics.antiAliasing);
    m_config.SetInt("Graphics", "ShadowQuality", m_graphics.shadowQuality);
    m_config.SetFloat("Graphics", "RenderScale", m_graphics.renderScale);
    m_config.SetBool("Graphics", "HDR", m_graphics.hdr);

    // Audio
    m_config.SetFloat("Audio", "MasterVolume", m_audio.masterVolume);
    m_config.SetFloat("Audio", "SFXVolume", m_audio.sfxVolume);
    m_config.SetFloat("Audio", "MusicVolume", m_audio.musicVolume);
    m_config.SetFloat("Audio", "VoiceVolume", m_audio.voiceVolume);
    m_config.SetBool("Audio", "MuteOnFocusLoss", m_audio.muteOnFocusLoss);

    // Controls
    m_config.SetFloat("Controls", "MouseSensitivity", m_controls.mouseSensitivity);
    m_config.SetBool("Controls", "InvertMouse", m_controls.invertMouseY);
    m_config.SetFloat("Controls", "MouseDeadZone", m_controls.mouseDeadZone);
    m_config.SetBool("Controls", "RawMouseInput", m_controls.rawMouseInput);
    m_config.SetBool("Controls", "MouseAcceleration", m_controls.mouseAcceleration);

    // Game
    m_config.SetString("Game", "Difficulty", m_game.difficulty);
    m_config.SetBool("Game", "ShowFPS", m_game.showFPS);
    m_config.SetBool("Game", "ShowDebugInfo", m_game.showDebugInfo);
    m_config.SetFloat("Game", "FieldOfView", m_game.fieldOfView);

    // Rendering
    m_config.SetInt("Rendering", "RenderPath", m_rendering.renderPath);
    m_config.SetInt("Rendering", "QualityPreset", m_rendering.qualityPreset);
    m_config.SetInt("Rendering", "MaxTextureSize", m_rendering.maxTextureSize);
    m_config.SetBool("Rendering", "AnisotropicFiltering", m_rendering.anisotropicFiltering);
    m_config.SetInt("Rendering", "AnisotropyLevel", m_rendering.anisotropyLevel);
    m_config.SetBool("Rendering", "Shadows", m_rendering.shadows);
    m_config.SetInt("Rendering", "ShadowMapSize", m_rendering.shadowMapSize);
    m_config.SetInt("Rendering", "CascadeCount", m_rendering.cascadeCount);
    m_config.SetBool("Rendering", "Bloom", m_rendering.bloom);
    m_config.SetBool("Rendering", "SSAO", m_rendering.ssao);
    m_config.SetBool("Rendering", "TAA", m_rendering.taa);
    m_config.SetBool("Rendering", "MotionBlur", m_rendering.motionBlur);
    m_config.SetBool("Rendering", "FrustumCulling", m_rendering.frustumCulling);
    m_config.SetBool("Rendering", "OcclusionCulling", m_rendering.occlusionCulling);
    m_config.SetBool("Rendering", "PortalCulling", m_rendering.portalCulling);
    m_config.SetBool("Rendering", "LevelOfDetail", m_rendering.levelOfDetail);
    m_config.SetInt("Rendering", "MaxDrawCalls", m_rendering.maxDrawCalls);
    m_config.SetBool("Rendering", "WireframeMode", m_rendering.wireframeMode);
    m_config.SetBool("Rendering", "DebugMode", m_rendering.debugMode);
    m_config.SetBool("Rendering", "EnableGPUTiming", m_rendering.enableGPUTiming);

    // PostProcess
    m_config.SetBool("PostProcess", "BloomEnabled", m_postProcess.bloomEnabled);
    m_config.SetFloat("PostProcess", "BloomThreshold", m_postProcess.bloomThreshold);
    m_config.SetFloat("PostProcess", "BloomIntensity", m_postProcess.bloomIntensity);
    m_config.SetFloat("PostProcess", "BloomRadius", m_postProcess.bloomRadius);
    m_config.SetFloat("PostProcess", "BloomSoftKnee", m_postProcess.bloomSoftKnee);
    m_config.SetInt("PostProcess", "BloomIterations", m_postProcess.bloomIterations);
    m_config.SetInt("PostProcess", "ToneMappingOperator", m_postProcess.toneMappingOperator);
    m_config.SetFloat("PostProcess", "Exposure", m_postProcess.exposure);
    m_config.SetFloat("PostProcess", "Gamma", m_postProcess.gamma);
    m_config.SetFloat("PostProcess", "WhitePoint", m_postProcess.whitePoint);
    m_config.SetBool("PostProcess", "ColorGradingEnabled", m_postProcess.colorGradingEnabled);
    m_config.SetFloat("PostProcess", "Temperature", m_postProcess.temperature);
    m_config.SetFloat("PostProcess", "Tint", m_postProcess.tint);
    m_config.SetFloat("PostProcess", "Contrast", m_postProcess.contrast);
    m_config.SetFloat("PostProcess", "Brightness", m_postProcess.brightness);
    m_config.SetFloat("PostProcess", "Saturation", m_postProcess.saturation);

    // SSAO
    m_config.SetBool("SSAO", "Enabled", m_ssao.enabled);
    m_config.SetFloat("SSAO", "Radius", m_ssao.radius);
    m_config.SetFloat("SSAO", "Intensity", m_ssao.intensity);
    m_config.SetInt("SSAO", "SampleCount", m_ssao.sampleCount);
    m_config.SetFloat("SSAO", "Bias", m_ssao.bias);
    m_config.SetBool("SSAO", "Blur", m_ssao.blur);

    // SSR
    m_config.SetBool("SSR", "Enabled", m_ssr.enabled);
    m_config.SetFloat("SSR", "MaxDistance", m_ssr.maxDistance);
    m_config.SetInt("SSR", "MaxSteps", m_ssr.maxSteps);
    m_config.SetFloat("SSR", "Thickness", m_ssr.thickness);
    m_config.SetFloat("SSR", "FadeStart", m_ssr.fadeStart);
    m_config.SetFloat("SSR", "FadeEnd", m_ssr.fadeEnd);

    // Volumetric
    m_config.SetBool("Volumetric", "Enabled", m_volumetric.enabled);
    m_config.SetInt("Volumetric", "SampleCount", m_volumetric.sampleCount);
    m_config.SetFloat("Volumetric", "Scattering", m_volumetric.scattering);
    m_config.SetFloat("Volumetric", "Extinction", m_volumetric.extinction);
    m_config.SetFloat("Volumetric", "Anisotropy", m_volumetric.anisotropy);

    // TAA
    m_config.SetBool("TAA", "Enabled", m_taa.enabled);
    m_config.SetInt("TAA", "Quality", m_taa.quality);
    m_config.SetInt("TAA", "JitterPattern", m_taa.jitterPattern);
    m_config.SetInt("TAA", "JitterSequenceLength", m_taa.jitterSequenceLength);
    m_config.SetFloat("TAA", "HistoryBlendFactor", m_taa.historyBlendFactor);
    m_config.SetFloat("TAA", "VarianceClipGamma", m_taa.varianceClipGamma);
    m_config.SetBool("TAA", "UseMotionVectors", m_taa.useMotionVectors);
    m_config.SetBool("TAA", "UseYCoCg", m_taa.useYCoCg);
    m_config.SetFloat("TAA", "Sharpness", m_taa.sharpness);
    m_config.SetFloat("TAA", "GhostingRejectionStrength", m_taa.ghostingRejectionStrength);
    m_config.SetFloat("TAA", "FlickerReduction", m_taa.flickerReduction);

    // MotionBlur
    m_config.SetBool("MotionBlur", "Enabled", m_motionBlur.enabled);
    m_config.SetInt("MotionBlur", "Type", m_motionBlur.type);
    m_config.SetFloat("MotionBlur", "Intensity", m_motionBlur.intensity);
    m_config.SetInt("MotionBlur", "SampleCount", m_motionBlur.sampleCount);
    m_config.SetFloat("MotionBlur", "MaxBlurRadius", m_motionBlur.maxBlurRadius);
    m_config.SetFloat("MotionBlur", "VelocityScale", m_motionBlur.velocityScale);
    m_config.SetFloat("MotionBlur", "MinVelocityThreshold", m_motionBlur.minVelocityThreshold);
    m_config.SetFloat("MotionBlur", "CameraRotationScale", m_motionBlur.cameraRotationScale);
    m_config.SetFloat("MotionBlur", "CameraTranslationScale", m_motionBlur.cameraTranslationScale);
    m_config.SetInt("MotionBlur", "TileSize", m_motionBlur.tileSize);

    // DynamicQuality
    m_config.SetBool("DynamicQuality", "Enabled", m_dynamicQuality.enabled);
    m_config.SetFloat("DynamicQuality", "TargetFrameTimeMs", m_dynamicQuality.targetFrameTimeMs);
    m_config.SetFloat("DynamicQuality", "MinRenderScale", m_dynamicQuality.minRenderScale);
    m_config.SetFloat("DynamicQuality", "MaxRenderScale", m_dynamicQuality.maxRenderScale);
    m_config.SetFloat("DynamicQuality", "RenderScaleStep", m_dynamicQuality.renderScaleStep);
    m_config.SetFloat("DynamicQuality", "MinShadowScale", m_dynamicQuality.minShadowScale);
    m_config.SetFloat("DynamicQuality", "MaxShadowScale", m_dynamicQuality.maxShadowScale);
    m_config.SetFloat("DynamicQuality", "ShadowScaleStep", m_dynamicQuality.shadowScaleStep);
    m_config.SetFloat("DynamicQuality", "MinLodBias", m_dynamicQuality.minLodBias);
    m_config.SetFloat("DynamicQuality", "MaxLodBias", m_dynamicQuality.maxLodBias);
    m_config.SetFloat("DynamicQuality", "LodBiasStep", m_dynamicQuality.lodBiasStep);
    m_config.SetFloat("DynamicQuality", "MinTextureMipBias", m_dynamicQuality.minTextureMipBias);
    m_config.SetFloat("DynamicQuality", "MaxTextureMipBias", m_dynamicQuality.maxTextureMipBias);
    m_config.SetFloat("DynamicQuality", "TextureMipBiasStep", m_dynamicQuality.textureMipBiasStep);
    m_config.SetInt("DynamicQuality", "FrameTimeWindowSize", m_dynamicQuality.frameTimeWindowSize);
    m_config.SetInt("DynamicQuality", "MinChangeIntervalFrames", m_dynamicQuality.minChangeIntervalFrames);
    m_config.SetFloat("DynamicQuality", "IncreaseThresholdMs", m_dynamicQuality.increaseThresholdMs);
    m_config.SetFloat("DynamicQuality", "PID_KP", m_dynamicQuality.pidKP);
    m_config.SetFloat("DynamicQuality", "PID_KI", m_dynamicQuality.pidKI);
    m_config.SetFloat("DynamicQuality", "PID_KD", m_dynamicQuality.pidKD);

    // AudioExtended
    m_config.SetFloat("AudioExtended", "DopplerScale", m_audioExtended.dopplerScale);
    m_config.SetFloat("AudioExtended", "DistanceScale", m_audioExtended.distanceScale);
    m_config.SetBool("AudioExtended", "Enable3D", m_audioExtended.enable3D);
    m_config.SetBool("AudioExtended", "EnableReverb", m_audioExtended.enableReverb);
    m_config.SetBool("AudioExtended", "EnableEAX", m_audioExtended.enableEAX);
    m_config.SetInt("AudioExtended", "MaxSources", m_audioExtended.maxSources);

    // Physics
    m_config.SetFloat("Physics", "GravityX", m_physics.gravityX);
    m_config.SetFloat("Physics", "GravityY", m_physics.gravityY);
    m_config.SetFloat("Physics", "GravityZ", m_physics.gravityZ);
    m_config.SetFloat("Physics", "FixedTimestep", m_physics.fixedTimestep);
    m_config.SetInt("Physics", "MaxSubSteps", m_physics.maxSubSteps);
    m_config.SetFloat("Physics", "DefaultFriction", m_physics.defaultFriction);
    m_config.SetFloat("Physics", "DefaultRestitution", m_physics.defaultRestitution);
    m_config.SetFloat("Physics", "DefaultLinearDamping", m_physics.defaultLinearDamping);
    m_config.SetFloat("Physics", "DefaultAngularDamping", m_physics.defaultAngularDamping);
    m_config.SetBool("Physics", "DebugDraw", m_physics.debugDraw);

    // AI
    m_config.SetFloat("AI", "DetectionRange", m_ai.detectionRange);
    m_config.SetFloat("AI", "AttackRange", m_ai.attackRange);
    m_config.SetFloat("AI", "MeleeRange", m_ai.meleeRange);
    m_config.SetFloat("AI", "MoveSpeed", m_ai.moveSpeed);
    m_config.SetFloat("AI", "TurnSpeed", m_ai.turnSpeed);
    m_config.SetFloat("AI", "Accuracy", m_ai.accuracy);
    m_config.SetFloat("AI", "ReactionTime", m_ai.reactionTime);
    m_config.SetFloat("AI", "CoverSearchRadius", m_ai.coverSearchRadius);
    m_config.SetBool("AI", "CanStrafe", m_ai.canStrafe);
    m_config.SetBool("AI", "CanSprint", m_ai.canSprint);
    m_config.SetBool("AI", "CanUseCover", m_ai.canUseCover);

    // Player
    m_config.SetFloat("Player", "MaxHealth", m_player.maxHealth);
    m_config.SetFloat("Player", "MaxArmor", m_player.maxArmor);
    m_config.SetFloat("Player", "MoveSpeed", m_player.moveSpeed);
    m_config.SetFloat("Player", "JumpHeight", m_player.jumpHeight);
    m_config.SetFloat("Player", "GravityForce", m_player.gravityForce);
    m_config.SetFloat("Player", "Friction", m_player.friction);
    m_config.SetFloat("Player", "SprintMultiplier", m_player.sprintMultiplier);
    m_config.SetFloat("Player", "CrouchMultiplier", m_player.crouchMultiplier);
    m_config.SetFloat("Player", "ADSSpeedMultiplier", m_player.adsSpeedMultiplier);
    m_config.SetFloat("Player", "MaxShield", m_player.maxShield);
    m_config.SetFloat("Player", "ShieldRechargeRate", m_player.shieldRechargeRate);
    m_config.SetFloat("Player", "ShieldRechargeDelay", m_player.shieldRechargeDelay);
    m_config.SetFloat("Player", "MaxEnergy", m_player.maxEnergy);
    m_config.SetFloat("Player", "EnergyRegenRate", m_player.energyRegenRate);

    // GameMode
    m_config.SetInt("GameMode", "ScoreLimit", m_gameMode.scoreLimit);
    m_config.SetInt("GameMode", "RoundLimit", m_gameMode.roundLimit);
    m_config.SetFloat("GameMode", "TimeLimit", m_gameMode.timeLimit);
    m_config.SetFloat("GameMode", "RespawnDelay", m_gameMode.respawnDelay);
    m_config.SetBool("GameMode", "AutoRespawn", m_gameMode.autoRespawn);
    m_config.SetInt("GameMode", "MaxLives", m_gameMode.maxLives);
    m_config.SetFloat("GameMode", "DamageMultiplier", m_gameMode.damageMultiplier);
    m_config.SetFloat("GameMode", "HealthMultiplier", m_gameMode.healthMultiplier);
    m_config.SetFloat("GameMode", "SpeedMultiplier", m_gameMode.speedMultiplier);
    m_config.SetBool("GameMode", "FriendlyFire", m_gameMode.friendlyFire);
    m_config.SetBool("GameMode", "Headshots", m_gameMode.headshots);
    m_config.SetFloat("GameMode", "HeadshotMultiplier", m_gameMode.headshotMultiplier);
    m_config.SetBool("GameMode", "AllWeaponsAvailable", m_gameMode.allWeaponsAvailable);
    m_config.SetBool("GameMode", "TeamsEnabled", m_gameMode.teamsEnabled);
    m_config.SetInt("GameMode", "MaxTeamSize", m_gameMode.maxTeamSize);
    m_config.SetBool("GameMode", "AutoBalance", m_gameMode.autoBalance);
    m_config.SetInt("GameMode", "KillPoints", m_gameMode.killPoints);
    m_config.SetInt("GameMode", "DeathPenalty", m_gameMode.deathPenalty);
    m_config.SetInt("GameMode", "AssistPoints", m_gameMode.assistPoints);
    m_config.SetInt("GameMode", "ObjectivePoints", m_gameMode.objectivePoints);
    m_config.SetInt("GameMode", "HeadshotBonus", m_gameMode.headshotBonus);

    // Camera
    m_config.SetFloat("Camera", "MoveSpeed", m_camera.moveSpeed);
    m_config.SetFloat("Camera", "RotationSpeed", m_camera.rotationSpeed);
    m_config.SetFloat("Camera", "DefaultFov", m_camera.defaultFov);
    m_config.SetFloat("Camera", "ZoomedFov", m_camera.zoomedFov);
    m_config.SetBool("Camera", "SmoothMovement", m_camera.smoothMovement);
    m_config.SetFloat("Camera", "NearPlane", m_camera.nearPlane);
    m_config.SetFloat("Camera", "FarPlane", m_camera.farPlane);

    // Editor
    m_config.SetFloat("Editor", "GridSize", m_editor.gridSize);
    m_config.SetBool("Editor", "SnapToGrid", m_editor.snapToGrid);
    m_config.SetBool("Editor", "ShowGrid", m_editor.showGrid);
    m_config.SetFloat("Editor", "GizmoScale", m_editor.gizmoScale);
    m_config.SetBool("Editor", "AutosaveEnabled", m_editor.autosaveEnabled);
    m_config.SetFloat("Editor", "AutosaveIntervalSeconds", m_editor.autosaveIntervalSeconds);
    m_config.SetInt("Editor", "UndoHistorySize", m_editor.undoHistorySize);

    // Network
    m_config.SetInt("Network", "ServerPort", m_network.serverPort);
    m_config.SetInt("Network", "MaxClients", m_network.maxClients);
    m_config.SetFloat("Network", "ConnectionTimeout", m_network.connectionTimeout);
    m_config.SetFloat("Network", "HeartbeatInterval", m_network.heartbeatInterval);
    m_config.SetFloat("Network", "ReplicationRate", m_network.replicationRate);
    m_config.SetFloat("Network", "ReliableRetransmitBase", m_network.reliableRetransmitBase);
    m_config.SetInt("Network", "MaxReliableRetries", m_network.maxReliableRetries);
    m_config.SetInt("Network", "SendBufferSize", m_network.sendBufferSize);
    m_config.SetInt("Network", "ReceiveBufferSize", m_network.receiveBufferSize);
    m_config.SetBool("Network", "EnableCompression", m_network.enableCompression);
    m_config.SetBool("Network", "EnableEncryption", m_network.enableEncryption);
    m_config.SetFloat("Network", "SimulatedLatencyMs", m_network.simulatedLatencyMs);
    m_config.SetFloat("Network", "SimulatedPacketLoss", m_network.simulatedPacketLoss);
    m_config.SetFloat("Network", "SimulatedJitterMs", m_network.simulatedJitterMs);

    // Scripting
    m_config.SetBool("Scripting", "HotReloadEnabled", m_scripting.hotReloadEnabled);
    m_config.SetFloat("Scripting", "HotReloadPollInterval", m_scripting.hotReloadPollInterval);
    m_config.SetInt("Scripting", "ContextPoolSize", m_scripting.contextPoolSize);
    m_config.SetFloat("Scripting", "ExecutionTimeoutMs", m_scripting.executionTimeoutMs);
    m_config.SetBool("Scripting", "GenerateDebugInfo", m_scripting.generateDebugInfo);
    m_config.SetInt("Scripting", "MaxCallStackDepth", m_scripting.maxCallStackDepth);
    m_config.SetInt("Scripting", "MaxScriptMemoryMB", m_scripting.maxScriptMemoryMB);
    m_config.SetBool("Scripting", "EnableProfiler", m_scripting.enableProfiler);

    // Animation
    m_config.SetFloat("Animation", "DefaultBlendTime", m_animation.defaultBlendTime);
    m_config.SetInt("Animation", "IKSolverIterations", m_animation.ikSolverIterations);
    m_config.SetFloat("Animation", "IKTolerance", m_animation.ikTolerance);
    m_config.SetInt("Animation", "MaxActiveMontages", m_animation.maxActiveMontages);
    m_config.SetBool("Animation", "EnableRootMotion", m_animation.enableRootMotion);
    m_config.SetFloat("Animation", "LODDistanceMultiplier", m_animation.lodDistanceMultiplier);
    m_config.SetInt("Animation", "CompressionQuality", m_animation.compressionQuality);
    m_config.SetBool("Animation", "EnableAnimationEvents", m_animation.enableAnimationEvents);

    // CrashReporting
    m_config.SetBool("CrashReporting", "Enabled", m_crashReporting.enabled);
    m_config.SetBool("CrashReporting", "RequireConsent", m_crashReporting.requireConsent);
    m_config.SetString("CrashReporting", "UploadURL", m_crashReporting.uploadURL);
    m_config.SetString("CrashReporting", "ProxyURL", m_crashReporting.proxyURL);
    m_config.SetString("CrashReporting", "GitHubRepo", m_crashReporting.githubRepo);
    m_config.SetString("CrashReporting", "GitHubToken", m_crashReporting.githubToken);
    m_config.SetString("CrashReporting", "GitHubLabels", m_crashReporting.githubLabels);
    m_config.SetBool("CrashReporting", "AttachDump", m_crashReporting.attachDump);
    m_config.SetBool("CrashReporting", "CaptureScreenshot", m_crashReporting.captureScreenshot);
    m_config.SetBool("CrashReporting", "CaptureSystemInfo", m_crashReporting.captureSystemInfo);
    m_config.SetBool("CrashReporting", "CaptureAllThreads", m_crashReporting.captureAllThreads);
    m_config.SetInt("CrashReporting", "TimeoutSeconds", m_crashReporting.timeoutSeconds);
    m_config.SetBool("CrashReporting", "HeadlessMode", m_crashReporting.headlessMode);
    m_config.SetBool("CrashReporting", "PromptUserDescription", m_crashReporting.promptUserDescription);
    m_config.SetBool("CrashReporting", "AllowScreenshotRefusal", m_crashReporting.allowScreenshotRefusal);
    m_config.SetString("CrashReporting", "SmtpUser", m_crashReporting.smtpUser);
    m_config.SetString("CrashReporting", "SmtpPass", m_crashReporting.smtpPass);
    m_config.SetString("CrashReporting", "EmailTo", m_crashReporting.emailTo);
    m_config.SetString("CrashReporting", "EmailFrom", m_crashReporting.emailFrom);

    // Debug
    m_config.SetBool("Debug", "SuppressFatalAsserts", m_debug.suppressFatalAsserts);
    m_config.SetBool("Debug", "BreakOnSuppressedAsserts", m_debug.breakOnSuppressedAsserts);

    // Weather
    m_config.SetInt("Weather", "WeatherType", m_weather.weatherType);
    m_config.SetFloat("Weather", "Intensity", m_weather.intensity);
    m_config.SetFloat("Weather", "TransitionTime", m_weather.transitionTime);
    m_config.SetFloat("Weather", "WindSpeed", m_weather.windSpeed);
    m_config.SetFloat("Weather", "WindDirectionX", m_weather.windDirectionX);
    m_config.SetFloat("Weather", "WindDirectionY", m_weather.windDirectionY);
    m_config.SetFloat("Weather", "WindDirectionZ", m_weather.windDirectionZ);
    m_config.SetFloat("Weather", "WindGustiness", m_weather.windGustiness);
    m_config.SetFloat("Weather", "FogDensity", m_weather.fogDensity);
    m_config.SetFloat("Weather", "FogStartDistance", m_weather.fogStartDistance);
    m_config.SetFloat("Weather", "FogEndDistance", m_weather.fogEndDistance);
    m_config.SetFloat("Weather", "PrecipitationRate", m_weather.precipitationRate);
    m_config.SetFloat("Weather", "PrecipitationSize", m_weather.precipitationSize);
    m_config.SetFloat("Weather", "LightningFrequency", m_weather.lightningFrequency);
    m_config.SetFloat("Weather", "ThunderDelay", m_weather.thunderDelay);
    m_config.SetFloat("Weather", "AmbientMultiplier", m_weather.ambientMultiplier);
    m_config.SetFloat("Weather", "DirectionalMultiplier", m_weather.directionalMultiplier);

    // TimeOfDay
    m_config.SetFloat("TimeOfDay", "StartHour", m_timeOfDay.startHour);
    m_config.SetFloat("TimeOfDay", "TimeScale", m_timeOfDay.timeScale);
    m_config.SetBool("TimeOfDay", "Paused", m_timeOfDay.paused);
    m_config.SetFloat("TimeOfDay", "SunriseHour", m_timeOfDay.sunriseHour);
    m_config.SetFloat("TimeOfDay", "SunsetHour", m_timeOfDay.sunsetHour);
    m_config.SetFloat("TimeOfDay", "MoonIntensity", m_timeOfDay.moonIntensity);
    m_config.SetFloat("TimeOfDay", "AmbientNightMultiplier", m_timeOfDay.ambientNightMultiplier);

    // Streaming
    m_config.SetFloat("Streaming", "DrawDistance", m_streaming.drawDistance);
    m_config.SetFloat("Streaming", "LODDistanceMultiplier", m_streaming.lodDistanceMultiplier);
    m_config.SetFloat("Streaming", "StreamingDistance", m_streaming.streamingDistance);
    m_config.SetInt("Streaming", "MaxLoadedAreas", m_streaming.maxLoadedAreas);
    m_config.SetFloat("Streaming", "LODBias", m_streaming.lodBias);
    m_config.SetInt("Streaming", "MaxConcurrentLoads", m_streaming.maxConcurrentLoads);
    m_config.SetBool("Streaming", "EnableStreaming", m_streaming.enableStreaming);
    m_config.SetFloat("Streaming", "ShadowDrawDistance", m_streaming.shadowDrawDistance);

    // Performance
    m_config.SetInt("Performance", "TargetFPS", m_performance.targetFPS);
    m_config.SetInt("Performance", "WorkerThreadCount", m_performance.workerThreadCount);
    m_config.SetBool("Performance", "EnableJobSystem", m_performance.enableJobSystem);
    m_config.SetInt("Performance", "MaxParticles", m_performance.maxParticles);
    m_config.SetInt("Performance", "MaxDecals", m_performance.maxDecals);
    m_config.SetBool("Performance", "EnableAsyncCompute", m_performance.enableAsyncCompute);
    m_config.SetBool("Performance", "EnableAsyncLoading", m_performance.enableAsyncLoading);
    m_config.SetFloat("Performance", "ObjectPoolGrowthFactor", m_performance.objectPoolGrowthFactor);

    // World
    m_config.SetFloat("World", "OriginRebaseThreshold", m_world.originRebaseThreshold);
    m_config.SetFloat("World", "DefaultGravityScale", m_world.defaultGravityScale);
    m_config.SetBool("World", "EnableOriginRebasing", m_world.enableOriginRebasing);
    m_config.SetFloat("World", "WorldBoundsMin", m_world.worldBoundsMin);
    m_config.SetFloat("World", "WorldBoundsMax", m_world.worldBoundsMax);

    // UI
    m_config.SetFloat("UI", "UIScale", m_ui.uiScale);
    m_config.SetFloat("UI", "FontSize", m_ui.fontSize);
    m_config.SetBool("UI", "ShowCrosshair", m_ui.showCrosshair);
    m_config.SetBool("UI", "ShowHUD", m_ui.showHUD);
    m_config.SetBool("UI", "ShowMinimap", m_ui.showMinimap);
    m_config.SetBool("UI", "ShowDamageNumbers", m_ui.showDamageNumbers);
    m_config.SetBool("UI", "ShowSubtitles", m_ui.showSubtitles);
    m_config.SetFloat("UI", "HUDOpacity", m_ui.hudOpacity);
    m_config.SetFloat("UI", "SubtitleSize", m_ui.subtitleSize);
    m_config.SetBool("UI", "ShowInteractionPrompts", m_ui.showInteractionPrompts);

    // Logging
    m_config.SetString("Logging", "GlobalLevel", m_logging.globalLevel);
    m_config.SetString("Logging", "StackTraceLevel", m_logging.stackTraceLevel);

    // Write CategoryMask as hex for readability
    {
        char hexBuf[16];
        snprintf(hexBuf, sizeof(hexBuf), "0x%08X", m_logging.categoryMask);
        m_config.SetString("Logging", "CategoryMask", hexBuf);
    }

    m_config.SetString("Logging", "CoreLevel", m_logging.coreLevel);
    m_config.SetString("Logging", "GraphicsLevel", m_logging.graphicsLevel);
    m_config.SetString("Logging", "PhysicsLevel", m_logging.physicsLevel);
    m_config.SetString("Logging", "AudioLevel", m_logging.audioLevel);
    m_config.SetString("Logging", "AILevel", m_logging.aiLevel);
    m_config.SetString("Logging", "AnimationLevel", m_logging.animationLevel);
    m_config.SetString("Logging", "ECSLevel", m_logging.ecsLevel);
    m_config.SetString("Logging", "NetworkLevel", m_logging.networkLevel);
    m_config.SetString("Logging", "InputLevel", m_logging.inputLevel);
    m_config.SetString("Logging", "ScriptingLevel", m_logging.scriptingLevel);
    m_config.SetString("Logging", "SceneLevel", m_logging.sceneLevel);
    m_config.SetString("Logging", "SaveLevel", m_logging.saveLevel);
    m_config.SetString("Logging", "CinematicLevel", m_logging.cinematicLevel);
    m_config.SetString("Logging", "ProceduralLevel", m_logging.proceduralLevel);
    m_config.SetString("Logging", "EditorLevel", m_logging.editorLevel);
    m_config.SetString("Logging", "GameLevel", m_logging.gameLevel);
}

// =============================================================================
// Populate defaults into the ConfigParser
// =============================================================================
void EngineSettings::PopulateDefaults()
{
    m_config.Clear();

    // Write current struct values (which have their defaults) into the parser
    WriteToConfig();
}

// =============================================================================
// Generic key access
// =============================================================================
std::string EngineSettings::GetValue(const std::string& section, const std::string& key) const
{
    // Sync struct -> config first
    WriteToConfig();
    return m_config.GetString(section, key, "");
}

bool EngineSettings::SetValue(const std::string& section, const std::string& key, const std::string& value)
{
    // Allow any known section
    auto sections = GetSections();
    bool validSection = false;
    for (const auto& s : sections)
    {
        if (s == section)
        {
            validSection = true;
            break;
        }
    }
    if (!validSection)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "SetValue: unknown section '%s'", section.c_str());
        return false;
    }

    SPARK_LOG_DEBUG(Spark::LogCategory::Core, "Setting changed: [%s] %s = %s", section.c_str(), key.c_str(),
                    value.c_str());
    m_config.SetString(section, key, value);
    ReadFromConfig(); // Sync back to structs
    NotifyChanged(section, key);
    return true;
}

std::vector<std::string> EngineSettings::GetSections() const
{
    return {"Graphics",  "Audio",          "Controls", "Game",       "Rendering",      "PostProcess",   "SSAO",
            "SSR",       "Volumetric",     "TAA",      "MotionBlur", "DynamicQuality", "AudioExtended", "Physics",
            "AI",        "Player",         "GameMode", "Camera",     "Editor",         "Network",       "Scripting",
            "Animation", "CrashReporting", "Weather",  "TimeOfDay",  "Streaming",      "Performance",   "World",
            "UI",        "Logging"};
}

std::vector<std::string> EngineSettings::GetKeys(const std::string& section) const
{
    WriteToConfig();
    return m_config.GetKeys(section);
}

// =============================================================================
// Change notification
// =============================================================================
void EngineSettings::OnSettingsChanged(SettingsChangedCallback callback)
{
    m_changeCallbacks.push_back(std::move(callback));
}

void EngineSettings::NotifyChanged(const std::string& section, const std::string& key) const
{
    for (const auto& cb : m_changeCallbacks)
    {
        cb(section, key);
    }
}

// =============================================================================
// Console commands
// =============================================================================
void EngineSettings::RegisterConsoleCommands()
{
    SPARK_LOG_DEBUG(Spark::LogCategory::Core, "Registering settings console commands");
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand(
        "settings_get",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 2)
                return "Usage: settings_get <section> <key>";
            auto& settings = EngineSettings::GetInstance();
            std::string val = settings.GetValue(args[0], args[1]);
            if (val.empty())
                return "Key not found: " + args[0] + "." + args[1];
            return args[0] + "." + args[1] + " = " + val;
        },
        "Get a settings value", "Settings");

    console.RegisterCommand(
        "settings_set",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 3)
                return "Usage: settings_set <section> <key> <value>";
            auto& settings = EngineSettings::GetInstance();
            if (settings.SetValue(args[0], args[1], args[2]))
            {
                return "Set " + args[0] + "." + args[1] + " = " + args[2];
            }
            return "Failed to set " + args[0] + "." + args[1];
        },
        "Set a settings value (runtime editable)", "Settings");

    console.RegisterCommand(
        "settings_save",
        [](const std::vector<std::string>&) -> std::string
        {
            auto& settings = EngineSettings::GetInstance();
            if (settings.Save())
            {
                return "Settings saved to " + settings.GetFilePath();
            }
            return "Failed to save settings";
        },
        "Save settings to disk", "Settings");

    console.RegisterCommand(
        "settings_reload",
        [](const std::vector<std::string>&) -> std::string
        {
            auto& settings = EngineSettings::GetInstance();
            if (settings.Load(settings.GetFilePath()))
            {
                return "Settings reloaded from " + settings.GetFilePath();
            }
            return "Failed to reload settings";
        },
        "Reload settings from disk (applies changes at runtime)", "Settings");

    console.RegisterCommand(
        "settings_reset",
        [](const std::vector<std::string>&) -> std::string
        {
            auto& settings = EngineSettings::GetInstance();
            settings.ResetToDefaults();
            return "Settings reset to defaults (use settings_save to persist)";
        },
        "Reset all settings to defaults", "Settings");

    console.RegisterCommand(
        "settings_list",
        [](const std::vector<std::string>& args) -> std::string
        {
            auto& settings = EngineSettings::GetInstance();
            std::stringstream ss;

            std::vector<std::string> sections;
            if (!args.empty())
            {
                sections.push_back(args[0]);
            }
            else
            {
                sections = settings.GetSections();
            }

            for (const auto& section : sections)
            {
                ss << "[" << section << "]\n";
                auto keys = settings.GetKeys(section);
                for (const auto& key : keys)
                {
                    ss << "  " << key << " = " << settings.GetValue(section, key) << "\n";
                }
            }
            return ss.str();
        },
        "List all settings (or settings in a section)", "Settings");

    console.RegisterCommand(
        "settings_sections",
        [](const std::vector<std::string>&) -> std::string
        {
            auto& settings = EngineSettings::GetInstance();
            std::stringstream ss;
            ss << "Available sections:\n";
            for (const auto& section : settings.GetSections())
            {
                auto keys = settings.GetKeys(section);
                ss << "  [" << section << "] (" << keys.size() << " keys)\n";
            }
            return ss.str();
        },
        "List all settings sections", "Settings");

    console.RegisterCommand(
        "settings_search",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: settings_search <pattern>";
            auto& settings = EngineSettings::GetInstance();
            std::string pattern = args[0];
            // Convert to lowercase for case-insensitive search
            std::string lowerPattern = pattern;
            std::transform(lowerPattern.begin(), lowerPattern.end(), lowerPattern.begin(), ::tolower);

            std::stringstream ss;
            int count = 0;
            for (const auto& section : settings.GetSections())
            {
                auto keys = settings.GetKeys(section);
                for (const auto& key : keys)
                {
                    std::string lowerSection = section;
                    std::transform(lowerSection.begin(), lowerSection.end(), lowerSection.begin(), ::tolower);
                    std::string lowerKey = key;
                    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);

                    if (lowerSection.contains(lowerPattern) || lowerKey.contains(lowerPattern))
                    {
                        ss << "  " << section << "." << key << " = " << settings.GetValue(section, key) << "\n";
                        count++;
                    }
                }
            }
            if (count == 0)
                return "No settings matching '" + pattern + "'";
            return "Found " + std::to_string(count) + " matching settings:\n" + ss.str();
        },
        "Search settings by name pattern", "Settings");
}
