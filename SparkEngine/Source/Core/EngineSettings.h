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
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

/// Flags indicating how a setting behaves at runtime.
/// Used by the console, editor UI, and settings_list to annotate each value.
enum class SettingsFlags : uint8_t
{
    Runtime = 0,         ///< Safe to change on the fly — takes effect immediately
    RequiresRestart = 1, ///< Stored but only applied on next engine/subsystem restart
    ReadOnly = 2,        ///< Informational — cannot be changed via console or editor
    DevOnly = 3          ///< Only modifiable in development builds or with cheats enabled
};

class EngineSettings
{
  public:
    // =========================================================================
    // Settings structures — organised by domain
    //
    // Legend (in comments after each field):
    //   [R]  = Runtime — takes effect immediately
    //   [RS] = Requires Restart — stored, applied on next launch
    //   [RO] = Read-Only — informational, not modifiable
    //   [D]  = Dev-Only — development/cheat builds only
    // =========================================================================

    // =====================================================================
    // 1. DISPLAY & WINDOW
    // =====================================================================

    struct GraphicsSettings
    {
        int windowWidth = 1280;          ///< [RS] Window width in pixels
        int windowHeight = 720;          ///< [RS] Window height in pixels
        bool fullscreen = false;         ///< [RS] Fullscreen mode (requires mode switch)
        bool vsync = true;               ///< [R]  Vertical sync toggle
        int antiAliasing = 4;            ///< [R]  MSAA sample count (1, 2, 4, 8)
        int shadowQuality = 2;           ///< [R]  0=Off, 1=Low, 2=Medium, 3=High
        float renderScale = 1.0f;        ///< [R]  Internal resolution scale
        bool hdr = false;                ///< [RS] HDR output mode
        int refreshRate = 0;             ///< [RS] Monitor refresh rate Hz (0=native)
        bool borderlessWindowed = false; ///< [RS] Borderless windowed mode
        int monitor = 0;                 ///< [RS] Monitor index for multi-monitor setups
        bool tripleBuffering = false;    ///< [RS] Triple buffering (reduces input lag with vsync)
        int maxFrameLatency = 2;         ///< [R]  Max pre-rendered frames (1-4)
    };

    // =====================================================================
    // 4. AUDIO
    // =====================================================================

    struct AudioSettings
    {
        float masterVolume = 1.0f;   ///< [R] Master volume (0-1)
        float sfxVolume = 0.8f;      ///< [R] Sound effects volume (0-1)
        float musicVolume = 0.6f;    ///< [R] Music volume (0-1)
        float voiceVolume = 1.0f;    ///< [R] Voice/dialogue volume (0-1)
        float ambienceVolume = 0.7f; ///< [R] Ambient sound volume (0-1)
        bool muteOnFocusLoss = true; ///< [R] Mute when window loses focus
        bool muteAll = false;        ///< [R] Master mute toggle
    };

    // =====================================================================
    // 5. INPUT & CONTROLS
    // =====================================================================

    struct ControlsSettings
    {
        float mouseSensitivity = 1.0f;        ///< [R] Mouse sensitivity multiplier
        bool invertMouseY = false;            ///< [R] Invert vertical mouse axis
        float mouseDeadZone = 0.0f;           ///< [R] Mouse dead zone threshold
        bool rawMouseInput = false;           ///< [R] Bypass OS mouse acceleration
        bool mouseAcceleration = false;       ///< [R] Enable mouse acceleration curve
        float controllerDeadZoneLeft = 0.15f; ///< [R] Left stick dead zone (0-1)
        float controllerDeadZoneRight = 0.1f; ///< [R] Right stick dead zone (0-1)
        float controllerSensitivity = 1.0f;   ///< [R] Controller aim sensitivity
        bool controllerVibration = true;      ///< [R] Controller haptic feedback
        bool invertControllerY = false;       ///< [R] Invert vertical controller axis
    };

    // =====================================================================
    // 6. GAME
    // =====================================================================

    struct GameSettings
    {
        std::string difficulty = "Normal"; ///< [R] Difficulty preset (Easy/Normal/Hard/Nightmare)
        bool showFPS = true;               ///< [R] Show FPS counter overlay
        bool showDebugInfo = false;        ///< [R] Show debug info overlay
        float fieldOfView = 90.0f;         ///< [R] Horizontal field of view (degrees)
        std::string language = "en";       ///< [RS] Game language code (en/de/fr/es/ja/zh/ko/pt/ru/it)
        bool pauseOnFocusLoss = true;      ///< [R] Pause single-player when window loses focus
    };

    // =====================================================================
    // 2. RENDERING PIPELINE
    // =====================================================================

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
        bool portalCulling = false;
        bool levelOfDetail = true;
        int maxDrawCalls = 1000;
        bool wireframeMode = false;
        bool debugMode = false;
        bool enableGPUTiming = false;
    };

    // =====================================================================
    // 3. POST-PROCESSING
    // =====================================================================

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

    struct SSAOSettings
    {
        bool enabled = false;
        float radius = 0.5f;
        float intensity = 1.0f;
        int sampleCount = 16;
        float bias = 0.025f;
        bool blur = true;
    };

    struct SSRSettings
    {
        bool enabled = false;
        float maxDistance = 100.0f;
        int maxSteps = 32;
        float thickness = 0.5f;
        float fadeStart = 80.0f;
        float fadeEnd = 100.0f;
    };

    struct VolumetricSettings
    {
        bool enabled = false;
        int sampleCount = 32;
        float scattering = 0.1f;
        float extinction = 0.01f;
        float anisotropy = 0.3f;
    };

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

    struct AudioExtendedSettings
    {
        float dopplerScale = 1.0f;
        float distanceScale = 1.0f;
        bool enable3D = true;
        bool enableReverb = false;
        bool enableEAX = false;
        int maxSources = 32;
    };

    // =====================================================================
    // 7. PHYSICS
    // =====================================================================

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

    // =====================================================================
    // 8. AI
    // =====================================================================

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

    // =====================================================================
    // 9. PLAYER
    // =====================================================================

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

    // =====================================================================
    // 10. GAME MODE
    // =====================================================================

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

    // =====================================================================
    // 11. CAMERA
    // =====================================================================

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

    // =====================================================================
    // 12. EDITOR
    // =====================================================================

    struct EditorSettings
    {
        float gridSize = 1.0f;                  ///< [R] Grid cell size (world units)
        bool snapToGrid = true;                 ///< [R] Snap objects to grid
        bool showGrid = true;                   ///< [R] Display grid overlay
        float gizmoScale = 1.0f;                ///< [R] Transform gizmo size multiplier
        bool autosaveEnabled = true;            ///< [R] Periodic autosave
        float autosaveIntervalSeconds = 300.0f; ///< [R] Autosave interval (seconds)
        int undoHistorySize = 100;              ///< [R] Undo stack depth
        float rotationSnap = 15.0f;             ///< [R] Rotation snap increment (degrees)
        float scaleSnap = 0.25f;                ///< [R] Scale snap increment
        bool showGizmoLabels = true;            ///< [R] Show position/rotation labels on gizmo
        bool showWireframeOverlay = false;      ///< [R] Wireframe overlay on selected objects
        bool showBounds = false;                ///< [R] Show bounding boxes
        bool showColliders = false;             ///< [R] Show physics colliders
        bool showNavMesh = false;               ///< [R] Show navigation mesh
        bool showLightRadius = true;            ///< [R] Show light influence radius
        float cameraSpeedMultiplier = 1.0f;     ///< [R] Editor camera fly speed multiplier
        int recentFilesMax = 10;                ///< [R] Max recent files in menu
        bool enableCollaboration = false;       ///< [RS] Multi-user collaborative editing
    };

    // =====================================================================
    // 13. NETWORK
    // =====================================================================

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

    // =====================================================================
    // 14. SCRIPTING
    // =====================================================================

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

    // =====================================================================
    // 15. ANIMATION
    // =====================================================================

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

    // =====================================================================
    // 16. CRASH REPORTING & DEBUG
    // =====================================================================

    struct CrashReportingSettings
    {
        bool enabled = true;                                     ///< Master switch for crash report uploading
        bool requireConsent = true;                              ///< Show consent dialog before uploading
        bool headlessMode = false;                               ///< Skip dialogs (CI/testing/headless — auto-consent)
        bool promptUserDescription = true;                       ///< Show "what were you doing" text input
        bool allowScreenshotRefusal = true;                      ///< Let users refuse screenshot in consent dialog
        std::string uploadURL = "";                              ///< Upload URL (auto-detects backend from prefix)
        std::string proxyURL = "";                               ///< Proxy relay endpoint (release builds)
        std::string githubRepo = "";                             ///< GitHub "owner/repo" for direct Issue creation
        std::string githubToken = "";                            ///< GitHub PAT (dev builds only, not shipped)
        std::string githubLabels = "crash-report";               ///< Comma-separated Issue labels
        bool attachDump = true;                                  ///< Attach zip dump to crash reports
        bool captureScreenshot = true;                           ///< Capture screenshot at crash time
        bool captureSystemInfo = true;                           ///< Collect OS/GPU/memory info
        bool captureAllThreads = true;                           ///< Dump all thread stacks
        int timeoutSeconds = 5;                                  ///< HTTP connection timeout
        std::string smtpUser = "";                               ///< SMTP username (email backend)
        std::string smtpPass = "";                               ///< SMTP password (email backend)
        std::string emailTo = "";                                ///< Recipient email address
        std::string emailFrom = "crashreporter@sparkengine.dev"; ///< Sender address
    };

    struct DebugSettings
    {
        bool suppressFatalAsserts = false;    ///< Suppress fatal assertions (log + continue instead of abort)
        bool breakOnSuppressedAsserts = true; ///< Break into debugger on suppressed assertions
    };

    // =====================================================================
    // 17. ENVIRONMENT (Weather, Time of Day, World)
    // =====================================================================

    struct WeatherSettings
    {
        int weatherType = 0; // 0=Clear, 1=Cloudy, 2=Rain, 3=Snow, 4=Fog, 5=Storm
        float intensity = 0.0f;
        float transitionTime = 5.0f;
        float windSpeed = 0.0f;
        float windDirectionX = 1.0f;
        float windDirectionY = 0.0f;
        float windDirectionZ = 0.0f;
        float windGustiness = 0.0f;
        float fogDensity = 0.0f;
        float fogStartDistance = 10.0f;
        float fogEndDistance = 500.0f;
        float precipitationRate = 100.0f;
        float precipitationSize = 1.0f;
        float lightningFrequency = 0.0f;
        float thunderDelay = 2.0f;
        float ambientMultiplier = 1.0f;
        float directionalMultiplier = 1.0f;
    };

    struct TimeOfDaySettings
    {
        float startHour = 12.0f;
        float timeScale = 60.0f; // Game seconds per real second
        bool paused = false;
        float sunriseHour = 6.0f;
        float sunsetHour = 20.0f;
        float moonIntensity = 0.1f;
        float ambientNightMultiplier = 0.2f;
    };

    // =====================================================================
    // 18. STREAMING & LOD
    // =====================================================================

    struct StreamingSettings
    {
        float drawDistance = 1000.0f;
        float lodDistanceMultiplier = 1.0f;
        float streamingDistance = 2000.0f;
        int maxLoadedAreas = 4;
        float lodBias = 0.0f; // Positive = lower quality at distance
        int maxConcurrentLoads = 2;
        bool enableStreaming = true;
        float shadowDrawDistance = 200.0f;
    };

    // =====================================================================
    // 19. PERFORMANCE & MEMORY
    // =====================================================================

    struct PerformanceSettings
    {
        int targetFPS = 0;         // 0 = uncapped
        int workerThreadCount = 0; // 0 = auto (hardware_concurrency - 1)
        bool enableJobSystem = true;
        int maxParticles = 10000;
        int maxDecals = 256;
        bool enableAsyncCompute = false;
        bool enableAsyncLoading = true;
        float objectPoolGrowthFactor = 1.5f;
    };

    struct WorldSettings
    {
        float originRebaseThreshold = 5000.0f;
        float defaultGravityScale = 1.0f;
        bool enableOriginRebasing = true;
        float worldBoundsMin = -100000.0f;
        float worldBoundsMax = 100000.0f;
    };

    // =====================================================================
    // 20. UI & ACCESSIBILITY
    // =====================================================================

    struct UISettings
    {
        float uiScale = 1.0f;
        float fontSize = 14.0f;
        bool showCrosshair = true;
        bool showHUD = true;
        bool showMinimap = true;
        bool showDamageNumbers = true;
        bool showSubtitles = true;
        float hudOpacity = 1.0f;
        float subtitleSize = 1.0f;
        bool showInteractionPrompts = true;
    };

    // =====================================================================
    // 21. LOGGING
    // =====================================================================

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

    // =====================================================================
    // 22. ACCESSIBILITY
    // =====================================================================

    struct AccessibilitySettings
    {
        int colorblindMode = 0;          ///< [R] 0=Off, 1=Protanopia, 2=Deuteranopia, 3=Tritanopia
        float colorblindStrength = 1.0f; ///< [R] Colorblind filter intensity (0-1)
        bool screenReader = false;       ///< [R] Enable screen reader support
        bool reduceMotion = false;       ///< [R] Reduce screen shake, camera bob, particle effects
        bool highContrast = false;       ///< [R] High-contrast UI and outlines
        float textToSpeechRate = 1.0f;   ///< [R] Text-to-speech speed multiplier
        bool largeText = false;          ///< [R] Force larger minimum text sizes
        bool closedCaptions = false;     ///< [R] Closed captions for non-dialogue sounds
        float holdTimeMultiplier = 1.0f; ///< [R] Multiplier for hold-to-interact durations
        bool toggleAim = false;          ///< [R] Toggle aim instead of hold
        bool toggleSprint = false;       ///< [R] Toggle sprint instead of hold
        bool toggleCrouch = false;       ///< [R] Toggle crouch instead of hold
        bool autoAim = false;            ///< [R] Aim assist for accessibility
        float autoAimStrength = 0.5f;    ///< [R] Aim assist strength (0-1)
    };

    // =====================================================================
    // 23. VR
    // =====================================================================

    struct VRSettings
    {
        bool enabled = false;                  ///< [RS] Enable VR mode (requires restart)
        int renderTargetWidth = 1440;          ///< [RS] Per-eye render target width
        int renderTargetHeight = 1600;         ///< [RS] Per-eye render target height
        float renderScale = 1.0f;              ///< [R]  VR supersampling scale
        int trackingSpace = 1;                 ///< [RS] 0=Seated, 1=RoomScale
        bool headTrackingEnabled = true;       ///< [R]  Head tracking
        bool controllerTrackingEnabled = true; ///< [R]  Controller tracking
        float hapticAmplitude = 1.0f;          ///< [R]  Default haptic feedback amplitude (0-1)
        float hapticDuration = 0.1f;           ///< [R]  Default haptic pulse duration (seconds)
        float ipd = 0.064f;                    ///< [R]  Inter-pupillary distance (meters)
        int comfortMode = 0;                   ///< [R]  0=Off, 1=Vignette, 2=SnapTurn
        float snapTurnAngle = 45.0f;           ///< [R]  Snap turn degrees
        bool reprojection = true;              ///< [R]  Motion reprojection
    };

    // =====================================================================
    // 24. DESTRUCTION
    // =====================================================================

    struct DestructionSettings
    {
        float debrisLifetime = 10.0f;    ///< [R] Default debris lifetime (seconds)
        float damageThreshold = 50.0f;   ///< [R] Min damage to trigger destruction
        float damageMultiplier = 1.0f;   ///< [R] Global destruction damage multiplier
        int maxDamageStages = 3;         ///< [R] Max progressive destruction stages
        float scatterForce = 5.0f;       ///< [R] Force applied to debris chunks
        int maxDebrisPieces = 100;       ///< [R] Max debris pieces per object
        bool enablePhysicsDebris = true; ///< [R] Physics-simulated debris
    };

    // =====================================================================
    // 25. DIALOGUE
    // =====================================================================

    struct DialogueSettings
    {
        float defaultCooldown = 5.0f; ///< [R] DRS rule cooldown (seconds)
        int defaultPriority = 50;     ///< [R] Default response rule priority (0-100)
        int maxRulesPerSignal = 16;   ///< [R] Max rules evaluated per signal
        float typingSpeed = 40.0f;    ///< [R] Characters per second for typewriter effect
        bool enableBarks = true;      ///< [R] NPC bark system enabled
        float barkRange = 15.0f;      ///< [R] Max distance to hear NPC barks
        float barkCooldown = 10.0f;   ///< [R] Min time between barks from same NPC
    };

    // =====================================================================
    // 26. MODDING
    // =====================================================================

    struct ModdingSettings
    {
        bool enableModding = false;         ///< [RS] Master mod loading toggle
        std::string modsDirectory = "Mods"; ///< [RS] Relative path to mods folder
        bool allowScriptMods = false;       ///< [RS] Allow mods to load scripts (security)
        bool allowAssetOverrides = true;    ///< [RS] Allow mods to override base assets
        int maxLoadedMods = 32;             ///< [RS] Maximum simultaneously loaded mods
        bool sandboxMods = true;            ///< [RS] Run mod scripts in sandbox
    };

    // =====================================================================
    // 27. LOCALIZATION
    // =====================================================================

    struct LocalizationSettings
    {
        std::string defaultLanguage = "en";           ///< [RS] Default language code
        std::string fallbackLanguage = "en";          ///< [R]  Fallback when key missing
        bool autoDetectLanguage = true;               ///< [RS] Auto-detect from OS locale
        bool loadAllLanguages = false;                ///< [RS] Preload all language files
        std::string localizationDir = "Localization"; ///< [RS] Localization files directory
    };

    // =====================================================================
    // 28. SAVE SYSTEM
    // =====================================================================

    struct SaveSystemSettings
    {
        std::string savesDirectory = "Saves"; ///< [RS] Save files directory
        int maxAutoSaveSlots = 3;             ///< [R]  Auto-save slot rotation count
        float autoSaveInterval = 300.0f;      ///< [R]  Game auto-save interval (seconds, 0=disabled)
        bool enableCloudSaves = false;        ///< [RS] Sync saves to cloud
        bool compressSaves = true;            ///< [R]  Compress save files
        bool backupOnSave = true;             ///< [R]  Keep backup of previous save
        int maxManualSaves = 100;             ///< [R]  Max manual save slots
    };

    // =====================================================================
    // 29. REPLAY
    // =====================================================================

    struct ReplaySettings
    {
        float recordInterval = 0.05f;            ///< [R]  Seconds between replay snapshots
        int maxFrameCount = 1000000;             ///< [R]  Max recorded frames (safety limit)
        int maxEntityCount = 100000;             ///< [R]  Max tracked entities
        int maxEventCount = 500000;              ///< [R]  Max recorded events
        int maxStringLength = 256;               ///< [R]  Max string length in replay data
        std::string replayDirectory = "Replays"; ///< [R] Replay storage directory
        bool autoRecord = false;                 ///< [R]  Auto-start recording on gameplay
    };

    // =====================================================================
    // 30. PERSISTENCE (Async Database)
    // =====================================================================

    struct PersistenceSettings
    {
        std::string databasePath = "Data/persistence.db"; ///< [RS] SQLite database path
        int connectionPoolSize = 4;                       ///< [RS] Database connection pool size
        int workerThreadCount = 2;                        ///< [RS] Async query worker threads
        float queryTimeoutMs = 5000.0f;                   ///< [R]  Query timeout (milliseconds)
        bool enableWAL = true;                            ///< [RS] SQLite WAL mode
        int maxRetries = 3;                               ///< [R]  Retry count on transient failures
    };

    // =====================================================================
    // 31. PARTICLES & DECALS
    // =====================================================================

    struct ParticleSettings
    {
        int maxParticles = 10000;           ///< [R]  Global particle count limit
        int maxEmitters = 256;              ///< [R]  Max active emitters
        float simulationRate = 60.0f;       ///< [R]  Particle simulation tick rate (Hz)
        float lodDistanceMultiplier = 1.0f; ///< [R]  Particle LOD distance scale
        bool gpuParticles = false;          ///< [RS] GPU-accelerated particles
        float globalScale = 1.0f;           ///< [R]  Global particle size multiplier
        bool softParticles = true;          ///< [R]  Depth-tested soft particles
    };

    struct DecalSettings
    {
        int maxDecals = 256;           ///< [R]  Max simultaneous decals
        float defaultLifetime = 30.0f; ///< [R]  Decal lifetime (seconds, 0=permanent)
        float fadeTime = 2.0f;         ///< [R]  Fade-out duration (seconds)
        int atlasSize = 2048;          ///< [RS] Decal atlas texture size
        bool enableDecals = true;      ///< [R]  Master decal toggle
    };

    // =====================================================================
    // 32. MEMORY MANAGEMENT
    // =====================================================================

    struct MemorySettings
    {
        int textureStreamingBudgetMB = 512; ///< [R]  Texture streaming memory budget
        int meshStreamingBudgetMB = 256;    ///< [R]  Mesh streaming memory budget
        int audioStreamingBudgetMB = 128;   ///< [R]  Audio streaming memory budget
        int shaderCacheSizeMB = 64;         ///< [RS] Shader cache size limit
        bool enableMemoryTracking = false;  ///< [RS] Detailed memory allocation tracking
        float gcInterval = 60.0f;           ///< [R]  Garbage collection interval (seconds)
        float gcAggressiveness = 0.5f;      ///< [R]  GC aggressiveness (0=lazy, 1=aggressive)
    };

    // =====================================================================
    // 33. ONLINE SERVICES
    // =====================================================================

    struct OnlineServicesSettings
    {
        int platformBackend = 0;           ///< [RS] 0=Null, 1=Steam, 2=Epic, 3=Custom
        bool enableOnlineServices = false; ///< [RS] Master online toggle
        float sessionTimeout = 300.0f;     ///< [R]  Session inactivity timeout (seconds)
        int maxSessionSearchResults = 50;  ///< [R]  Matchmaking search result limit
        bool enableVoiceChat = false;      ///< [R]  In-game voice chat
        float voiceChatVolume = 0.8f;      ///< [R]  Voice chat volume (0-1)
        bool pushToTalk = true;            ///< [R]  Push-to-talk vs open mic
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

    CrashReportingSettings& CrashReporting() { return m_crashReporting; }
    const CrashReportingSettings& CrashReporting() const { return m_crashReporting; }

    DebugSettings& Debug() { return m_debug; }
    const DebugSettings& Debug() const { return m_debug; }

    WeatherSettings& Weather() { return m_weather; }
    const WeatherSettings& Weather() const { return m_weather; }

    TimeOfDaySettings& TimeOfDay() { return m_timeOfDay; }
    const TimeOfDaySettings& TimeOfDay() const { return m_timeOfDay; }

    StreamingSettings& Streaming() { return m_streaming; }
    const StreamingSettings& Streaming() const { return m_streaming; }

    PerformanceSettings& Performance() { return m_performance; }
    const PerformanceSettings& Performance() const { return m_performance; }

    WorldSettings& WorldConfig() { return m_world; }
    const WorldSettings& WorldConfig() const { return m_world; }

    UISettings& UI() { return m_ui; }
    const UISettings& UI() const { return m_ui; }

    LoggingSettings& Logging() { return m_logging; }
    const LoggingSettings& Logging() const { return m_logging; }

    AccessibilitySettings& Accessibility() { return m_accessibility; }
    const AccessibilitySettings& Accessibility() const { return m_accessibility; }

    VRSettings& VR() { return m_vr; }
    const VRSettings& VR() const { return m_vr; }

    DestructionSettings& Destruction() { return m_destruction; }
    const DestructionSettings& Destruction() const { return m_destruction; }

    DialogueSettings& Dialogue() { return m_dialogue; }
    const DialogueSettings& Dialogue() const { return m_dialogue; }

    ModdingSettings& Modding() { return m_modding; }
    const ModdingSettings& Modding() const { return m_modding; }

    LocalizationSettings& Localization() { return m_localization; }
    const LocalizationSettings& Localization() const { return m_localization; }

    SaveSystemSettings& SaveSystem() { return m_saveSystem; }
    const SaveSystemSettings& SaveSystem() const { return m_saveSystem; }

    ReplaySettings& Replay() { return m_replay; }
    const ReplaySettings& Replay() const { return m_replay; }

    PersistenceSettings& Persistence() { return m_persistence; }
    const PersistenceSettings& Persistence() const { return m_persistence; }

    ParticleSettings& Particles() { return m_particles; }
    const ParticleSettings& Particles() const { return m_particles; }

    DecalSettings& Decals() { return m_decals; }
    const DecalSettings& Decals() const { return m_decals; }

    MemorySettings& Memory() { return m_memory; }
    const MemorySettings& Memory() const { return m_memory; }

    OnlineServicesSettings& OnlineServices() { return m_onlineServices; }
    const OnlineServicesSettings& OnlineServices() const { return m_onlineServices; }

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

    /// Apply debug settings (assertion suppression) to the Assert system.
    void ApplyDebugSettings();

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
    CrashReportingSettings m_crashReporting;
    DebugSettings m_debug;
    WeatherSettings m_weather;
    TimeOfDaySettings m_timeOfDay;
    StreamingSettings m_streaming;
    PerformanceSettings m_performance;
    WorldSettings m_world;
    UISettings m_ui;
    LoggingSettings m_logging;
    AccessibilitySettings m_accessibility;
    VRSettings m_vr;
    DestructionSettings m_destruction;
    DialogueSettings m_dialogue;
    ModdingSettings m_modding;
    LocalizationSettings m_localization;
    SaveSystemSettings m_saveSystem;
    ReplaySettings m_replay;
    PersistenceSettings m_persistence;
    ParticleSettings m_particles;
    DecalSettings m_decals;
    MemorySettings m_memory;
    OnlineServicesSettings m_onlineServices;

    // Change listeners
    std::vector<SettingsChangedCallback> m_changeCallbacks;
};
