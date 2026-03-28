/**
 * @file EngineSettings.h
 * @brief Centralized engine settings loaded from INI configuration
 * @author Spark Engine Team
 * @date 2025
 *
 * Wraps ConfigParser to provide typed accessors for all engine settings,
 * default value population, and console integration for live tweaking.
 * All configurable variables across every subsystem are surfaced here
 * so they can be read from / written to settings.ini and modified at
 * runtime via the console or editor.
 */

#pragma once

#include "Utils/ConfigParser.h"
#include <string>
#include <vector>
#include <functional>

class EngineSettings
{
  public:
    // =========================================================================
    // Settings structures
    // =========================================================================

    struct GraphicsSettings
    {
        int windowWidth = 1280;
        int windowHeight = 720;
        bool fullscreen = false;
        bool vsync = true;
        int antiAliasing = 4;     // MSAA sample count (1, 2, 4, 8)
        int shadowQuality = 2;    // 0=Off, 1=Low, 2=Medium, 3=High
        float renderScale = 1.0f; // Internal resolution scale
        bool hdr = false;
    };

    struct AudioSettings
    {
        float masterVolume = 1.0f;
        float sfxVolume = 0.8f;
        float musicVolume = 0.6f;
        float voiceVolume = 1.0f;
        bool muteOnFocusLoss = true;
    };

    struct ControlsSettings
    {
        float mouseSensitivity = 1.0f;
        bool invertMouseY = false;
        float mouseDeadZone = 0.0f;
        bool rawMouseInput = false;
        bool mouseAcceleration = false;
    };

    struct GameSettings
    {
        std::string difficulty = "Normal";
        bool showFPS = true;
        bool showDebugInfo = false;
        float fieldOfView = 90.0f;
    };

    // ---- New: Rendering Advanced ----

    struct RenderingSettings
    {
        // Render path: 0=Forward, 1=Deferred, 2=ForwardPlus, 3=Clustered
        int renderPath = 1;
        // Quality preset: 0=Low, 1=Medium, 2=High, 3=Ultra, 4=Custom
        int qualityPreset = 2;
        int maxTextureSize = 2048;
        bool anisotropicFiltering = true;
        int anisotropyLevel = 16;
        bool shadows = true;
        int shadowMapSize = 2048;
        int cascadeCount = 3;
        bool bloom = true;
        bool ssao = false;
        bool taa = false;
        bool motionBlur = false;
        bool frustumCulling = true;
        bool occlusionCulling = false;
        bool levelOfDetail = true;
        int maxDrawCalls = 1000;
        bool wireframeMode = false;
        bool debugMode = false;
        bool enableGPUTiming = false;
    };

    // ---- New: Post-Processing ----

    struct PostProcessSettings
    {
        // Bloom
        bool bloomEnabled = true;
        float bloomThreshold = 1.0f;
        float bloomIntensity = 1.0f;
        float bloomRadius = 1.0f;
        float bloomSoftKnee = 0.5f;
        int bloomIterations = 6;

        // Tone Mapping: 0=None, 1=Reinhard, 2=ReinhardJodie, 3=Uncharted2, 4=ACES, 5=AgX, 6=FilmicALU
        int toneMappingOperator = 4;
        float exposure = 1.0f;
        float gamma = 2.2f;
        float whitePoint = 11.2f;

        // Color Grading
        bool colorGradingEnabled = false;
        float temperature = 0.0f;
        float tint = 0.0f;
        float contrast = 1.0f;
        float brightness = 0.0f;
        float saturation = 1.0f;
    };

    // ---- New: SSAO ----

    struct SSAOSettings
    {
        bool enabled = false;
        float radius = 0.5f;
        float intensity = 1.0f;
        int sampleCount = 16;
        float bias = 0.025f;
        bool blur = true;
    };

    // ---- New: SSR ----

    struct SSRSettings
    {
        bool enabled = false;
        float maxDistance = 100.0f;
        int maxSteps = 32;
        float thickness = 0.5f;
        float fadeStart = 80.0f;
        float fadeEnd = 100.0f;
    };

    // ---- New: Volumetric Lighting ----

    struct VolumetricSettings
    {
        bool enabled = false;
        int sampleCount = 32;
        float scattering = 0.1f;
        float extinction = 0.01f;
        float anisotropy = 0.3f;
    };

    // ---- New: TAA ----

    struct TAASettings
    {
        bool enabled = true;
        // Quality: 0=Low, 1=Medium, 2=High, 3=Ultra
        int quality = 2;
        // JitterPattern: 0=Halton23, 1=BlueNoise, 2=Uniform8x, 3=InterleavedGradient
        int jitterPattern = 0;
        int jitterSequenceLength = 16;
        float historyBlendFactor = 0.9f;
        float varianceClipGamma = 1.0f;
        bool useMotionVectors = true;
        bool useYCoCg = true;
        float sharpness = 0.0f;
        float ghostingRejectionStrength = 0.8f;
        float flickerReduction = 0.5f;
    };

    // ---- New: Motion Blur ----

    struct MotionBlurSettings
    {
        bool enabled = false;
        // Type: 0=CameraOnly, 1=PerObject, 2=Combined
        int type = 2;
        float intensity = 0.5f;
        int sampleCount = 8;
        float maxBlurRadius = 32.0f;
        float velocityScale = 1.0f;
        float minVelocityThreshold = 0.5f;
        float cameraRotationScale = 0.5f;
        float cameraTranslationScale = 1.0f;
        int tileSize = 20;
    };

    // ---- New: Dynamic Quality Scaler ----

    struct DynamicQualitySettings
    {
        bool enabled = true;
        float targetFrameTimeMs = 16.67f;
        float minRenderScale = 0.5f;
        float maxRenderScale = 1.0f;
        float renderScaleStep = 0.05f;
        float minShadowScale = 0.25f;
        float maxShadowScale = 1.0f;
        float shadowScaleStep = 0.25f;
        float minLodBias = 0.5f;
        float maxLodBias = 1.0f;
        float lodBiasStep = 0.1f;
        float minTextureMipBias = 0.0f;
        float maxTextureMipBias = 4.0f;
        float textureMipBiasStep = 1.0f;
        int frameTimeWindowSize = 30;
        int minChangeIntervalFrames = 15;
        float increaseThresholdMs = 2.0f;
        float pidKP = 0.5f;
        float pidKI = 0.05f;
        float pidKD = 0.1f;
    };

    // ---- New: Audio Extended ----

    struct AudioExtendedSettings
    {
        float dopplerScale = 1.0f;
        float distanceScale = 1.0f;
        bool enable3D = true;
        bool enableReverb = false;
        bool enableEAX = false;
        int maxSources = 32;
    };

    // ---- New: Physics ----

    struct PhysicsSettings
    {
        float gravityX = 0.0f;
        float gravityY = -20.0f;
        float gravityZ = 0.0f;
        float fixedTimestep = 0.016667f;
        int maxSubSteps = 4;
        float defaultFriction = 0.5f;
        float defaultRestitution = 0.3f;
        float defaultLinearDamping = 0.0f;
        float defaultAngularDamping = 0.05f;
        bool debugDraw = false;
    };

    // ---- New: AI ----

    struct AISettings
    {
        float detectionRange = 30.0f;
        float attackRange = 15.0f;
        float meleeRange = 2.0f;
        float moveSpeed = 5.0f;
        float turnSpeed = 180.0f;
        float accuracy = 0.7f;
        float reactionTime = 0.3f;
        float coverSearchRadius = 20.0f;
        bool canStrafe = true;
        bool canSprint = true;
        bool canUseCover = true;
    };

    // ---- New: Player Defaults ----

    struct PlayerSettings
    {
        float maxHealth = 100.0f;
        float maxArmor = 100.0f;
        float moveSpeed = 5.0f;
        float jumpHeight = 3.0f;
        float gravityForce = 20.0f;
        float friction = 0.9f;
        float sprintMultiplier = 2.0f;
        float crouchMultiplier = 0.5f;
        float adsSpeedMultiplier = 0.5f;
        float maxShield = 50.0f;
        float shieldRechargeRate = 10.0f;
        float shieldRechargeDelay = 6.0f;
        float maxEnergy = 100.0f;
        float energyRegenRate = 10.0f;
    };

    // ---- New: Game Mode Rules ----

    struct GameModeSettings
    {
        int scoreLimit = 50;
        int roundLimit = 1;
        float timeLimit = 600.0f;
        float respawnDelay = 3.0f;
        bool autoRespawn = true;
        int maxLives = 0;
        float damageMultiplier = 1.0f;
        float healthMultiplier = 1.0f;
        float speedMultiplier = 1.0f;
        bool friendlyFire = false;
        bool headshots = true;
        float headshotMultiplier = 2.0f;
        bool allWeaponsAvailable = true;
        bool teamsEnabled = false;
        int maxTeamSize = 8;
        bool autoBalance = true;
        int killPoints = 100;
        int deathPenalty = 0;
        int assistPoints = 25;
        int objectivePoints = 200;
        int headshotBonus = 50;
    };

    // ---- New: Camera ----

    struct CameraSettings
    {
        float moveSpeed = 10.0f;
        float rotationSpeed = 2.0f;
        float defaultFov = 90.0f;
        float zoomedFov = 45.0f;
        bool smoothMovement = true;
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
    };

    // ---- New: Editor ----

    struct EditorSettings
    {
        float gridSize = 1.0f;
        bool snapToGrid = true;
        bool showGrid = true;
        float gizmoScale = 1.0f;
        bool autosaveEnabled = true;
        float autosaveIntervalSeconds = 300.0f;
        int undoHistorySize = 100;
    };

    // ---- Network ----

    struct NetworkSettings
    {
        int serverPort = 27015;
        int maxClients = 32;
        float connectionTimeout = 10.0f;
        float heartbeatInterval = 1.0f;
        float replicationRate = 20.0f; // Updates per second
        float reliableRetransmitBase = 0.5f;
        int maxReliableRetries = 5;
        int sendBufferSize = 65536;
        int receiveBufferSize = 65536;
        bool enableCompression = false;
        bool enableEncryption = false;
        // Lag simulation (development only)
        float simulatedLatencyMs = 0.0f;
        float simulatedPacketLoss = 0.0f;
        float simulatedJitterMs = 0.0f;
    };

    // ---- Scripting ----

    struct ScriptingSettings
    {
        bool hotReloadEnabled = true;
        float hotReloadPollInterval = 1.0f; // Seconds between file change checks
        int contextPoolSize = 8;
        float executionTimeoutMs = 100.0f;
        bool generateDebugInfo = true;
        int maxCallStackDepth = 64;
        int maxScriptMemoryMB = 64;
        bool enableProfiler = false;
    };

    // ---- Animation ----

    struct AnimationSettings
    {
        float defaultBlendTime = 0.2f;
        int ikSolverIterations = 10;
        float ikTolerance = 0.001f;
        int maxActiveMontages = 4;
        bool enableRootMotion = true;
        float lodDistanceMultiplier = 1.0f; // Skeleton LOD distance scale
        int compressionQuality = 2;         // 0=None, 1=Low, 2=Medium, 3=High
        bool enableAnimationEvents = true;
    };

    // ---- Logging ----

    struct LoggingSettings
    {
        /// Global minimum log level: Trace, Debug, Info, Warn, Error, Fatal, Off
        std::string globalLevel = "Info";

        /// Minimum level at which stack traces are auto-captured (Off to disable)
        std::string stackTraceLevel = "Error";

        /// Category enable bitmask (hex or decimal). Each bit = one LogCategory.
        /// 0xFFFF = all enabled, 0x0000 = all disabled.
        /// Bit 0=Core, 1=Graphics, 2=Physics, 3=Audio, 4=AI, 5=Animation,
        /// 6=ECS, 7=Network, 8=Input, 9=Scripting, 10=Scene, 11=Save,
        /// 12=Cinematic, 13=Procedural, 14=Editor, 15=Game
        uint32_t categoryMask = 0xFFFFFFFF;

        /// Per-category level overrides (empty string = use global level)
        std::string coreLevel;
        std::string graphicsLevel;
        std::string physicsLevel;
        std::string audioLevel;
        std::string aiLevel;
        std::string animationLevel;
        std::string ecsLevel;
        std::string networkLevel;
        std::string inputLevel;
        std::string scriptingLevel;
        std::string sceneLevel;
        std::string saveLevel;
        std::string cinematicLevel;
        std::string proceduralLevel;
        std::string editorLevel;
        std::string gameLevel;
    };

    // =========================================================================
    // Singleton access
    // =========================================================================

    static EngineSettings& GetInstance();

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /// Load settings from file. Creates defaults if file doesn't exist.
    bool Load(const std::string& path = "");

    /// Save current settings to file.
    bool Save() const;

    /// Save to a specific path.
    bool SaveAs(const std::string& path) const;

    /// Reset all settings to defaults.
    void ResetToDefaults();

    /// Get the path settings were loaded from.
    const std::string& GetFilePath() const { return m_filePath; }

    // =========================================================================
    // Typed accessors
    // =========================================================================

    GraphicsSettings& Graphics() { return m_graphics; }
    const GraphicsSettings& Graphics() const { return m_graphics; }

    AudioSettings& Audio() { return m_audio; }
    const AudioSettings& Audio() const { return m_audio; }

    ControlsSettings& Controls() { return m_controls; }
    const ControlsSettings& Controls() const { return m_controls; }

    GameSettings& Game() { return m_game; }
    const GameSettings& Game() const { return m_game; }

    RenderingSettings& Rendering() { return m_rendering; }
    const RenderingSettings& Rendering() const { return m_rendering; }

    PostProcessSettings& PostProcess() { return m_postProcess; }
    const PostProcessSettings& PostProcess() const { return m_postProcess; }

    SSAOSettings& SSAO() { return m_ssao; }
    const SSAOSettings& SSAO() const { return m_ssao; }

    SSRSettings& SSR() { return m_ssr; }
    const SSRSettings& SSR() const { return m_ssr; }

    VolumetricSettings& Volumetric() { return m_volumetric; }
    const VolumetricSettings& Volumetric() const { return m_volumetric; }

    TAASettings& TAA() { return m_taa; }
    const TAASettings& TAA() const { return m_taa; }

    MotionBlurSettings& MotionBlur() { return m_motionBlur; }
    const MotionBlurSettings& MotionBlur() const { return m_motionBlur; }

    DynamicQualitySettings& DynamicQuality() { return m_dynamicQuality; }
    const DynamicQualitySettings& DynamicQuality() const { return m_dynamicQuality; }

    AudioExtendedSettings& AudioExtended() { return m_audioExtended; }
    const AudioExtendedSettings& AudioExtended() const { return m_audioExtended; }

    PhysicsSettings& Physics() { return m_physics; }
    const PhysicsSettings& Physics() const { return m_physics; }

    AISettings& AI() { return m_ai; }
    const AISettings& AI() const { return m_ai; }

    PlayerSettings& Player() { return m_player; }
    const PlayerSettings& Player() const { return m_player; }

    GameModeSettings& GameMode() { return m_gameMode; }
    const GameModeSettings& GameMode() const { return m_gameMode; }

    CameraSettings& Camera() { return m_camera; }
    const CameraSettings& Camera() const { return m_camera; }

    EditorSettings& Editor() { return m_editor; }
    const EditorSettings& Editor() const { return m_editor; }

    NetworkSettings& Network() { return m_network; }
    const NetworkSettings& Network() const { return m_network; }

    ScriptingSettings& Scripting() { return m_scripting; }
    const ScriptingSettings& Scripting() const { return m_scripting; }

    AnimationSettings& Animation() { return m_animation; }
    const AnimationSettings& Animation() const { return m_animation; }

    LoggingSettings& Logging() { return m_logging; }
    const LoggingSettings& Logging() const { return m_logging; }

    // =========================================================================
    // Generic key access (for console commands)
    // =========================================================================

    /// Get a setting value as string: "Graphics.WindowWidth" -> "1280"
    std::string GetValue(const std::string& section, const std::string& key) const;

    /// Set a setting value from string: "Graphics", "WindowWidth", "1920"
    bool SetValue(const std::string& section, const std::string& key, const std::string& value);

    /// Get all section names.
    std::vector<std::string> GetSections() const;

    /// Get all keys in a section.
    std::vector<std::string> GetKeys(const std::string& section) const;

    // =========================================================================
    // Runtime reload / apply
    // =========================================================================

    /// Register a callback invoked after settings are applied via SetValue or Load.
    using SettingsChangedCallback = std::function<void(const std::string& section, const std::string& key)>;
    void OnSettingsChanged(SettingsChangedCallback callback);

    // =========================================================================
    // Console command registration
    // =========================================================================

    /// Register settings-related console commands with SparkConsole.
    void RegisterConsoleCommands();

  private:
    EngineSettings() = default;

    /// Read struct fields from the ConfigParser.
    void ReadFromConfig();

    /// Write struct fields to the ConfigParser.
    void WriteToConfig() const;

    /// Populate the ConfigParser with sensible defaults.
    void PopulateDefaults();

    /// Find settings.ini relative to executable.
    std::string FindSettingsPath() const;

    /// Notify change listeners.
    void NotifyChanged(const std::string& section, const std::string& key) const;

    // Data
    mutable Spark::ConfigParser m_config;
    std::string m_filePath;

    GraphicsSettings m_graphics;
    AudioSettings m_audio;
    ControlsSettings m_controls;
    GameSettings m_game;
    RenderingSettings m_rendering;
    PostProcessSettings m_postProcess;
    SSAOSettings m_ssao;
    SSRSettings m_ssr;
    VolumetricSettings m_volumetric;
    TAASettings m_taa;
    MotionBlurSettings m_motionBlur;
    DynamicQualitySettings m_dynamicQuality;
    AudioExtendedSettings m_audioExtended;
    PhysicsSettings m_physics;
    AISettings m_ai;
    PlayerSettings m_player;
    GameModeSettings m_gameMode;
    CameraSettings m_camera;
    EditorSettings m_editor;
    NetworkSettings m_network;
    ScriptingSettings m_scripting;
    AnimationSettings m_animation;
    LoggingSettings m_logging;

    // Change listeners
    std::vector<SettingsChangedCallback> m_changeCallbacks;
};
