/**
 * @file EngineSettings.cpp
 * @brief Implementation of centralized engine settings
 */

#include "EngineSettings.h"
#include "Reflection.h"
#include "Utils/Assert.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"
#include "Utils/Validate.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

// ============================================================================
// Reflection registrations for settings structs.
// field.name is used as the INI config key (e.g., "WindowWidth").
// ============================================================================

using GS = EngineSettings::GraphicsSettings;
SPARK_REFLECT_TYPE(GS)
SPARK_REFLECT_FIELD(GS, windowWidth, "WindowWidth")
SPARK_REFLECT_FIELD(GS, windowHeight, "WindowHeight")
SPARK_REFLECT_FIELD(GS, fullscreen, "Fullscreen")
SPARK_REFLECT_FIELD(GS, vsync, "VSync")
SPARK_REFLECT_FIELD(GS, antiAliasing, "AntiAliasing")
SPARK_REFLECT_FIELD(GS, shadowQuality, "ShadowQuality")
SPARK_REFLECT_FIELD(GS, renderScale, "RenderScale")
SPARK_REFLECT_FIELD(GS, hdr, "HDR")
SPARK_REFLECT_FIELD(GS, refreshRate, "RefreshRate")
SPARK_REFLECT_FIELD(GS, borderlessWindowed, "BorderlessWindowed")
SPARK_REFLECT_FIELD(GS, monitor, "Monitor")
SPARK_REFLECT_FIELD(GS, tripleBuffering, "TripleBuffering")
SPARK_REFLECT_FIELD(GS, maxFrameLatency, "MaxFrameLatency")
SPARK_REFLECT_END(GS)

using AS = EngineSettings::AudioSettings;
SPARK_REFLECT_TYPE(AS)
SPARK_REFLECT_FIELD(AS, masterVolume, "MasterVolume")
SPARK_REFLECT_FIELD(AS, sfxVolume, "SFXVolume")
SPARK_REFLECT_FIELD(AS, musicVolume, "MusicVolume")
SPARK_REFLECT_FIELD(AS, voiceVolume, "VoiceVolume")
SPARK_REFLECT_FIELD(AS, ambienceVolume, "AmbienceVolume")
SPARK_REFLECT_FIELD(AS, muteOnFocusLoss, "MuteOnFocusLoss")
SPARK_REFLECT_FIELD(AS, muteAll, "MuteAll")
SPARK_REFLECT_END(AS)

using GameS = EngineSettings::GameSettings;
SPARK_REFLECT_TYPE(GameS)
SPARK_REFLECT_FIELD(GameS, difficulty, "Difficulty")
SPARK_REFLECT_FIELD(GameS, showFPS, "ShowFPS")
SPARK_REFLECT_FIELD(GameS, showDebugInfo, "ShowDebugInfo")
SPARK_REFLECT_FIELD(GameS, fieldOfView, "FieldOfView")
SPARK_REFLECT_FIELD(GameS, language, "Language")
SPARK_REFLECT_FIELD(GameS, pauseOnFocusLoss, "PauseOnFocusLoss")
SPARK_REFLECT_END(GameS)

using PS = EngineSettings::PhysicsSettings;
SPARK_REFLECT_TYPE(PS)
SPARK_REFLECT_FIELD(PS, gravityX, "GravityX")
SPARK_REFLECT_FIELD(PS, gravityY, "GravityY")
SPARK_REFLECT_FIELD(PS, gravityZ, "GravityZ")
SPARK_REFLECT_FIELD(PS, fixedTimestep, "FixedTimestep")
SPARK_REFLECT_FIELD(PS, maxSubSteps, "MaxSubSteps")
SPARK_REFLECT_FIELD(PS, defaultFriction, "DefaultFriction")
SPARK_REFLECT_FIELD(PS, defaultRestitution, "DefaultRestitution")
SPARK_REFLECT_FIELD(PS, defaultLinearDamping, "DefaultLinearDamping")
SPARK_REFLECT_FIELD(PS, defaultAngularDamping, "DefaultAngularDamping")
SPARK_REFLECT_FIELD(PS, debugDraw, "DebugDraw")
SPARK_REFLECT_END(PS)

using CamS = EngineSettings::CameraSettings;
SPARK_REFLECT_TYPE(CamS)
SPARK_REFLECT_FIELD(CamS, moveSpeed, "MoveSpeed")
SPARK_REFLECT_FIELD(CamS, rotationSpeed, "RotationSpeed")
SPARK_REFLECT_FIELD(CamS, defaultFov, "DefaultFov")
SPARK_REFLECT_FIELD(CamS, zoomedFov, "ZoomedFov")
SPARK_REFLECT_FIELD(CamS, smoothMovement, "SmoothMovement")
SPARK_REFLECT_FIELD(CamS, nearPlane, "NearPlane")
SPARK_REFLECT_FIELD(CamS, farPlane, "FarPlane")
SPARK_REFLECT_END(CamS)

using CS = EngineSettings::ControlsSettings;
SPARK_REFLECT_TYPE(CS)
SPARK_REFLECT_FIELD(CS, mouseSensitivity, "MouseSensitivity")
SPARK_REFLECT_FIELD(CS, invertMouseY, "InvertMouse") // config key differs from field name
SPARK_REFLECT_FIELD(CS, mouseDeadZone, "MouseDeadZone")
SPARK_REFLECT_FIELD(CS, rawMouseInput, "RawMouseInput")
SPARK_REFLECT_FIELD(CS, mouseAcceleration, "MouseAcceleration")
SPARK_REFLECT_FIELD(CS, controllerDeadZoneLeft, "ControllerDeadZoneLeft")
SPARK_REFLECT_FIELD(CS, controllerDeadZoneRight, "ControllerDeadZoneRight")
SPARK_REFLECT_FIELD(CS, controllerSensitivity, "ControllerSensitivity")
SPARK_REFLECT_FIELD(CS, controllerVibration, "ControllerVibration")
SPARK_REFLECT_FIELD(CS, invertControllerY, "InvertControllerY")
SPARK_REFLECT_END(CS)

using RS = EngineSettings::RenderingSettings;
SPARK_REFLECT_TYPE(RS)
SPARK_REFLECT_FIELD(RS, renderPath, "RenderPath")
SPARK_REFLECT_FIELD(RS, qualityPreset, "QualityPreset")
SPARK_REFLECT_FIELD(RS, maxTextureSize, "MaxTextureSize")
SPARK_REFLECT_FIELD(RS, anisotropicFiltering, "AnisotropicFiltering")
SPARK_REFLECT_FIELD(RS, anisotropyLevel, "AnisotropyLevel")
SPARK_REFLECT_FIELD(RS, shadows, "Shadows")
SPARK_REFLECT_FIELD(RS, shadowMapSize, "ShadowMapSize")
SPARK_REFLECT_FIELD(RS, cascadeCount, "CascadeCount")
SPARK_REFLECT_FIELD(RS, bloom, "Bloom")
SPARK_REFLECT_FIELD(RS, ssao, "SSAO")
SPARK_REFLECT_FIELD(RS, taa, "TAA")
SPARK_REFLECT_FIELD(RS, motionBlur, "MotionBlur")
SPARK_REFLECT_FIELD(RS, frustumCulling, "FrustumCulling")
SPARK_REFLECT_FIELD(RS, occlusionCulling, "OcclusionCulling")
SPARK_REFLECT_FIELD(RS, portalCulling, "PortalCulling")
SPARK_REFLECT_FIELD(RS, levelOfDetail, "LevelOfDetail")
SPARK_REFLECT_FIELD(RS, maxDrawCalls, "MaxDrawCalls")
SPARK_REFLECT_FIELD(RS, wireframeMode, "WireframeMode")
SPARK_REFLECT_FIELD(RS, debugMode, "DebugMode")
SPARK_REFLECT_FIELD(RS, enableGPUTiming, "EnableGPUTiming")
SPARK_REFLECT_END(RS)

using PPS = EngineSettings::PostProcessSettings;
SPARK_REFLECT_TYPE(PPS)
SPARK_REFLECT_FIELD(PPS, bloomEnabled, "BloomEnabled")
SPARK_REFLECT_FIELD(PPS, bloomThreshold, "BloomThreshold")
SPARK_REFLECT_FIELD(PPS, bloomIntensity, "BloomIntensity")
SPARK_REFLECT_FIELD(PPS, bloomRadius, "BloomRadius")
SPARK_REFLECT_FIELD(PPS, bloomSoftKnee, "BloomSoftKnee")
SPARK_REFLECT_FIELD(PPS, bloomIterations, "BloomIterations")
SPARK_REFLECT_FIELD(PPS, toneMappingOperator, "ToneMappingOperator")
SPARK_REFLECT_FIELD(PPS, exposure, "Exposure")
SPARK_REFLECT_FIELD(PPS, gamma, "Gamma")
SPARK_REFLECT_FIELD(PPS, whitePoint, "WhitePoint")
SPARK_REFLECT_FIELD(PPS, colorGradingEnabled, "ColorGradingEnabled")
SPARK_REFLECT_FIELD(PPS, temperature, "Temperature")
SPARK_REFLECT_FIELD(PPS, tint, "Tint")
SPARK_REFLECT_FIELD(PPS, contrast, "Contrast")
SPARK_REFLECT_FIELD(PPS, brightness, "Brightness")
SPARK_REFLECT_FIELD(PPS, saturation, "Saturation")
SPARK_REFLECT_END(PPS)

using SSAOSet = EngineSettings::SSAOSettings;
SPARK_REFLECT_TYPE(SSAOSet)
SPARK_REFLECT_FIELD(SSAOSet, enabled, "Enabled")
SPARK_REFLECT_FIELD(SSAOSet, radius, "Radius")
SPARK_REFLECT_FIELD(SSAOSet, intensity, "Intensity")
SPARK_REFLECT_FIELD(SSAOSet, sampleCount, "SampleCount")
SPARK_REFLECT_FIELD(SSAOSet, bias, "Bias")
SPARK_REFLECT_FIELD(SSAOSet, blur, "Blur")
SPARK_REFLECT_END(SSAOSet)

using AIS = EngineSettings::AISettings;
SPARK_REFLECT_TYPE(AIS)
SPARK_REFLECT_FIELD(AIS, detectionRange, "DetectionRange")
SPARK_REFLECT_FIELD(AIS, attackRange, "AttackRange")
SPARK_REFLECT_FIELD(AIS, meleeRange, "MeleeRange")
SPARK_REFLECT_FIELD(AIS, moveSpeed, "MoveSpeed")
SPARK_REFLECT_FIELD(AIS, turnSpeed, "TurnSpeed")
SPARK_REFLECT_FIELD(AIS, accuracy, "Accuracy")
SPARK_REFLECT_FIELD(AIS, reactionTime, "ReactionTime")
SPARK_REFLECT_FIELD(AIS, coverSearchRadius, "CoverSearchRadius")
SPARK_REFLECT_FIELD(AIS, canStrafe, "CanStrafe")
SPARK_REFLECT_FIELD(AIS, canSprint, "CanSprint")
SPARK_REFLECT_FIELD(AIS, canUseCover, "CanUseCover")
SPARK_REFLECT_END(AIS)

using PlS = EngineSettings::PlayerSettings;
SPARK_REFLECT_TYPE(PlS)
SPARK_REFLECT_FIELD(PlS, maxHealth, "MaxHealth")
SPARK_REFLECT_FIELD(PlS, maxArmor, "MaxArmor")
SPARK_REFLECT_FIELD(PlS, moveSpeed, "MoveSpeed")
SPARK_REFLECT_FIELD(PlS, jumpHeight, "JumpHeight")
SPARK_REFLECT_FIELD(PlS, gravityForce, "GravityForce")
SPARK_REFLECT_FIELD(PlS, friction, "Friction")
SPARK_REFLECT_FIELD(PlS, sprintMultiplier, "SprintMultiplier")
SPARK_REFLECT_FIELD(PlS, crouchMultiplier, "CrouchMultiplier")
SPARK_REFLECT_FIELD(PlS, adsSpeedMultiplier, "ADSSpeedMultiplier") // config key differs
SPARK_REFLECT_FIELD(PlS, maxShield, "MaxShield")
SPARK_REFLECT_FIELD(PlS, shieldRechargeRate, "ShieldRechargeRate")
SPARK_REFLECT_FIELD(PlS, shieldRechargeDelay, "ShieldRechargeDelay")
SPARK_REFLECT_FIELD(PlS, maxEnergy, "MaxEnergy")
SPARK_REFLECT_FIELD(PlS, energyRegenRate, "EnergyRegenRate")
SPARK_REFLECT_END(PlS)

using ES = EngineSettings::EditorSettings;
SPARK_REFLECT_TYPE(ES)
SPARK_REFLECT_FIELD(ES, gridSize, "GridSize")
SPARK_REFLECT_FIELD(ES, snapToGrid, "SnapToGrid")
SPARK_REFLECT_FIELD(ES, showGrid, "ShowGrid")
SPARK_REFLECT_FIELD(ES, gizmoScale, "GizmoScale")
SPARK_REFLECT_FIELD(ES, autosaveEnabled, "AutosaveEnabled")
SPARK_REFLECT_FIELD(ES, autosaveIntervalSeconds, "AutosaveIntervalSeconds")
SPARK_REFLECT_FIELD(ES, undoHistorySize, "UndoHistorySize")
SPARK_REFLECT_FIELD(ES, rotationSnap, "RotationSnap")
SPARK_REFLECT_FIELD(ES, scaleSnap, "ScaleSnap")
SPARK_REFLECT_FIELD(ES, showGizmoLabels, "ShowGizmoLabels")
SPARK_REFLECT_FIELD(ES, showWireframeOverlay, "ShowWireframeOverlay")
SPARK_REFLECT_FIELD(ES, showBounds, "ShowBounds")
SPARK_REFLECT_FIELD(ES, showColliders, "ShowColliders")
SPARK_REFLECT_FIELD(ES, showNavMesh, "ShowNavMesh")
SPARK_REFLECT_FIELD(ES, showLightRadius, "ShowLightRadius")
SPARK_REFLECT_FIELD(ES, cameraSpeedMultiplier, "CameraSpeedMultiplier")
SPARK_REFLECT_FIELD(ES, recentFilesMax, "RecentFilesMax")
SPARK_REFLECT_FIELD(ES, enableCollaboration, "EnableCollaboration")
SPARK_REFLECT_END(ES)

using NS = EngineSettings::NetworkSettings;
SPARK_REFLECT_TYPE(NS)
SPARK_REFLECT_FIELD(NS, serverPort, "ServerPort")
SPARK_REFLECT_FIELD(NS, maxClients, "MaxClients")
SPARK_REFLECT_FIELD(NS, connectionTimeout, "ConnectionTimeout")
SPARK_REFLECT_FIELD(NS, heartbeatInterval, "HeartbeatInterval")
SPARK_REFLECT_FIELD(NS, replicationRate, "ReplicationRate")
SPARK_REFLECT_FIELD(NS, reliableRetransmitBase, "ReliableRetransmitBase")
SPARK_REFLECT_FIELD(NS, maxReliableRetries, "MaxReliableRetries")
SPARK_REFLECT_FIELD(NS, sendBufferSize, "SendBufferSize")
SPARK_REFLECT_FIELD(NS, receiveBufferSize, "ReceiveBufferSize")
SPARK_REFLECT_FIELD(NS, enableCompression, "EnableCompression")
SPARK_REFLECT_FIELD(NS, enableEncryption, "EnableEncryption")
SPARK_REFLECT_FIELD(NS, simulatedLatencyMs, "SimulatedLatencyMs")
SPARK_REFLECT_FIELD(NS, simulatedPacketLoss, "SimulatedPacketLoss")
SPARK_REFLECT_FIELD(NS, simulatedJitterMs, "SimulatedJitterMs")
SPARK_REFLECT_END(NS)

using DS = EngineSettings::DebugSettings;
SPARK_REFLECT_TYPE(DS)
SPARK_REFLECT_FIELD(DS, suppressFatalAsserts, "SuppressFatalAsserts")
SPARK_REFLECT_FIELD(DS, breakOnSuppressedAsserts, "BreakOnSuppressedAsserts")
SPARK_REFLECT_END(DS)

// --- Batch 2: remaining settings groups ---

using SSRS = EngineSettings::SSRSettings;
SPARK_REFLECT_TYPE(SSRS)
SPARK_REFLECT_FIELD(SSRS, enabled, "Enabled")
SPARK_REFLECT_FIELD(SSRS, maxDistance, "MaxDistance")
SPARK_REFLECT_FIELD(SSRS, maxSteps, "MaxSteps")
SPARK_REFLECT_FIELD(SSRS, thickness, "Thickness")
SPARK_REFLECT_FIELD(SSRS, fadeStart, "FadeStart")
SPARK_REFLECT_FIELD(SSRS, fadeEnd, "FadeEnd")
SPARK_REFLECT_END(SSRS)

using VolS = EngineSettings::VolumetricSettings;
SPARK_REFLECT_TYPE(VolS)
SPARK_REFLECT_FIELD(VolS, enabled, "Enabled")
SPARK_REFLECT_FIELD(VolS, sampleCount, "SampleCount")
SPARK_REFLECT_FIELD(VolS, scattering, "Scattering")
SPARK_REFLECT_FIELD(VolS, extinction, "Extinction")
SPARK_REFLECT_FIELD(VolS, anisotropy, "Anisotropy")
SPARK_REFLECT_END(VolS)

using TAAS = EngineSettings::TAASettings;
SPARK_REFLECT_TYPE(TAAS)
SPARK_REFLECT_FIELD(TAAS, enabled, "Enabled")
SPARK_REFLECT_FIELD(TAAS, quality, "Quality")
SPARK_REFLECT_FIELD(TAAS, jitterPattern, "JitterPattern")
SPARK_REFLECT_FIELD(TAAS, jitterSequenceLength, "JitterSequenceLength")
SPARK_REFLECT_FIELD(TAAS, historyBlendFactor, "HistoryBlendFactor")
SPARK_REFLECT_FIELD(TAAS, varianceClipGamma, "VarianceClipGamma")
SPARK_REFLECT_FIELD(TAAS, useMotionVectors, "UseMotionVectors")
SPARK_REFLECT_FIELD(TAAS, useYCoCg, "UseYCoCg")
SPARK_REFLECT_FIELD(TAAS, sharpness, "Sharpness")
SPARK_REFLECT_FIELD(TAAS, ghostingRejectionStrength, "GhostingRejectionStrength")
SPARK_REFLECT_FIELD(TAAS, flickerReduction, "FlickerReduction")
SPARK_REFLECT_END(TAAS)

using MBS = EngineSettings::MotionBlurSettings;
SPARK_REFLECT_TYPE(MBS)
SPARK_REFLECT_FIELD(MBS, enabled, "Enabled")
SPARK_REFLECT_FIELD(MBS, type, "Type")
SPARK_REFLECT_FIELD(MBS, intensity, "Intensity")
SPARK_REFLECT_FIELD(MBS, sampleCount, "SampleCount")
SPARK_REFLECT_FIELD(MBS, maxBlurRadius, "MaxBlurRadius")
SPARK_REFLECT_FIELD(MBS, velocityScale, "VelocityScale")
SPARK_REFLECT_FIELD(MBS, minVelocityThreshold, "MinVelocityThreshold")
SPARK_REFLECT_FIELD(MBS, cameraRotationScale, "CameraRotationScale")
SPARK_REFLECT_FIELD(MBS, cameraTranslationScale, "CameraTranslationScale")
SPARK_REFLECT_FIELD(MBS, tileSize, "TileSize")
SPARK_REFLECT_END(MBS)

using DQS = EngineSettings::DynamicQualitySettings;
SPARK_REFLECT_TYPE(DQS)
SPARK_REFLECT_FIELD(DQS, enabled, "Enabled")
SPARK_REFLECT_FIELD(DQS, targetFrameTimeMs, "TargetFrameTimeMs")
SPARK_REFLECT_FIELD(DQS, minRenderScale, "MinRenderScale")
SPARK_REFLECT_FIELD(DQS, maxRenderScale, "MaxRenderScale")
SPARK_REFLECT_FIELD(DQS, renderScaleStep, "RenderScaleStep")
SPARK_REFLECT_FIELD(DQS, minShadowScale, "MinShadowScale")
SPARK_REFLECT_FIELD(DQS, maxShadowScale, "MaxShadowScale")
SPARK_REFLECT_FIELD(DQS, shadowScaleStep, "ShadowScaleStep")
SPARK_REFLECT_FIELD(DQS, minLodBias, "MinLodBias")
SPARK_REFLECT_FIELD(DQS, maxLodBias, "MaxLodBias")
SPARK_REFLECT_FIELD(DQS, lodBiasStep, "LodBiasStep")
SPARK_REFLECT_FIELD(DQS, minTextureMipBias, "MinTextureMipBias")
SPARK_REFLECT_FIELD(DQS, maxTextureMipBias, "MaxTextureMipBias")
SPARK_REFLECT_FIELD(DQS, textureMipBiasStep, "TextureMipBiasStep")
SPARK_REFLECT_FIELD(DQS, frameTimeWindowSize, "FrameTimeWindowSize")
SPARK_REFLECT_FIELD(DQS, minChangeIntervalFrames, "MinChangeIntervalFrames")
SPARK_REFLECT_FIELD(DQS, increaseThresholdMs, "IncreaseThresholdMs")
SPARK_REFLECT_FIELD(DQS, pidKP, "PID_KP")
SPARK_REFLECT_FIELD(DQS, pidKI, "PID_KI")
SPARK_REFLECT_FIELD(DQS, pidKD, "PID_KD")
SPARK_REFLECT_END(DQS)

using AES = EngineSettings::AudioExtendedSettings;
SPARK_REFLECT_TYPE(AES)
SPARK_REFLECT_FIELD(AES, dopplerScale, "DopplerScale")
SPARK_REFLECT_FIELD(AES, distanceScale, "DistanceScale")
SPARK_REFLECT_FIELD(AES, enable3D, "Enable3D")
SPARK_REFLECT_FIELD(AES, enableReverb, "EnableReverb")
SPARK_REFLECT_FIELD(AES, enableEAX, "EnableEAX")
SPARK_REFLECT_FIELD(AES, maxSources, "MaxSources")
SPARK_REFLECT_END(AES)

using GMS = EngineSettings::GameModeSettings;
SPARK_REFLECT_TYPE(GMS)
SPARK_REFLECT_FIELD(GMS, scoreLimit, "ScoreLimit")
SPARK_REFLECT_FIELD(GMS, roundLimit, "RoundLimit")
SPARK_REFLECT_FIELD(GMS, timeLimit, "TimeLimit")
SPARK_REFLECT_FIELD(GMS, respawnDelay, "RespawnDelay")
SPARK_REFLECT_FIELD(GMS, autoRespawn, "AutoRespawn")
SPARK_REFLECT_FIELD(GMS, maxLives, "MaxLives")
SPARK_REFLECT_FIELD(GMS, damageMultiplier, "DamageMultiplier")
SPARK_REFLECT_FIELD(GMS, healthMultiplier, "HealthMultiplier")
SPARK_REFLECT_FIELD(GMS, speedMultiplier, "SpeedMultiplier")
SPARK_REFLECT_FIELD(GMS, friendlyFire, "FriendlyFire")
SPARK_REFLECT_FIELD(GMS, headshots, "Headshots")
SPARK_REFLECT_FIELD(GMS, headshotMultiplier, "HeadshotMultiplier")
SPARK_REFLECT_FIELD(GMS, allWeaponsAvailable, "AllWeaponsAvailable")
SPARK_REFLECT_FIELD(GMS, teamsEnabled, "TeamsEnabled")
SPARK_REFLECT_FIELD(GMS, maxTeamSize, "MaxTeamSize")
SPARK_REFLECT_FIELD(GMS, autoBalance, "AutoBalance")
SPARK_REFLECT_FIELD(GMS, killPoints, "KillPoints")
SPARK_REFLECT_FIELD(GMS, deathPenalty, "DeathPenalty")
SPARK_REFLECT_FIELD(GMS, assistPoints, "AssistPoints")
SPARK_REFLECT_FIELD(GMS, objectivePoints, "ObjectivePoints")
SPARK_REFLECT_FIELD(GMS, headshotBonus, "HeadshotBonus")
SPARK_REFLECT_END(GMS)

using ScrS = EngineSettings::ScriptingSettings;
SPARK_REFLECT_TYPE(ScrS)
SPARK_REFLECT_FIELD(ScrS, hotReloadEnabled, "HotReloadEnabled")
SPARK_REFLECT_FIELD(ScrS, hotReloadPollInterval, "HotReloadPollInterval")
SPARK_REFLECT_FIELD(ScrS, contextPoolSize, "ContextPoolSize")
SPARK_REFLECT_FIELD(ScrS, executionTimeoutMs, "ExecutionTimeoutMs")
SPARK_REFLECT_FIELD(ScrS, generateDebugInfo, "GenerateDebugInfo")
SPARK_REFLECT_FIELD(ScrS, maxCallStackDepth, "MaxCallStackDepth")
SPARK_REFLECT_FIELD(ScrS, maxScriptMemoryMB, "MaxScriptMemoryMB")
SPARK_REFLECT_FIELD(ScrS, enableProfiler, "EnableProfiler")
SPARK_REFLECT_END(ScrS)

using AnimS = EngineSettings::AnimationSettings;
SPARK_REFLECT_TYPE(AnimS)
SPARK_REFLECT_FIELD(AnimS, defaultBlendTime, "DefaultBlendTime")
SPARK_REFLECT_FIELD(AnimS, ikSolverIterations, "IKSolverIterations")
SPARK_REFLECT_FIELD(AnimS, ikTolerance, "IKTolerance")
SPARK_REFLECT_FIELD(AnimS, maxActiveMontages, "MaxActiveMontages")
SPARK_REFLECT_FIELD(AnimS, enableRootMotion, "EnableRootMotion")
SPARK_REFLECT_FIELD(AnimS, lodDistanceMultiplier, "LODDistanceMultiplier")
SPARK_REFLECT_FIELD(AnimS, compressionQuality, "CompressionQuality")
SPARK_REFLECT_FIELD(AnimS, enableAnimationEvents, "EnableAnimationEvents")
SPARK_REFLECT_END(AnimS)

using CRS = EngineSettings::CrashReportingSettings;
SPARK_REFLECT_TYPE(CRS)
SPARK_REFLECT_FIELD(CRS, enabled, "Enabled")
SPARK_REFLECT_FIELD(CRS, requireConsent, "RequireConsent")
SPARK_REFLECT_FIELD(CRS, headlessMode, "HeadlessMode")
SPARK_REFLECT_FIELD(CRS, promptUserDescription, "PromptUserDescription")
SPARK_REFLECT_FIELD(CRS, allowScreenshotRefusal, "AllowScreenshotRefusal")
SPARK_REFLECT_FIELD(CRS, uploadURL, "UploadURL")
SPARK_REFLECT_FIELD(CRS, proxyURL, "ProxyURL")
SPARK_REFLECT_FIELD(CRS, githubRepo, "GithubRepo")
SPARK_REFLECT_FIELD(CRS, githubToken, "GithubToken")
SPARK_REFLECT_FIELD(CRS, githubLabels, "GithubLabels")
SPARK_REFLECT_FIELD(CRS, attachDump, "AttachDump")
SPARK_REFLECT_FIELD(CRS, captureScreenshot, "CaptureScreenshot")
SPARK_REFLECT_FIELD(CRS, captureSystemInfo, "CaptureSystemInfo")
SPARK_REFLECT_FIELD(CRS, captureAllThreads, "CaptureAllThreads")
SPARK_REFLECT_FIELD(CRS, timeoutSeconds, "TimeoutSeconds")
SPARK_REFLECT_FIELD(CRS, smtpUser, "SmtpUser")
SPARK_REFLECT_FIELD(CRS, smtpPass, "SmtpPass")
SPARK_REFLECT_FIELD(CRS, emailTo, "EmailTo")
SPARK_REFLECT_FIELD(CRS, emailFrom, "EmailFrom")
SPARK_REFLECT_END(CRS)

using WthS = EngineSettings::WeatherSettings;
SPARK_REFLECT_TYPE(WthS)
SPARK_REFLECT_FIELD(WthS, weatherType, "WeatherType")
SPARK_REFLECT_FIELD(WthS, intensity, "Intensity")
SPARK_REFLECT_FIELD(WthS, transitionTime, "TransitionTime")
SPARK_REFLECT_FIELD(WthS, windSpeed, "WindSpeed")
SPARK_REFLECT_FIELD(WthS, windDirectionX, "WindDirectionX")
SPARK_REFLECT_FIELD(WthS, windDirectionY, "WindDirectionY")
SPARK_REFLECT_FIELD(WthS, windDirectionZ, "WindDirectionZ")
SPARK_REFLECT_FIELD(WthS, windGustiness, "WindGustiness")
SPARK_REFLECT_FIELD(WthS, fogDensity, "FogDensity")
SPARK_REFLECT_FIELD(WthS, fogStartDistance, "FogStartDistance")
SPARK_REFLECT_FIELD(WthS, fogEndDistance, "FogEndDistance")
SPARK_REFLECT_FIELD(WthS, precipitationRate, "PrecipitationRate")
SPARK_REFLECT_FIELD(WthS, precipitationSize, "PrecipitationSize")
SPARK_REFLECT_FIELD(WthS, lightningFrequency, "LightningFrequency")
SPARK_REFLECT_FIELD(WthS, thunderDelay, "ThunderDelay")
SPARK_REFLECT_FIELD(WthS, ambientMultiplier, "AmbientMultiplier")
SPARK_REFLECT_FIELD(WthS, directionalMultiplier, "DirectionalMultiplier")
SPARK_REFLECT_END(WthS)

using TODS = EngineSettings::TimeOfDaySettings;
SPARK_REFLECT_TYPE(TODS)
SPARK_REFLECT_FIELD(TODS, startHour, "StartHour")
SPARK_REFLECT_FIELD(TODS, timeScale, "TimeScale")
SPARK_REFLECT_FIELD(TODS, paused, "Paused")
SPARK_REFLECT_FIELD(TODS, sunriseHour, "SunriseHour")
SPARK_REFLECT_FIELD(TODS, sunsetHour, "SunsetHour")
SPARK_REFLECT_FIELD(TODS, moonIntensity, "MoonIntensity")
SPARK_REFLECT_FIELD(TODS, ambientNightMultiplier, "AmbientNightMultiplier")
SPARK_REFLECT_END(TODS)

using StrS = EngineSettings::StreamingSettings;
SPARK_REFLECT_TYPE(StrS)
SPARK_REFLECT_FIELD(StrS, drawDistance, "DrawDistance")
SPARK_REFLECT_FIELD(StrS, lodDistanceMultiplier, "LODDistanceMultiplier")
SPARK_REFLECT_FIELD(StrS, streamingDistance, "StreamingDistance")
SPARK_REFLECT_FIELD(StrS, maxLoadedAreas, "MaxLoadedAreas")
SPARK_REFLECT_FIELD(StrS, lodBias, "LODBias")
SPARK_REFLECT_FIELD(StrS, maxConcurrentLoads, "MaxConcurrentLoads")
SPARK_REFLECT_FIELD(StrS, enableStreaming, "EnableStreaming")
SPARK_REFLECT_FIELD(StrS, shadowDrawDistance, "ShadowDrawDistance")
SPARK_REFLECT_END(StrS)

using PerfS = EngineSettings::PerformanceSettings;
SPARK_REFLECT_TYPE(PerfS)
SPARK_REFLECT_FIELD(PerfS, targetFPS, "TargetFPS")
SPARK_REFLECT_FIELD(PerfS, workerThreadCount, "WorkerThreadCount")
SPARK_REFLECT_FIELD(PerfS, enableJobSystem, "EnableJobSystem")
SPARK_REFLECT_FIELD(PerfS, maxParticles, "MaxParticles")
SPARK_REFLECT_FIELD(PerfS, maxDecals, "MaxDecals")
SPARK_REFLECT_FIELD(PerfS, enableAsyncCompute, "EnableAsyncCompute")
SPARK_REFLECT_FIELD(PerfS, enableAsyncLoading, "EnableAsyncLoading")
SPARK_REFLECT_FIELD(PerfS, objectPoolGrowthFactor, "ObjectPoolGrowthFactor")
SPARK_REFLECT_END(PerfS)

using WrldS = EngineSettings::WorldSettings;
SPARK_REFLECT_TYPE(WrldS)
SPARK_REFLECT_FIELD(WrldS, originRebaseThreshold, "OriginRebaseThreshold")
SPARK_REFLECT_FIELD(WrldS, defaultGravityScale, "DefaultGravityScale")
SPARK_REFLECT_FIELD(WrldS, enableOriginRebasing, "EnableOriginRebasing")
SPARK_REFLECT_FIELD(WrldS, worldBoundsMin, "WorldBoundsMin")
SPARK_REFLECT_FIELD(WrldS, worldBoundsMax, "WorldBoundsMax")
SPARK_REFLECT_END(WrldS)

using UIS = EngineSettings::UISettings;
SPARK_REFLECT_TYPE(UIS)
SPARK_REFLECT_FIELD(UIS, uiScale, "UIScale")
SPARK_REFLECT_FIELD(UIS, fontSize, "FontSize")
SPARK_REFLECT_FIELD(UIS, showCrosshair, "ShowCrosshair")
SPARK_REFLECT_FIELD(UIS, showHUD, "ShowHUD")
SPARK_REFLECT_FIELD(UIS, showMinimap, "ShowMinimap")
SPARK_REFLECT_FIELD(UIS, showDamageNumbers, "ShowDamageNumbers")
SPARK_REFLECT_FIELD(UIS, showSubtitles, "ShowSubtitles")
SPARK_REFLECT_FIELD(UIS, hudOpacity, "HUDOpacity")
SPARK_REFLECT_FIELD(UIS, subtitleSize, "SubtitleSize")
SPARK_REFLECT_FIELD(UIS, showInteractionPrompts, "ShowInteractionPrompts")
SPARK_REFLECT_END(UIS)

using AccS = EngineSettings::AccessibilitySettings;
SPARK_REFLECT_TYPE(AccS)
SPARK_REFLECT_FIELD(AccS, colorblindMode, "ColorblindMode")
SPARK_REFLECT_FIELD(AccS, colorblindStrength, "ColorblindStrength")
SPARK_REFLECT_FIELD(AccS, screenReader, "ScreenReader")
SPARK_REFLECT_FIELD(AccS, reduceMotion, "ReduceMotion")
SPARK_REFLECT_FIELD(AccS, highContrast, "HighContrast")
SPARK_REFLECT_FIELD(AccS, textToSpeechRate, "TextToSpeechRate")
SPARK_REFLECT_FIELD(AccS, largeText, "LargeText")
SPARK_REFLECT_FIELD(AccS, closedCaptions, "ClosedCaptions")
SPARK_REFLECT_FIELD(AccS, holdTimeMultiplier, "HoldTimeMultiplier")
SPARK_REFLECT_FIELD(AccS, toggleAim, "ToggleAim")
SPARK_REFLECT_FIELD(AccS, toggleSprint, "ToggleSprint")
SPARK_REFLECT_FIELD(AccS, toggleCrouch, "ToggleCrouch")
SPARK_REFLECT_FIELD(AccS, autoAim, "AutoAim")
SPARK_REFLECT_FIELD(AccS, autoAimStrength, "AutoAimStrength")
SPARK_REFLECT_END(AccS)

using VRS = EngineSettings::VRSettings;
SPARK_REFLECT_TYPE(VRS)
SPARK_REFLECT_FIELD(VRS, enabled, "Enabled")
SPARK_REFLECT_FIELD(VRS, renderTargetWidth, "RenderTargetWidth")
SPARK_REFLECT_FIELD(VRS, renderTargetHeight, "RenderTargetHeight")
SPARK_REFLECT_FIELD(VRS, renderScale, "RenderScale")
SPARK_REFLECT_FIELD(VRS, trackingSpace, "TrackingSpace")
SPARK_REFLECT_FIELD(VRS, headTrackingEnabled, "HeadTrackingEnabled")
SPARK_REFLECT_FIELD(VRS, controllerTrackingEnabled, "ControllerTrackingEnabled")
SPARK_REFLECT_FIELD(VRS, hapticAmplitude, "HapticAmplitude")
SPARK_REFLECT_FIELD(VRS, hapticDuration, "HapticDuration")
SPARK_REFLECT_FIELD(VRS, ipd, "IPD")
SPARK_REFLECT_FIELD(VRS, comfortMode, "ComfortMode")
SPARK_REFLECT_FIELD(VRS, snapTurnAngle, "SnapTurnAngle")
SPARK_REFLECT_FIELD(VRS, reprojection, "Reprojection")
SPARK_REFLECT_END(VRS)

using DestS = EngineSettings::DestructionSettings;
SPARK_REFLECT_TYPE(DestS)
SPARK_REFLECT_FIELD(DestS, debrisLifetime, "DebrisLifetime")
SPARK_REFLECT_FIELD(DestS, damageThreshold, "DamageThreshold")
SPARK_REFLECT_FIELD(DestS, damageMultiplier, "DamageMultiplier")
SPARK_REFLECT_FIELD(DestS, maxDamageStages, "MaxDamageStages")
SPARK_REFLECT_FIELD(DestS, scatterForce, "ScatterForce")
SPARK_REFLECT_FIELD(DestS, maxDebrisPieces, "MaxDebrisPieces")
SPARK_REFLECT_FIELD(DestS, enablePhysicsDebris, "EnablePhysicsDebris")
SPARK_REFLECT_END(DestS)

using DlgS = EngineSettings::DialogueSettings;
SPARK_REFLECT_TYPE(DlgS)
SPARK_REFLECT_FIELD(DlgS, defaultCooldown, "DefaultCooldown")
SPARK_REFLECT_FIELD(DlgS, defaultPriority, "DefaultPriority")
SPARK_REFLECT_FIELD(DlgS, maxRulesPerSignal, "MaxRulesPerSignal")
SPARK_REFLECT_FIELD(DlgS, typingSpeed, "TypingSpeed")
SPARK_REFLECT_FIELD(DlgS, enableBarks, "EnableBarks")
SPARK_REFLECT_FIELD(DlgS, barkRange, "BarkRange")
SPARK_REFLECT_FIELD(DlgS, barkCooldown, "BarkCooldown")
SPARK_REFLECT_END(DlgS)

using ModS = EngineSettings::ModdingSettings;
SPARK_REFLECT_TYPE(ModS)
SPARK_REFLECT_FIELD(ModS, enableModding, "EnableModding")
SPARK_REFLECT_FIELD(ModS, modsDirectory, "ModsDirectory")
SPARK_REFLECT_FIELD(ModS, allowScriptMods, "AllowScriptMods")
SPARK_REFLECT_FIELD(ModS, allowAssetOverrides, "AllowAssetOverrides")
SPARK_REFLECT_FIELD(ModS, maxLoadedMods, "MaxLoadedMods")
SPARK_REFLECT_FIELD(ModS, sandboxMods, "SandboxMods")
SPARK_REFLECT_END(ModS)

using LocS = EngineSettings::LocalizationSettings;
SPARK_REFLECT_TYPE(LocS)
SPARK_REFLECT_FIELD(LocS, defaultLanguage, "DefaultLanguage")
SPARK_REFLECT_FIELD(LocS, fallbackLanguage, "FallbackLanguage")
SPARK_REFLECT_FIELD(LocS, autoDetectLanguage, "AutoDetectLanguage")
SPARK_REFLECT_FIELD(LocS, loadAllLanguages, "LoadAllLanguages")
SPARK_REFLECT_FIELD(LocS, localizationDir, "LocalizationDir")
SPARK_REFLECT_END(LocS)

using SaveS = EngineSettings::SaveSystemSettings;
SPARK_REFLECT_TYPE(SaveS)
SPARK_REFLECT_FIELD(SaveS, savesDirectory, "SavesDirectory")
SPARK_REFLECT_FIELD(SaveS, maxAutoSaveSlots, "MaxAutoSaveSlots")
SPARK_REFLECT_FIELD(SaveS, autoSaveInterval, "AutoSaveInterval")
SPARK_REFLECT_FIELD(SaveS, enableCloudSaves, "EnableCloudSaves")
SPARK_REFLECT_FIELD(SaveS, compressSaves, "CompressSaves")
SPARK_REFLECT_FIELD(SaveS, backupOnSave, "BackupOnSave")
SPARK_REFLECT_FIELD(SaveS, maxManualSaves, "MaxManualSaves")
SPARK_REFLECT_END(SaveS)

using RepS = EngineSettings::ReplaySettings;
SPARK_REFLECT_TYPE(RepS)
SPARK_REFLECT_FIELD(RepS, recordInterval, "RecordInterval")
SPARK_REFLECT_FIELD(RepS, maxFrameCount, "MaxFrameCount")
SPARK_REFLECT_FIELD(RepS, maxEntityCount, "MaxEntityCount")
SPARK_REFLECT_FIELD(RepS, maxEventCount, "MaxEventCount")
SPARK_REFLECT_FIELD(RepS, maxStringLength, "MaxStringLength")
SPARK_REFLECT_FIELD(RepS, replayDirectory, "ReplayDirectory")
SPARK_REFLECT_FIELD(RepS, autoRecord, "AutoRecord")
SPARK_REFLECT_END(RepS)

using PersS = EngineSettings::PersistenceSettings;
SPARK_REFLECT_TYPE(PersS)
SPARK_REFLECT_FIELD(PersS, databasePath, "DatabasePath")
SPARK_REFLECT_FIELD(PersS, connectionPoolSize, "ConnectionPoolSize")
SPARK_REFLECT_FIELD(PersS, workerThreadCount, "WorkerThreadCount")
SPARK_REFLECT_FIELD(PersS, queryTimeoutMs, "QueryTimeoutMs")
SPARK_REFLECT_FIELD(PersS, enableWAL, "EnableWAL")
SPARK_REFLECT_FIELD(PersS, maxRetries, "MaxRetries")
SPARK_REFLECT_END(PersS)

using PartS = EngineSettings::ParticleSettings;
SPARK_REFLECT_TYPE(PartS)
SPARK_REFLECT_FIELD(PartS, maxParticles, "MaxParticles")
SPARK_REFLECT_FIELD(PartS, maxEmitters, "MaxEmitters")
SPARK_REFLECT_FIELD(PartS, simulationRate, "SimulationRate")
SPARK_REFLECT_FIELD(PartS, lodDistanceMultiplier, "LODDistanceMultiplier")
SPARK_REFLECT_FIELD(PartS, gpuParticles, "GPUParticles")
SPARK_REFLECT_FIELD(PartS, globalScale, "GlobalScale")
SPARK_REFLECT_FIELD(PartS, softParticles, "SoftParticles")
SPARK_REFLECT_END(PartS)

using DecS = EngineSettings::DecalSettings;
SPARK_REFLECT_TYPE(DecS)
SPARK_REFLECT_FIELD(DecS, maxDecals, "MaxDecals")
SPARK_REFLECT_FIELD(DecS, defaultLifetime, "DefaultLifetime")
SPARK_REFLECT_FIELD(DecS, fadeTime, "FadeTime")
SPARK_REFLECT_FIELD(DecS, atlasSize, "AtlasSize")
SPARK_REFLECT_FIELD(DecS, enableDecals, "EnableDecals")
SPARK_REFLECT_END(DecS)

using MemS = EngineSettings::MemorySettings;
SPARK_REFLECT_TYPE(MemS)
SPARK_REFLECT_FIELD(MemS, textureStreamingBudgetMB, "TextureStreamingBudgetMB")
SPARK_REFLECT_FIELD(MemS, meshStreamingBudgetMB, "MeshStreamingBudgetMB")
SPARK_REFLECT_FIELD(MemS, audioStreamingBudgetMB, "AudioStreamingBudgetMB")
SPARK_REFLECT_FIELD(MemS, shaderCacheSizeMB, "ShaderCacheSizeMB")
SPARK_REFLECT_FIELD(MemS, enableMemoryTracking, "EnableMemoryTracking")
SPARK_REFLECT_FIELD(MemS, gcInterval, "GCInterval")
SPARK_REFLECT_FIELD(MemS, gcAggressiveness, "GCAggressiveness")
SPARK_REFLECT_END(MemS)

using OLS = EngineSettings::OnlineServicesSettings;
SPARK_REFLECT_TYPE(OLS)
SPARK_REFLECT_FIELD(OLS, platformBackend, "PlatformBackend")
SPARK_REFLECT_FIELD(OLS, enableOnlineServices, "EnableOnlineServices")
SPARK_REFLECT_FIELD(OLS, sessionTimeout, "SessionTimeout")
SPARK_REFLECT_FIELD(OLS, maxSessionSearchResults, "MaxSessionSearchResults")
SPARK_REFLECT_FIELD(OLS, enableVoiceChat, "EnableVoiceChat")
SPARK_REFLECT_FIELD(OLS, voiceChatVolume, "VoiceChatVolume")
SPARK_REFLECT_FIELD(OLS, pushToTalk, "PushToTalk")
SPARK_REFLECT_END(OLS)

// ============================================================================
// Generic reflection-driven config read/write.
// Uses field.name as config key, struct defaults as fallback values.
// ============================================================================

namespace
{

    // The field.size guards below are defence-in-depth for mis-registered reflection
    // metadata. GCC still emits -Wstringop-overflow in the inlined caller for structs
    // whose total size is smaller than Int/Float/String — e.g. DebugSettings is two
    // bools, total size 2, and the Int arm never executes for its fields, but GCC
    // can't prove that statically. The warning is a known false positive.

    template <typename T>
    void ReadReflectedConfig(T& settings, const Spark::ConfigParser& cfg, const std::string& section)
    {
        const auto* typeInfo = Spark::TypeRegistry::Get().FindType(GetTypeId<T>());
        if (!typeInfo)
            return;

        auto* base = reinterpret_cast<char*>(&settings);
        for (const auto& field : typeInfo->fields)
        {
            const std::string& key = field.name; // field.name holds the config key
            auto* dst = base + field.offset;

            switch (field.type)
            {
            case Spark::FieldType::Int:
                if (field.size >= sizeof(int))
                {
                    int current{};
                    std::memcpy(&current, dst, sizeof(int));
                    int value = cfg.GetInt(section, key, current);
                    std::memcpy(dst, &value, sizeof(int));
                }
                break;
            case Spark::FieldType::Float:
                if (field.size >= sizeof(float))
                {
                    float current{};
                    std::memcpy(&current, dst, sizeof(float));
                    float value = cfg.GetFloat(section, key, current);
                    std::memcpy(dst, &value, sizeof(float));
                }
                break;
            case Spark::FieldType::Bool:
                if (field.size >= sizeof(bool))
                {
                    bool current{};
                    std::memcpy(&current, dst, sizeof(bool));
                    bool value = cfg.GetBool(section, key, current);
                    std::memcpy(dst, &value, sizeof(bool));
                }
                break;
            case Spark::FieldType::String:
            {
                if (field.size >= sizeof(std::string))
                {
                    auto* str = reinterpret_cast<std::string*>(dst);
                    *str = cfg.GetString(section, key, *str);
                }
                break;
            }
            default:
                break;
            }
        }
    }

    template <typename T>
    void WriteReflectedConfig(const T& settings, Spark::ConfigParser& cfg, const std::string& section)
    {
        const auto* typeInfo = Spark::TypeRegistry::Get().FindType(GetTypeId<T>());
        if (!typeInfo)
            return;

        const auto* base = reinterpret_cast<const char*>(&settings);
        for (const auto& field : typeInfo->fields)
        {
            const std::string& key = field.name;
            const auto* src = base + field.offset;

            switch (field.type)
            {
            case Spark::FieldType::Int:
                if (field.size >= sizeof(int))
                {
                    int value{};
                    std::memcpy(&value, src, sizeof(int));
                    cfg.SetInt(section, key, value);
                }
                break;
            case Spark::FieldType::Float:
                if (field.size >= sizeof(float))
                {
                    float value{};
                    std::memcpy(&value, src, sizeof(float));
                    cfg.SetFloat(section, key, value);
                }
                break;
            case Spark::FieldType::Bool:
                if (field.size >= sizeof(bool))
                {
                    bool value{};
                    std::memcpy(&value, src, sizeof(bool));
                    cfg.SetBool(section, key, value);
                }
                break;
            case Spark::FieldType::String:
                if (field.size >= sizeof(std::string))
                    cfg.SetString(section, key, *reinterpret_cast<const std::string*>(src));
                break;
            default:
                break;
            }
        }
    }

} // anonymous namespace

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{
    bool SaveConfigAtomically(const Spark::ConfigParser& config, const std::string& path)
    {
        if (path.empty())
            return false;

        const std::filesystem::path target(path);
        std::filesystem::path temporary = target;
        static std::atomic<uint64_t> temporarySerial{0};
#ifdef SPARK_PLATFORM_WINDOWS
        const auto processId = static_cast<uint64_t>(GetCurrentProcessId());
#else
        const auto processId = static_cast<uint64_t>(getpid());
#endif
        temporary += ".tmp." + std::to_string(processId) + "." +
                     std::to_string(temporarySerial.fetch_add(1, std::memory_order_relaxed));

        std::error_code error;
        std::filesystem::remove(temporary, error);
        error.clear();

        if (!config.Save(temporary.string()))
        {
            std::filesystem::remove(temporary, error);
            return false;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        const bool replaced =
            MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
        std::filesystem::rename(temporary, target, error);
        const bool replaced = !error;
#endif
        if (!replaced)
            std::filesystem::remove(temporary, error);
        return replaced;
    }
} // namespace

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
namespace Spark::UserPaths
{
    namespace
    {
        /// Environment-variable lookup that treats an empty value as unset.
        std::filesystem::path EnvironmentDirectory(const char* name)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            // _wgetenv keeps non-ASCII profile directories intact.
            const std::wstring wideName(name, name + std::strlen(name));
            const wchar_t* value = _wgetenv(wideName.c_str());
#else
            const char* value = std::getenv(name);
#endif
            if (!value || *value == 0)
                return {};
            return std::filesystem::path(value);
        }

        constexpr const char* ProductFolderName = "SparkEngine";
    } // namespace

    std::filesystem::path GetUserDataDir()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        const std::filesystem::path root = EnvironmentDirectory("LOCALAPPDATA");
        if (root.empty())
            return {};
        return root / ProductFolderName;
#else
        if (const std::filesystem::path xdg = EnvironmentDirectory("XDG_DATA_HOME"); !xdg.empty())
            return xdg / ProductFolderName;
        const std::filesystem::path home = EnvironmentDirectory("HOME");
        if (home.empty())
            return {};
        return home / ".local" / "share" / ProductFolderName;
#endif
    }

    std::filesystem::path GetUserConfigDir()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        const std::filesystem::path data = GetUserDataDir();
        return data.empty() ? data : data / "Config";
#else
        if (const std::filesystem::path xdg = EnvironmentDirectory("XDG_CONFIG_HOME"); !xdg.empty())
            return xdg / ProductFolderName;
        const std::filesystem::path home = EnvironmentDirectory("HOME");
        if (home.empty())
            return {};
        return home / ".config" / ProductFolderName;
#endif
    }

    std::filesystem::path GetUserSavesDir()
    {
        const std::filesystem::path data = GetUserDataDir();
        return data.empty() ? data : data / "Saves";
    }

    bool IsDirectoryWritable(const std::filesystem::path& directory)
    {
        if (directory.empty())
            return false;

        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (!std::filesystem::is_directory(directory, error) || error)
            return false;

        const std::filesystem::path probe = directory / ".spark-write-probe";
        {
            std::ofstream stream(probe, std::ios::trunc);
            if (!stream)
                return false;
            stream << 'x';
            if (!stream)
                return false;
        }
        std::filesystem::remove(probe, error);
        return true;
    }

    std::filesystem::path NarrowSafeDirectory(const std::filesystem::path& directory)
    {
        if (directory.empty())
            return {};

        // path::string() and path(std::string) use the same narrow code page, so a
        // value that survives the round trip is one every narrow file API reopens
        // as this directory. Anything else has been replaced with '?' characters,
        // which are not legal in a Windows filename.
        const auto roundTrips = [](const std::filesystem::path& candidate)
        {
            try
            {
                return std::filesystem::path(candidate.string()) == candidate;
            }
            catch (const std::exception&)
            {
                return false;
            }
        };

        if (roundTrips(directory))
            return directory;

#ifdef SPARK_PLATFORM_WINDOWS
        // The 8.3 alias is ASCII and names the same directory, but Windows only
        // reports it for a path that exists.
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (!std::filesystem::is_directory(directory, error))
            return {};

        std::wstring shortForm(MAX_PATH, L'\0');
        DWORD length = GetShortPathNameW(directory.c_str(), shortForm.data(), static_cast<DWORD>(shortForm.size()));
        if (length >= shortForm.size())
        {
            shortForm.resize(length);
            length = GetShortPathNameW(directory.c_str(), shortForm.data(), static_cast<DWORD>(shortForm.size()));
        }
        if (length == 0 || length >= shortForm.size())
            return {};
        shortForm.resize(length);

        // 8.3 name generation can be disabled per volume, in which case Windows
        // hands back the long name unchanged and there is nothing to fall back to.
        const std::filesystem::path aliased(shortForm);
        if (roundTrips(aliased))
            return aliased;
#endif
        return {};
    }

    std::string ResolveSaveDirectory()
    {
        constexpr const char* LegacySaveDirectory = "Saves";

        const std::filesystem::path userSaves = NarrowSafeDirectory(GetUserSavesDir());
        if (userSaves.empty())
            return LegacySaveDirectory;

        std::error_code error;
        std::filesystem::create_directories(userSaves, error);

        // One-time migration. The marker means deleting a save does not resurrect
        // it from the legacy tree on the next launch.
        const std::filesystem::path marker = userSaves / ".migrated-from-working-directory";
        if (std::filesystem::exists(marker, error))
            return userSaves.string();

        const std::filesystem::path legacy = std::filesystem::current_path(error) / LegacySaveDirectory;
        if (error || !std::filesystem::is_directory(legacy, error) ||
            std::filesystem::equivalent(legacy, userSaves, error))
        {
            std::ofstream(marker, std::ios::trunc) << "no legacy save directory\n";
            return userSaves.string();
        }

        std::filesystem::copy(legacy, userSaves,
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing,
                              error);
        if (error)
        {
            // Leave the marker off so the next launch retries; the player's saves
            // are still in the legacy directory and nothing has been lost.
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Could not migrate saves from '%s': %s", legacy.string().c_str(),
                           error.message().c_str());
            return userSaves.string();
        }

        SPARK_LOG_INFO(Spark::LogCategory::Core, "Migrated saves from '%s' to the per-user save directory",
                       legacy.string().c_str());
        std::ofstream(marker, std::ios::trunc) << "migrated\n";
        return userSaves.string();
    }
} // namespace Spark::UserPaths

std::string EngineSettings::FindSettingsPath()
{
#ifdef SPARK_PLATFORM_WINDOWS
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
#else
    auto dir = std::filesystem::current_path();
#endif

    // A settings.ini the player already owns always wins: it is the only copy
    // that survives an upgrade or a reinstall.
    const std::filesystem::path userConfigDir = Spark::UserPaths::GetUserConfigDir();
    const std::filesystem::path narrowUserConfigDir = Spark::UserPaths::NarrowSafeDirectory(userConfigDir);
    std::error_code error;
    if (!narrowUserConfigDir.empty() && std::filesystem::exists(narrowUserConfigDir / "settings.ini", error) && !error)
        return (narrowUserConfigDir / "settings.ini").string();

    // Seed the per-user copy from a settings.ini the package shipped read-only.
    // Save() writes wherever this returns, so returning the packaged copy on an
    // installed build (Program Files) loses the player's options on every launch.
    const auto adoptPackagedSettings = [&narrowUserConfigDir](const std::filesystem::path& packaged) -> std::string
    {
        if (narrowUserConfigDir.empty())
            return {};
        std::error_code copyError;
        std::filesystem::create_directories(narrowUserConfigDir, copyError);
        const std::filesystem::path userSettings = narrowUserConfigDir / "settings.ini";
        std::filesystem::copy_file(packaged, userSettings, std::filesystem::copy_options::skip_existing, copyError);
        if (!std::filesystem::exists(userSettings, copyError) || copyError)
            return {};
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Seeded per-user settings from the read-only packaged copy '%s'",
                       packaged.string().c_str());
        return userSettings.string();
    };

    // Check Resources/Config/ first, then one level up (development builds).
    // An in-tree copy only wins while the tree is writable: development and
    // portable installs keep settings beside the binaries, an installed build
    // cannot.
    auto configDir = dir / "Resources" / "Config";
    auto parentConfig = dir.parent_path() / "Resources" / "Config";
    for (const auto& candidate : {configDir, parentConfig})
    {
        const std::filesystem::path settings = candidate / "settings.ini";
        if (!std::filesystem::exists(settings, error) || error)
            continue;
        if (Spark::UserPaths::IsDirectoryWritable(candidate))
            return settings.string();
        if (std::string adopted = adoptPackagedSettings(settings); !adopted.empty())
            return adopted;
        // No per-user location either: return the packaged path so the caller
        // reports one concrete failure instead of writing somewhere unexpected.
        return settings.string();
    }

    // Nothing exists yet.
    if (Spark::UserPaths::IsDirectoryWritable(configDir))
        return (configDir / "settings.ini").string();

    if (!narrowUserConfigDir.empty() && Spark::UserPaths::IsDirectoryWritable(narrowUserConfigDir))
        return (narrowUserConfigDir / "settings.ini").string();

    // Neither location accepted a write. Return the in-tree path so the caller
    // reports one concrete failure instead of writing somewhere unexpected.
    return (configDir / "settings.ini").string();
}

// =============================================================================
// Load
// =============================================================================
bool EngineSettings::Load(const std::string& path)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);

    std::string requestedPath;
    try
    {
        requestedPath = path.empty() ? FindSettingsPath() : path;
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "Failed to resolve engine settings path: %s", error.what());
        return false;
    }

    SPARK_LOG_INFO(Spark::LogCategory::Core, "Loading engine settings from: %s", requestedPath.c_str());

    std::error_code fileError;
    const bool settingsExist = std::filesystem::exists(requestedPath, fileError);
    if (fileError)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "Failed to inspect engine settings file '%s': %s",
                        requestedPath.c_str(), fileError.message().c_str());
        return false;
    }

    EngineSettings staged;
    staged.m_filePath = requestedPath;

    if (settingsExist)
    {
        if (!staged.m_config.Load(requestedPath))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "Failed to read or parse engine settings: %s",
                            requestedPath.c_str());
            return false;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Core, "Engine settings loaded successfully");

        // Load local override file (gitignored — safe for secrets)
        std::string localPath = requestedPath;
        auto dotPos = localPath.rfind('.');
        if (dotPos != std::string::npos)
            localPath = localPath.substr(0, dotPos) + ".local" + localPath.substr(dotPos);
        else
            localPath += ".local";

        fileError.clear();
        const bool localExists = std::filesystem::exists(localPath, fileError);
        if (fileError)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "Failed to inspect local settings override '%s': %s",
                            localPath.c_str(), fileError.message().c_str());
            return false;
        }

        if (localExists)
        {
            Spark::ConfigParser localConfig;
            if (!localConfig.Load(localPath))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "Failed to read or parse local settings override: %s",
                                localPath.c_str());
                return false;
            }

            SPARK_LOG_INFO(Spark::LogCategory::Core, "Loaded local settings override: %s", localPath.c_str());
            // Merge local values into the main config (local takes precedence)
            for (const auto& section : localConfig.GetSections())
            {
                for (const auto& key : localConfig.GetKeys(section))
                {
                    std::string val = localConfig.GetString(section, key, "");
                    if (!val.empty())
                        staged.m_config.SetString(section, key, val);
                }
            }
        }

        staged.ReadFromConfig();
    }
    else
    {
        // Preserve first-run behavior, but only report success when the default
        // file was actually created.
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Settings file not found, creating defaults: %s",
                       requestedPath.c_str());
        staged.PopulateDefaults();
        if (!SaveConfigAtomically(staged.m_config, requestedPath))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "Failed to create default engine settings: %s",
                            requestedPath.c_str());
            return false;
        }
    }

    staged.m_changeCallbacks = m_changeCallbacks;
    *this = std::move(staged);
    ApplyDebugSettings();
    return true;
}

// =============================================================================
// Save
// =============================================================================
bool EngineSettings::Save() const
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);

    SPARK_LOG_INFO(Spark::LogCategory::Core, "Saving engine settings to: %s", m_filePath.c_str());
    EngineSettings staged = *this;
    staged.WriteToConfig();
    const bool result = SaveConfigAtomically(staged.m_config, m_filePath);
    if (!result)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "Failed to save engine settings to: %s", m_filePath.c_str());
        return false;
    }

    m_config = std::move(staged.m_config);
    return true;
}

bool EngineSettings::SaveAs(const std::string& path) const
{
    EngineSettings staged = *this;
    staged.WriteToConfig();
    if (!SaveConfigAtomically(staged.m_config, path))
        return false;

    m_config = std::move(staged.m_config);
    return true;
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
    m_accessibility = AccessibilitySettings{};
    m_vr = VRSettings{};
    m_destruction = DestructionSettings{};
    m_dialogue = DialogueSettings{};
    m_modding = ModdingSettings{};
    m_localization = LocalizationSettings{};
    m_saveSystem = SaveSystemSettings{};
    m_replay = ReplaySettings{};
    m_persistence = PersistenceSettings{};
    m_particles = ParticleSettings{};
    m_decals = DecalSettings{};
    m_memory = MemorySettings{};
    m_onlineServices = OnlineServicesSettings{};
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
    // Graphics — reflection-driven (fields registered in SPARK_REFLECT_TYPE(GS))
    ReadReflectedConfig(m_graphics, m_config, "Graphics");

    // Audio — reflection-driven
    ReadReflectedConfig(m_audio, m_config, "Audio");

    // Controls — reflection-driven
    ReadReflectedConfig(m_controls, m_config, "Controls");

    // Game — reflection-driven
    ReadReflectedConfig(m_game, m_config, "Game");

    // Rendering — reflection-driven
    ReadReflectedConfig(m_rendering, m_config, "Rendering");

    // PostProcess — reflection-driven
    ReadReflectedConfig(m_postProcess, m_config, "PostProcess");

    // SSAO — reflection-driven
    ReadReflectedConfig(m_ssao, m_config, "SSAO");

    // SSR — reflection-driven
    ReadReflectedConfig(m_ssr, m_config, "SSR");

    // Volumetric — reflection-driven
    ReadReflectedConfig(m_volumetric, m_config, "Volumetric");

    // TAA — reflection-driven
    ReadReflectedConfig(m_taa, m_config, "TAA");

    // MotionBlur — reflection-driven
    ReadReflectedConfig(m_motionBlur, m_config, "MotionBlur");

    // DynamicQuality — reflection-driven
    ReadReflectedConfig(m_dynamicQuality, m_config, "DynamicQuality");

    // AudioExtended — reflection-driven
    ReadReflectedConfig(m_audioExtended, m_config, "AudioExtended");

    // Physics — reflection-driven
    ReadReflectedConfig(m_physics, m_config, "Physics");

    // AI — reflection-driven
    ReadReflectedConfig(m_ai, m_config, "AI");

    // Player — reflection-driven
    ReadReflectedConfig(m_player, m_config, "Player");

    // GameMode — reflection-driven
    ReadReflectedConfig(m_gameMode, m_config, "GameMode");

    // Camera — reflection-driven
    ReadReflectedConfig(m_camera, m_config, "Camera");

    // Editor — reflection-driven
    ReadReflectedConfig(m_editor, m_config, "Editor");

    // Network — reflection-driven
    ReadReflectedConfig(m_network, m_config, "Network");

    // Scripting — reflection-driven
    ReadReflectedConfig(m_scripting, m_config, "Scripting");

    // Animation — reflection-driven
    ReadReflectedConfig(m_animation, m_config, "Animation");

    // CrashReporting — reflection-driven
    ReadReflectedConfig(m_crashReporting, m_config, "CrashReporting");

    // Debug — reflection-driven
    ReadReflectedConfig(m_debug, m_config, "Debug");

    // Weather — reflection-driven
    ReadReflectedConfig(m_weather, m_config, "Weather");

    // TimeOfDay — reflection-driven
    ReadReflectedConfig(m_timeOfDay, m_config, "TimeOfDay");

    // Streaming — reflection-driven
    ReadReflectedConfig(m_streaming, m_config, "Streaming");

    // Performance — reflection-driven
    ReadReflectedConfig(m_performance, m_config, "Performance");

    // World — reflection-driven
    ReadReflectedConfig(m_world, m_config, "World");

    // UI — reflection-driven
    ReadReflectedConfig(m_ui, m_config, "UI");

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

    // Accessibility — reflection-driven
    ReadReflectedConfig(m_accessibility, m_config, "Accessibility");

    // VR — reflection-driven
    ReadReflectedConfig(m_vr, m_config, "VR");

    // Destruction — reflection-driven
    ReadReflectedConfig(m_destruction, m_config, "Destruction");

    // Dialogue — reflection-driven
    ReadReflectedConfig(m_dialogue, m_config, "Dialogue");

    // Modding — reflection-driven
    ReadReflectedConfig(m_modding, m_config, "Modding");

    // Localization — reflection-driven
    ReadReflectedConfig(m_localization, m_config, "Localization");

    // SaveSystem — reflection-driven
    ReadReflectedConfig(m_saveSystem, m_config, "SaveSystem");

    // Replay — reflection-driven
    ReadReflectedConfig(m_replay, m_config, "Replay");

    // Persistence — reflection-driven
    ReadReflectedConfig(m_persistence, m_config, "Persistence");

    // Particles — reflection-driven
    ReadReflectedConfig(m_particles, m_config, "Particles");

    // Decals — reflection-driven
    ReadReflectedConfig(m_decals, m_config, "Decals");

    // Memory — reflection-driven
    ReadReflectedConfig(m_memory, m_config, "Memory");

    // OnlineServices — reflection-driven
    ReadReflectedConfig(m_onlineServices, m_config, "OnlineServices");
}

// =============================================================================
// Write struct fields to ConfigParser
// =============================================================================
void EngineSettings::WriteToConfig() const
{
    // Graphics — reflection-driven
    WriteReflectedConfig(m_graphics, m_config, "Graphics");

    // Audio — reflection-driven
    WriteReflectedConfig(m_audio, m_config, "Audio");

    // Controls — reflection-driven
    WriteReflectedConfig(m_controls, m_config, "Controls");

    // Game — reflection-driven
    WriteReflectedConfig(m_game, m_config, "Game");

    // Rendering — reflection-driven
    WriteReflectedConfig(m_rendering, m_config, "Rendering");

    // PostProcess — reflection-driven
    WriteReflectedConfig(m_postProcess, m_config, "PostProcess");

    // SSAO — reflection-driven
    WriteReflectedConfig(m_ssao, m_config, "SSAO");

    // SSR — reflection-driven
    WriteReflectedConfig(m_ssr, m_config, "SSR");

    // Volumetric — reflection-driven
    WriteReflectedConfig(m_volumetric, m_config, "Volumetric");

    // TAA — reflection-driven
    WriteReflectedConfig(m_taa, m_config, "TAA");

    // MotionBlur — reflection-driven
    WriteReflectedConfig(m_motionBlur, m_config, "MotionBlur");

    // DynamicQuality — reflection-driven
    WriteReflectedConfig(m_dynamicQuality, m_config, "DynamicQuality");

    // AudioExtended — reflection-driven
    WriteReflectedConfig(m_audioExtended, m_config, "AudioExtended");

    // Physics — reflection-driven
    WriteReflectedConfig(m_physics, m_config, "Physics");

    // AI — reflection-driven
    WriteReflectedConfig(m_ai, m_config, "AI");

    // Player — reflection-driven
    WriteReflectedConfig(m_player, m_config, "Player");

    // GameMode — reflection-driven
    WriteReflectedConfig(m_gameMode, m_config, "GameMode");

    // Camera — reflection-driven
    WriteReflectedConfig(m_camera, m_config, "Camera");

    // Editor — reflection-driven
    WriteReflectedConfig(m_editor, m_config, "Editor");

    // Network — reflection-driven
    WriteReflectedConfig(m_network, m_config, "Network");

    // Scripting — reflection-driven
    WriteReflectedConfig(m_scripting, m_config, "Scripting");

    // Animation — reflection-driven
    WriteReflectedConfig(m_animation, m_config, "Animation");

    // CrashReporting — reflection-driven
    WriteReflectedConfig(m_crashReporting, m_config, "CrashReporting");

    // Debug — reflection-driven
    WriteReflectedConfig(m_debug, m_config, "Debug");

    // Weather — reflection-driven
    WriteReflectedConfig(m_weather, m_config, "Weather");

    // TimeOfDay — reflection-driven
    WriteReflectedConfig(m_timeOfDay, m_config, "TimeOfDay");

    // Streaming — reflection-driven
    WriteReflectedConfig(m_streaming, m_config, "Streaming");

    // Performance — reflection-driven
    WriteReflectedConfig(m_performance, m_config, "Performance");

    // World — reflection-driven
    WriteReflectedConfig(m_world, m_config, "World");

    // UI — reflection-driven
    WriteReflectedConfig(m_ui, m_config, "UI");

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

    // Accessibility — reflection-driven
    WriteReflectedConfig(m_accessibility, m_config, "Accessibility");

    // VR — reflection-driven
    WriteReflectedConfig(m_vr, m_config, "VR");

    // Destruction — reflection-driven
    WriteReflectedConfig(m_destruction, m_config, "Destruction");

    // Dialogue — reflection-driven
    WriteReflectedConfig(m_dialogue, m_config, "Dialogue");

    // Modding — reflection-driven
    WriteReflectedConfig(m_modding, m_config, "Modding");

    // Localization — reflection-driven
    WriteReflectedConfig(m_localization, m_config, "Localization");

    // SaveSystem — reflection-driven
    WriteReflectedConfig(m_saveSystem, m_config, "SaveSystem");

    // Replay — reflection-driven
    WriteReflectedConfig(m_replay, m_config, "Replay");

    // Persistence — reflection-driven
    WriteReflectedConfig(m_persistence, m_config, "Persistence");

    // Particles — reflection-driven
    WriteReflectedConfig(m_particles, m_config, "Particles");

    // Decals — reflection-driven
    WriteReflectedConfig(m_decals, m_config, "Decals");

    // Memory — reflection-driven
    WriteReflectedConfig(m_memory, m_config, "Memory");

    // OnlineServices — reflection-driven
    WriteReflectedConfig(m_onlineServices, m_config, "OnlineServices");
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
    return {// Display & Window
            "Graphics",
            // Rendering Pipeline
            "Rendering", "PostProcess", "SSAO", "SSR", "Volumetric", "TAA", "MotionBlur", "DynamicQuality",
            // Audio
            "Audio", "AudioExtended",
            // Input & Controls
            "Controls",
            // Game
            "Game", "Player", "GameMode",
            // Camera
            "Camera",
            // Physics
            "Physics",
            // AI
            "AI",
            // Animation
            "Animation",
            // Network & Online
            "Network", "OnlineServices",
            // Scripting
            "Scripting",
            // Environment
            "Weather", "TimeOfDay", "World",
            // Streaming & Performance
            "Streaming", "Performance", "Memory", "Particles", "Decals",
            // Editor
            "Editor",
            // Destruction & Dialogue
            "Destruction", "Dialogue",
            // Modding & Localization
            "Modding", "Localization",
            // Save & Replay
            "SaveSystem", "Replay", "Persistence",
            // Accessibility & VR
            "Accessibility", "VR",
            // UI
            "UI",
            // Diagnostics
            "Logging", "CrashReporting", "Debug"};
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
